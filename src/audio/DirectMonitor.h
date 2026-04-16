#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// DirectMonitor — bypasses Bitwig's output buffer entirely
//
// Path: mic → WASAPI capture → this thread → WASAPI render → headphones
// Latency: ~2ms (USB ADC) + ~10ms (capture period) + ~1.5ms (output) = ~13.5ms
//
// This completely skips:
//   - Bitwig's processing block (~10ms)
//   - Bitwig's output buffer (~10ms)
//
// The capture thread calls write() on every packet.
// This thread reads and pushes to the output device independently.
// processBlock still feeds Bitwig for recording — unchanged.
// ─────────────────────────────────────────────────────────────────────────────
#include "MicLog.h"
#include "AudioRingBuffer.h"
#include "ThreadOptimizer.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

class DirectMonitor
{
public:
    DirectMonitor()  = default;
    ~DirectMonitor() { stop(); }

    // Call after capture is open — needs to know the capture sample rate
    bool start(UINT32 captureSampleRate, UINT32 captureChannels)
    {
        stop();
        m_captureSr  = captureSampleRate;
        m_captureCh  = captureChannels;
        m_ring.resize(captureSampleRate / 2); // 500ms headroom
        m_ring.reset();

        if (!openRender()) return false;

        m_stopFlag.store(false);
        m_running.store(true);
        m_thread = std::thread(&DirectMonitor::monitorThread, this);
        MICLOG("DirectMonitor started: captureSr=" << captureSampleRate
            << " renderSr=" << m_renderSr << " vol=" << m_volume.load());
        return true;
    }

    void stop()
    {
        if (!m_running.load()) return;
        m_stopFlag.store(true);
        if (m_eventHandle) SetEvent(m_eventHandle);
        if (m_thread.joinable()) m_thread.join();
        if (m_renderClient)  { m_renderClient->Release();  m_renderClient  = nullptr; }
        if (m_audioClient)   { m_audioClient->Release();   m_audioClient   = nullptr; }
        if (m_device)        { m_device->Release();         m_device        = nullptr; }
        if (m_eventHandle)   { CloseHandle(m_eventHandle);  m_eventHandle   = nullptr; }
        m_running.store(false);
        MICLOG("DirectMonitor stopped");
    }

    // Called from capture thread — write mic audio into monitor ring
    // captureStereo: interleaved L,R float32 at captureSampleRate
    void write(const float* captureStereo, size_t frames)
    {
        if (!m_running.load(std::memory_order_relaxed)) return;
        if (m_volume.load(std::memory_order_relaxed) < 0.001f) return;

        // Apply volume in-place on a temp buffer
        const size_t samples = frames * 2;
        if (m_volBuf.size() < samples) m_volBuf.resize(samples);
        const float vol = m_volume.load(std::memory_order_relaxed);
        for (size_t i = 0; i < samples; ++i)
            m_volBuf[i] = captureStereo[i] * vol;

        m_ring.write(m_volBuf.data(), frames);
    }

    void setVolume(float v) { m_volume.store(std::max(0.f, std::min(1.f, v))); }
    float getVolume() const { return m_volume.load(); }
    bool isRunning()  const { return m_running.load(); }
    float latencyMs() const { return m_latencyMs.load(); }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lk(m_errMu);
        return m_lastError;
    }

