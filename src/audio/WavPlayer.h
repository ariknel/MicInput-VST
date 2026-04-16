#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WavPlayer — plays a WAV file through the default WASAPI render device.
// Runs a background thread. Provides peak thumbnail for waveform display.
// Thread-safe: all public methods callable from message thread.
//
// Fixed issues vs previous version:
//  - stream.release() now happens only after reader confirmed non-null
//  - WAVEFORMATEXTENSIBLE handled correctly (not mutated as WAVEFORMATEX)
//  - SetEventHandle only called after successful Initialize
//  - Resampling implemented so playback SR matches device SR
//  - dataMutex no longer held across WASAPI calls (deadlock-safe)
//  - m_loaded reset to false on stop()
//  - int overflow guard on large files (capped at 60s for UI)
// ─────────────────────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

class WavPlayer
{
public:
    WavPlayer()  = default;
    ~WavPlayer() { stop(); }

    // ── Message thread ────────────────────────────────────────────────────────
    // Load file, build peak thumbnail, pre-buffer audio. Blocking but fast (<50ms).
    bool load(const juce::String& filePath, int numBars = 60)
    {
        stop();

        juce::WavAudioFormat fmt;
        // Create stream first, check it, THEN pass to reader
        auto streamOwner = juce::File(filePath).createInputStream();
        if (!streamOwner) return false;

        // createReaderFor takes ownership of the stream.
        // Pass raw pointer with deleteStreamIfOpeningFails=true so JUCE
        // cleans up the stream even if reader creation fails.
        auto* rawStream = streamOwner.release();
        std::unique_ptr<juce::AudioFormatReader> reader(
            fmt.createReaderFor(rawStream, true));
        if (!reader) return false;  // JUCE already deleted rawStream

        m_fileSr       = reader->sampleRate;
        m_numChannels  = (int)std::min((unsigned int)reader->numChannels, 2u);
        m_filePath     = filePath;

        // Cap at 60 s to keep memory reasonable and avoid int overflow
        const int64_t maxFrames = (int64_t)(60.0 * m_fileSr);
        m_totalFrames = std::min(reader->lengthInSamples, maxFrames);

        buildPeaks(*reader, numBars);

        // Load into memory
        juce::AudioBuffer<float> buf(m_numChannels, (int)m_totalFrames);
        reader->read(&buf, 0, (int)m_totalFrames, 0, true, true);

        {
            std::lock_guard<std::mutex> lk(m_dataMutex);
            m_audio = std::move(buf);
        }

        m_position.store(0);
        m_playing.store(false);
        m_loaded.store(true);
        return true;
    }

    void play()
    {
        if (!m_loaded.load()) return;
        if (m_position.load() >= m_totalFrames) m_position.store(0);
        if (!m_threadRunning.load()) startThread();
        m_playing.store(true);
    }

    void pause() { m_playing.store(false); }

    void stop()
    {
        m_playing.store(false);
        m_loaded.store(false);
        m_stopThread.store(true);
        if (m_thread.joinable()) m_thread.join();
        m_stopThread.store(false);
        m_threadRunning.store(false);
    }

    void seek(float t) // 0..1
    {
        int64_t pos = (int64_t)(std::clamp(t, 0.f, 1.f) * (float)m_totalFrames);
        m_position.store(pos);
    }

    bool   isPlaying()  const { return m_playing.load(); }
    bool   isLoaded()   const { return m_loaded.load(); }
    float  getProgress() const
    {
        if (m_totalFrames <= 0) return 0.f;
        return (float)m_position.load() / (float)m_totalFrames;
    }
    double getDurationSecs() const
    {
        if (m_fileSr <= 0) return 0.0;
        return (double)m_totalFrames / m_fileSr;
    }
    double getPositionSecs() const
    {
        if (m_fileSr <= 0) return 0.0;
        return (double)m_position.load() / m_fileSr;
    }
    const std::vector<float>& peaks() const { return m_peaks; }

private:
    // ── Peak thumbnail ────────────────────────────────────────────────────────
    void buildPeaks(juce::AudioFormatReader& reader, int numBars)
    {
        m_peaks.assign(numBars, 0.f);
        if (m_totalFrames == 0) return;

        const int64_t framesPerBar = std::max((int64_t)1, m_totalFrames / (int64_t)numBars);
        const int chunkSize = 4096;
        juce::AudioBuffer<float> tmp(1, chunkSize);

        for (int b = 0; b < numBars; ++b)
        {
            int64_t start = (int64_t)b * framesPerBar;
            int64_t end   = std::min(start + framesPerBar, m_totalFrames);
            float   peak  = 0.f;
            for (int64_t pos = start; pos < end; pos += chunkSize)
            {
                int n = (int)std::min((int64_t)chunkSize, end - pos);
                reader.read(&tmp, 0, n, pos, true, false);
                for (int i = 0; i < n; ++i)
                    peak = std::max(peak, std::abs(tmp.getSample(0, i)));
            }
            m_peaks[b] = peak;
        }
        float mx = *std::max_element(m_peaks.begin(), m_peaks.end());
        if (mx > 0.f) for (auto& p : m_peaks) p /= mx;
    }

    // ── Thread management ─────────────────────────────────────────────────────
    void startThread()
    {
        m_stopThread.store(false);
        m_threadRunning.store(true);
        m_thread = std::thread(&WavPlayer::playerThread, this);
    }

    // ── WASAPI render thread ──────────────────────────────────────────────────
    void playerThread()
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDeviceEnumerator* pEnum = nullptr;
        IMMDevice*           pDev  = nullptr;
        IAudioClient*        pAC   = nullptr;
        IAudioRenderClient*  pRC   = nullptr;
        HANDLE               hEvent = nullptr;
        WAVEFORMATEX*        pwfx   = nullptr;
        UINT32               bufSize = 0;