private:
    bool openRender()
    {
        IMMDeviceEnumerator* en = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                       CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
        MICLOG_HR("DirectMonitor: CoCreateInstance", hr);
        if (FAILED(hr)) { setError("CoCreateInstance failed"); return false; }

        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        MICLOG_HR("DirectMonitor: GetDefaultRenderEndpoint", hr);
        en->Release();
        if (FAILED(hr)) { setError("No render device"); return false; }

        // Log output device name
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(m_device->OpenPropertyStore(STGM_READ, &props)))
        {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal)
            {
                std::wstring wn = pv.pwszVal;
                std::string n(wn.begin(), wn.end());
                MICLOG("DirectMonitor: output device = " << n);
            }
            PropVariantClear(&pv); props->Release();
        }

        // Get render mix format
        IAudioClient* temp = nullptr;
        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&temp);
        if (FAILED(hr)) { setError("Activate render failed"); return false; }

        WAVEFORMATEX* wfx = nullptr;
        hr = temp->GetMixFormat(&wfx);
        temp->Release();
        if (FAILED(hr) || !wfx) { setError("GetMixFormat render failed"); return false; }

        m_renderSr  = wfx->nSamplesPerSec;
        m_renderCh  = wfx->nChannels;
        MICLOG("DirectMonitor: render format sr=" << m_renderSr << " ch=" << m_renderCh);

        // Open render client
        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
        if (FAILED(hr)) { CoTaskMemFree(wfx); setError("Activate render2 failed"); return false; }

        m_eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // Use minimum buffer duration for lowest output latency
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        m_audioClient->GetDevicePeriod(&defPeriod, &minPeriod);
        MICLOG("DirectMonitor: render periods def=" << defPeriod/10000.0
            << "ms min=" << minPeriod/10000.0 << "ms");

        hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            defPeriod, 0, wfx, nullptr);
        MICLOG_HR("DirectMonitor: Initialize(render)", hr);

        if (FAILED(hr)) { CoTaskMemFree(wfx); setError("Render Initialize failed"); return false; }

        m_audioClient->SetEventHandle(m_eventHandle);
        m_audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_renderClient);

        REFERENCE_TIME lat = 0;
        m_audioClient->GetStreamLatency(&lat);
        m_audioClient->GetBufferSize(&m_renderBufFrames);
        // Render latency = output period only (we bypass Bitwig block entirely)
        float renderMs = (float)m_renderBufFrames / m_renderSr * 500.f; // half buffer
        m_latencyMs.store(renderMs);

        MICLOG("DirectMonitor: renderBufFrames=" << m_renderBufFrames
            << " renderMs=" << renderMs
            << " streamLatency=" << lat/10000.0 << "ms");

        CoTaskMemFree(wfx);
        return true;
    }

    void monitorThread()
    {
        MICLOG("DirectMonitor::monitorThread started");
        // Apply MMCSS for the monitor thread too
        DWORD taskIndex = 0;
        HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        m_audioClient->Start();

        // Pre-fill render buffer with silence to avoid initial glitch
        {
            UINT32 padFrames = 0;
            m_audioClient->GetCurrentPadding(&padFrames);
            UINT32 toFill = m_renderBufFrames - padFrames;
            BYTE* buf = nullptr;
            if (SUCCEEDED(m_renderClient->GetBuffer(toFill, &buf)) && buf)
            {
                memset(buf, 0, toFill * m_renderCh * sizeof(float));
                m_renderClient->ReleaseBuffer(toFill, 0);
            }
        }

        // Resample ratio: capture→render (usually both 48kHz, ratio=1)
        // If they differ, we do simple linear interpolation
        const double ratio = (double)m_captureSr / m_renderSr;
        std::vector<float> renderBuf;

        while (!m_stopFlag.load(std::memory_order_relaxed))
        {
            DWORD wr = WaitForSingleObject(m_eventHandle, 100);
            if (wr != WAIT_OBJECT_0) continue;
            if (m_stopFlag.load(std::memory_order_relaxed)) break;

            // How many frames does the render device need?
            UINT32 padFrames = 0;
            if (FAILED(m_audioClient->GetCurrentPadding(&padFrames))) continue;
            UINT32 available = m_renderBufFrames - padFrames;
            if (available == 0) continue;

            // How many capture frames do we need to produce `available` render frames?
            size_t srcNeeded = (size_t)std::ceil(available * ratio) + 2;
            size_t ringAvail = m_ring.available();

            // If ring is empty, fill with silence to avoid clicks
            if (ringAvail == 0)
            {
                BYTE* buf = nullptr;
                if (SUCCEEDED(m_renderClient->GetBuffer(available, &buf)) && buf)
                {
                    memset(buf, 0, available * m_renderCh * sizeof(float));
                    m_renderClient->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
                }
                continue;
            }

            size_t toRead = std::min(ringAvail, srcNeeded);
            if (m_srcBuf.size() < toRead * 2) m_srcBuf.resize(toRead * 2);
            size_t got = m_ring.read(m_srcBuf.data(), toRead);
            if (got == 0) continue;

            // Get render buffer
            BYTE* renderBytes = nullptr;
            if (FAILED(m_renderClient->GetBuffer(available, &renderBytes)) || !renderBytes)
                continue;

            float* out = (float*)renderBytes;
            const UINT32 outCh = m_renderCh;

            if (std::abs(ratio - 1.0) < 0.001 && m_captureCh == 2 && outCh == 2)
            {
                // Same rate, stereo → stereo: direct copy
                size_t frames = std::min((size_t)available, got);
                memcpy(out, m_srcBuf.data(), frames * 2 * sizeof(float));
                // Fill remainder with silence if needed
                if (frames < available)
                    memset(out + frames * 2, 0, (available - frames) * 2 * sizeof(float));
            }
            else
            {
                // Mix mono capture to stereo render, handle SR mismatch
                for (UINT32 i = 0; i < available; ++i)
                {
                    size_t srcIdx = std::min((size_t)(i * ratio), got > 0 ? got-1 : 0);
                    float L = m_srcBuf[srcIdx * 2];
                    float R = (m_captureCh > 1) ? m_srcBuf[srcIdx * 2 + 1] : L;
                    for (UINT32 c = 0; c < outCh; ++c)
                        out[i * outCh + c] = (c == 0) ? L : (c == 1 ? R : 0.f);
                }
            }

            m_renderClient->ReleaseBuffer(available, 0);
        }

        m_audioClient->Stop();
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        MICLOG("DirectMonitor::monitorThread exited");
    }

    void setError(const std::string& e)
    {
        std::lock_guard<std::mutex> lk(m_errMu);
        m_lastError = e;
        MICLOG("DirectMonitor ERROR: " << e);
    }

    // Render WASAPI
    IMMDevice*          m_device       = nullptr;
    IAudioClient*       m_audioClient  = nullptr;
    IAudioRenderClient* m_renderClient = nullptr;
    HANDLE              m_eventHandle  = nullptr;
    UINT32              m_renderBufFrames = 0;
    UINT32              m_renderSr     = 48000;
    UINT32              m_renderCh     = 2;

    // Capture info
    UINT32 m_captureSr = 48000;
    UINT32 m_captureCh = 2;

    // Ring buffer fed by capture thread
    AudioRingBuffer m_ring{9600};

    // Thread
    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopFlag{false};

    std::atomic<float> m_volume{0.8f};
    std::atomic<float> m_latencyMs{10.f};

    // Temp buffers
    std::vector<float> m_volBuf;
    std::vector<float> m_srcBuf;

    mutable std::mutex m_errMu;
    std::string        m_lastError;
};