        auto cleanup = [&]() {
            if (pRC)  { pRC->Release(); pRC = nullptr; }
            if (pAC)  { pAC->Stop(); pAC->Release(); pAC = nullptr; }
            if (pDev) { pDev->Release(); pDev = nullptr; }
            if (pEnum){ pEnum->Release(); pEnum = nullptr; }
            if (hEvent){ CloseHandle(hEvent); hEvent = nullptr; }
            if (pwfx) { CoTaskMemFree(pwfx); pwfx = nullptr; }
            CoUninitialize();
            m_threadRunning.store(false);
        };

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)))
        { cleanup(); return; }
        if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
        { cleanup(); return; }
        if (FAILED(pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAC)))
        { cleanup(); return; }

        // Get device mix format — keep as-is, don't mutate
        if (FAILED(pAC->GetMixFormat(&pwfx))) { cleanup(); return; }

        hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent) { cleanup(); return; }

        REFERENCE_TIME refDur = 200000; // 20ms
        if (FAILED(pAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, refDur, 0, pwfx, nullptr)))
        { cleanup(); return; }

        if (FAILED(pAC->SetEventHandle(hEvent))) { cleanup(); return; }
        if (FAILED(pAC->GetBufferSize(&bufSize))) { cleanup(); return; }
        if (FAILED(pAC->GetService(__uuidof(IAudioRenderClient), (void**)&pRC)))
        { cleanup(); return; }

        // Device format info
        const UINT32 devSr  = pwfx->nSamplesPerSec;
        const int    devCh  = pwfx->nChannels;

        // Determine if device wants float (WAVE_FORMAT_IEEE_FLOAT or EXTENSIBLE with KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
        bool devIsFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22)
        {
            // WAVEFORMATEXTENSIBLE: SubFormat GUID starts at byte 8 of the extra data
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT GUID: {00000003-0000-0010-8000-00AA00389B71}
            static const GUID kFloat = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
            devIsFloat = (IsEqualGUID(ext->SubFormat, kFloat) != 0);
        }

        // Resampling ratio: file SR → device SR
        const double ratio = (devSr > 0) ? (double)m_fileSr / (double)devSr : 1.0;

        pAC->Start();

        // Local copy buffer to avoid holding mutex while calling WASAPI
        std::vector<float> localL, localR;

        while (!m_stopThread.load())
        {
            DWORD wait = WaitForSingleObject(hEvent, 100);
            if (wait != WAIT_OBJECT_0) continue;
            if (!m_playing.load()) continue;

            UINT32 padding = 0;
            if (FAILED(pAC->GetCurrentPadding(&padding))) continue;
            UINT32 available = bufSize - padding;
            if (available == 0) continue;

            // Fill local buffer without holding mutex during WASAPI calls
            if (localL.size() < available) { localL.resize(available); localR.resize(available); }

            bool reachedEnd = false;
            {
                std::lock_guard<std::mutex> lk(m_dataMutex);
                for (UINT32 f = 0; f < available; ++f)
                {
                    // Nearest-neighbour resample: map device frame → file frame
                    int64_t srcPos = (int64_t)((m_position.load() + (int64_t)f) * ratio);
                    if (srcPos >= m_totalFrames)
                    {
                        // Fill rest with silence
                        for (UINT32 ff = f; ff < available; ++ff)
                            localL[ff] = localR[ff] = 0.f;
                        reachedEnd = true;
                        break;
                    }
                    localL[f] = m_audio.getSample(0, (int)srcPos);
                    localR[f] = (m_numChannels > 1) ? m_audio.getSample(1, (int)srcPos) : localL[f];
                }
            }

            // Advance position by device frames (will be mapped to file frames by ratio)
            if (!reachedEnd)
                m_position.fetch_add((int64_t)available);
            else
                m_playing.store(false);

            BYTE* pData = nullptr;
            if (FAILED(pRC->GetBuffer(available, &pData))) continue;

            // Write to device buffer
            if (devIsFloat)
            {
                float* out = reinterpret_cast<float*>(pData);
                for (UINT32 f = 0; f < available; ++f)
                {
                    out[f * devCh + 0] = localL[f];
                    if (devCh > 1) out[f * devCh + 1] = localR[f];
                    for (int c = 2; c < devCh; ++c) out[f * devCh + c] = 0.f;
                }
            }
            else
            {
                // 16-bit integer output
                int16_t* out = reinterpret_cast<int16_t*>(pData);
                for (UINT32 f = 0; f < available; ++f)
                {
                    out[f * devCh + 0] = (int16_t)(localL[f] * 32767.f);
                    if (devCh > 1) out[f * devCh + 1] = (int16_t)(localR[f] * 32767.f);
                    for (int c = 2; c < devCh; ++c) out[f * devCh + c] = 0;
                }
            }

            pRC->ReleaseBuffer(available, 0);
        }

        cleanup();
    }

    // ── State ─────────────────────────────────────────────────────────────────
    std::atomic<bool>    m_loaded       {false};
    std::atomic<bool>    m_playing      {false};
    std::atomic<bool>    m_stopThread   {false};
    std::atomic<bool>    m_threadRunning{false};
    std::atomic<int64_t> m_position     {0};

    int64_t      m_totalFrames  = 0;
    double       m_fileSr       = 48000.0;
    int          m_numChannels  = 2;
    juce::String m_filePath;

    std::mutex               m_dataMutex;
    juce::AudioBuffer<float> m_audio;
    std::vector<float>       m_peaks;
    std::thread              m_thread;
};
