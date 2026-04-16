// Include order matters critically on Windows:
// 1. initguid.h first — enables GUID definitions in subsequent headers
// 2. windows.h chain before ksmedia (ks.h is pulled in via audioclient chain)
// 3. ksmedia.h last (requires ks.h which comes via mmdeviceapi -> audioclient chain)
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include "WasapiCapture.h"
#include "DirectMonitor.h"
#include "ThreadOptimizer.h"
#include "MicLog.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// timeBeginPeriod/timeEndPeriod from winmm.lib (already in CMakeLists)
extern "C" {
    __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
    __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────
WasapiCapture::WasapiCapture()
{
    // Force 1ms Windows scheduler quantum.
    // Default is 15.6ms — WaitForSingleObject wakes up to 15ms late without this.
    // All DAWs, Chrome, Discord call this. Safe to set globally.
    timeBeginPeriod(1);
    MICLOG("timeBeginPeriod(1) — scheduler quantum = 1ms");
}

WasapiCapture::~WasapiCapture()
{
    close();
    timeEndPeriod(1);
}

// ─── Enumerate devices ────────────────────────────────────────────────────────
std::vector<CaptureDeviceInfo> WasapiCapture::enumerateDevices()
{
    MICLOG("--- enumerateDevices ---");
    std::vector<CaptureDeviceInfo> result;
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    MICLOG_HR("CoCreateInstance(MMDeviceEnumerator)", hr);
    if (FAILED(hr)) return result;

    IMMDevice* defaultDev = nullptr;
    std::wstring defaultId;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultDev)))
    {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDev->GetId(&id))) { defaultId = id; CoTaskMemFree(id); }
        defaultDev->Release();
    }

    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)))
    { en->Release(); return result; }

    UINT count = 0; col->GetCount(&count);
    MICLOG("Found " << count << " capture device(s)");
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* dev = nullptr;
        if (FAILED(col->Item(i, &dev))) continue;
        CaptureDeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) { info.id = id; CoTaskMemFree(id); }
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
        {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal)
                info.name = pv.pwszVal;
            PropVariantClear(&pv); props->Release();
        }
        info.isDefault = (info.id == defaultId);
        std::string nm(info.name.begin(), info.name.end());
        MICLOG("  [" << i << "] " << nm << (info.isDefault ? " (DEFAULT)" : ""));
        result.push_back(std::move(info));
        dev->Release();
    }
    col->Release(); en->Release();
    return result;
}

// ─── open() ───────────────────────────────────────────────────────────────────
bool WasapiCapture::open(const std::wstring& deviceId, AudioRingBuffer& ring,
                          int mode, double dawSampleRate)
{
    MICLOG("\n========================================");
    MICLOG("WasapiCapture::open() mode=" << (mode==0?"Shared":"Exclusive")
        << " dawSr=" << (int)dawSampleRate);
    close();
    m_ring = &ring;
    ring.reset();

    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    MICLOG_HR("CoCreateInstance", hr);
    if (FAILED(hr)) { setError("CoCreateInstance failed"); return false; }

    if (deviceId.empty())
    {
        MICLOG("Using default capture device");
        hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &m_device);
    }
    else
    {
        std::string nm(deviceId.begin(), deviceId.end());
        MICLOG("Device: " << nm);
        hr = en->GetDevice(deviceId.c_str(), &m_device);
    }
    MICLOG_HR("GetDevice", hr);
    en->Release();

    if (FAILED(hr) || !m_device)
    { setError("Failed to get device — check Windows mic privacy settings"); return false; }

    // Get mix format — the engine's internal format
    // CRITICAL: Do NOT modify this before passing to shared Initialize
    // For exclusive: must use PKEY_AudioEngine_DeviceFormat instead
    IAudioClient* tempClient = nullptr;
    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&tempClient);
    MICLOG_HR("Activate for GetMixFormat", hr);
    if (FAILED(hr)) { setError("Activate failed"); return false; }

    WAVEFORMATEX* wfx = nullptr;
    hr = tempClient->GetMixFormat(&wfx);
    MICLOG_HR("GetMixFormat", hr);
    tempClient->Release();
    if (FAILED(hr) || !wfx) { setError("GetMixFormat failed"); return false; }

    MICLOG_WFX("MixFormat (engine internal)", wfx);

    // Also read PKEY_AudioEngine_DeviceFormat — the HARDWARE format
    // This is the only format GUARANTEED to work in exclusive mode
    WAVEFORMATEX* hwFmt = nullptr;
    size_t hwFmtSize = 0;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(m_device->OpenPropertyStore(STGM_READ, &props)))
    {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_AudioEngine_DeviceFormat, &pv))
            && pv.vt == VT_BLOB && pv.blob.cbSize >= sizeof(WAVEFORMATEX))
        {
            hwFmtSize = pv.blob.cbSize;
            hwFmt = (WAVEFORMATEX*)CoTaskMemAlloc(hwFmtSize);
            if (hwFmt) memcpy(hwFmt, pv.blob.pBlobData, hwFmtSize);
            MICLOG_WFX("PKEY_AudioEngine_DeviceFormat (hardware native)", hwFmt);
        }
        else MICLOG("PKEY_AudioEngine_DeviceFormat: not available or empty");
        PropVariantClear(&pv);
        props->Release();
    }

    // Store native format from mix format (what we'll actually receive in shared)
    m_nativeSr   = wfx->nSamplesPerSec;
    m_dawSr      = (UINT32)dawSampleRate;
    m_nativeCh   = wfx->nChannels;
    m_blockAlign = wfx->nBlockAlign;
    m_bitDepth   = wfx->wBitsPerSample;
    m_isFloat    = false;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        auto* ext = (WAVEFORMATEXTENSIBLE*)wfx;
        m_isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) m_isFloat = true;

    MICLOG("Native: sr=" << m_nativeSr << " daw=" << m_dawSr
        << " ch=" << m_nativeCh << " float=" << m_isFloat << " bits=" << m_bitDepth);
    if (m_nativeSr != m_dawSr)
        MICLOG("*** SR MISMATCH: mic=" << m_nativeSr << " DAW=" << m_dawSr
            << " — set Bitwig to " << m_nativeSr << "Hz to fix! ***");

    m_eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_eventHandle) { CoTaskMemFree(wfx); if(hwFmt) CoTaskMemFree(hwFmt); setError("CreateEvent failed"); return false; }

    if (mode == 1)
    {
        // ── EXCLUSIVE MODE ────────────────────────────────────────────────────
        // Research finding: USB Full Speed devices (usbaudio2.sys) have IRPs
        // that execute only every ~15ms making exclusive mode unreliable.
        // However we try anyway with proper format negotiation.
        //
        // Correct exclusive format order:
        // 1. PKEY_AudioEngine_DeviceFormat (guaranteed by Microsoft docs)
        // 2. 16-bit PCM (most USB devices accept this)
        // 3. 24-bit PCM
        // 4. Mix format as fallback (may fail on USB)
        MICLOG("--- Exclusive mode: probing formats ---");

        bool excSuccess = false;

        // Build format candidates for exclusive mode
        // ORDER MATTERS: Try simple formats first — USB devices prefer these
        // USB mics almost always reject EXTENSIBLE format in exclusive mode
        // even though the engine uses it internally

        // Candidate A: Simple 16-bit PCM (no EXTENSIBLE wrapper)
        // Most USB mics support this natively — it IS the hardware format
        WAVEFORMATEX pcm16 = {};
        pcm16.wFormatTag      = WAVE_FORMAT_PCM;
        pcm16.nChannels       = 2;
        pcm16.nSamplesPerSec  = m_nativeSr;
        pcm16.wBitsPerSample  = 16;
        pcm16.nBlockAlign     = 4;
        pcm16.nAvgBytesPerSec = m_nativeSr * 4;
        pcm16.cbSize          = 0;

        // Candidate B: Mono 16-bit (some mics only support mono in exclusive)
        WAVEFORMATEX pcm16mono = {};
        pcm16mono.wFormatTag      = WAVE_FORMAT_PCM;
        pcm16mono.nChannels       = 1;
        pcm16mono.nSamplesPerSec  = m_nativeSr;
        pcm16mono.wBitsPerSample  = 16;
        pcm16mono.nBlockAlign     = 2;
        pcm16mono.nAvgBytesPerSec = m_nativeSr * 2;
        pcm16mono.cbSize          = 0;

        // Candidate C: Simple 24-bit PCM
        WAVEFORMATEX pcm24 = {};
        pcm24.wFormatTag      = WAVE_FORMAT_PCM;
        pcm24.nChannels       = 2;
        pcm24.nSamplesPerSec  = m_nativeSr;
        pcm24.wBitsPerSample  = 24;
        pcm24.nBlockAlign     = 6;
        pcm24.nAvgBytesPerSec = m_nativeSr * 6;
        pcm24.cbSize          = 0;

        // Candidate D: Hardware registry format (if available)
        // Candidate E: Mix format last resort

        struct FmtCandidate { const char* name; WAVEFORMATEX* wf; };
        FmtCandidate candidates[] = {
            { "PCM 16-bit stereo (simple)",    &pcm16    },
            { "PCM 16-bit mono   (simple)",    &pcm16mono},
            { "PCM 24-bit stereo (simple)",    &pcm24    },
            { "PKEY_AudioEngine_DeviceFormat", hwFmt     },
            { "MixFormat float32 (unlikely)",  wfx       },
        };

        // Try each format with IsFormatSupported first, then Initialize
        for (auto& cand : candidates)
        {
            if (!cand.wf) continue;
            MICLOG("  Trying: " << cand.name);
            MICLOG_WFX("  Format", cand.wf);

            // Probe with IsFormatSupported
            IAudioClient* acProbe = nullptr;
            if (FAILED(m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&acProbe)))
                continue;

            HRESULT hrSupp = acProbe->IsFormatSupported(
                AUDCLNT_SHAREMODE_EXCLUSIVE, cand.wf, nullptr);
            MICLOG_HR("  IsFormatSupported(EXCLUSIVE)", hrSupp);
            acProbe->Release();

            if (FAILED(hrSupp) && hrSupp != AUDCLNT_E_UNSUPPORTED_FORMAT)
            {
                MICLOG("  Skipping (IsFormatSupported returned unexpected error)");
                continue;
            }

            // Try Initialize regardless (IsFormatSupported is not always reliable)
            if (FAILED(m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient)))
                continue;

            UINT32 hwFrames = (UINT32)(0.003 * m_nativeSr); // 3ms
            REFERENCE_TIME hwRef = (REFERENCE_TIME)(10000000.0 * hwFrames / m_nativeSr);

            hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hwRef, hwRef, cand.wf, nullptr);
            MICLOG_HR("  Initialize(EXCLUSIVE, 3ms)", hr);

            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
            {
                UINT32 aligned = 0;
                m_audioClient->GetBufferSize(&aligned);
                MICLOG("  Buffer not aligned, retrying with " << aligned << " frames");
                m_audioClient->Release(); m_audioClient = nullptr;
                m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
                REFERENCE_TIME alignedRef = (REFERENCE_TIME)(10000000.0 * aligned / m_nativeSr);
                hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, alignedRef, alignedRef, cand.wf, nullptr);
                MICLOG_HR("  Initialize(EXCLUSIVE, aligned)", hr);
            }

            if (SUCCEEDED(hr))
            {
                // Update format info from what actually worked
                m_bitDepth   = cand.wf->wBitsPerSample;
                m_blockAlign = cand.wf->nBlockAlign;
                m_isFloat    = (cand.wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
                if (cand.wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                    m_isFloat = IsEqualGUID(((WAVEFORMATEXTENSIBLE*)cand.wf)->SubFormat,
                                             KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

                UINT32 bufFrames = 0;
                m_audioClient->GetBufferSize(&bufFrames);
                m_actualPeriodMs.store((float)bufFrames / m_nativeSr * 1000.f);
                m_isExclusive.store(true); m_usedIAC3.store(false);
                MICLOG("EXCLUSIVE SUCCESS with: " << cand.name);
                MICLOG("  bufFrames=" << bufFrames << " periodMs=" << m_actualPeriodMs.load()
                    << " bits=" << m_bitDepth << " float=" << m_isFloat);
                excSuccess = true;
                break;
            }
            else
            {
                m_audioClient->Release(); m_audioClient = nullptr;
                MICLOG("  Failed with this format");
            }
        }

        if (!excSuccess)
        {
            MICLOG("=== EXCLUSIVE MODE: ALL FORMATS REJECTED ===");
            MICLOG("  Tried: PCM16 stereo, PCM16 mono, PCM24 stereo, DeviceFormat, MixFormat");
            MICLOG("  Root cause for USB mics (usbaudio2.sys):");
            MICLOG("    USB audio IRP timing = ~15ms intervals, exclusive mode needs <5ms");
            MICLOG("    The driver hard-limits min shared period to 480 frames (10ms at 48kHz)");
            MICLOG("    This is a Windows/driver architectural limit, not a bug in this plugin");
            MICLOG("");
            MICLOG("  BEST ACHIEVABLE LATENCY without exclusive:");
            float bestMs = 2.f + 10.f + 1.f + (m_dawSr>0 ? 128.f/m_dawSr*1000.f : 1.3f) + 1.5f;
            MICLOG("    Set Bitwig: 48000Hz + 128 block + 1ms buffer = ~" << bestMs << "ms total");
            MICLOG("    That is close to or better than exclusive mode on most USB interfaces");

            setError("Exclusive not supported by your USB mic driver (usbaudio2.sys). "
                     "Set Bitwig to 48kHz + 128 block size + 1ms buffer to get ~15ms — "
                     "comparable to exclusive. Audio is still working in shared mode.");
            CoTaskMemFree(wfx);
            if (hwFmt) CoTaskMemFree(hwFmt);
            return false;
        }
    }
    else
    {
        // ── SHARED MODE ───────────────────────────────────────────────────────
        MICLOG("--- Shared mode ---");
        m_isExclusive.store(false);

        // Try IAudioClient3 for small period first
        bool iac3Success = false;
        IAudioClient3* ac3 = nullptr;
        hr = m_device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void**)&ac3);
        MICLOG_HR("Activate(IAudioClient3)", hr);

        if (SUCCEEDED(hr))
        {
            AudioClientProperties acp{};
            acp.cbSize    = sizeof(acp);
            acp.eCategory = AudioCategory_Media;
            // Try RAW (bypass OEM APOs) first, fall back if not supported
            acp.Options = AUDCLNT_STREAMOPTIONS_MATCH_FORMAT | AUDCLNT_STREAMOPTIONS_RAW;
            HRESULT rawHr = ac3->SetClientProperties(&acp);
            if (FAILED(rawHr)) {
                acp.Options = AUDCLNT_STREAMOPTIONS_MATCH_FORMAT;
                ac3->SetClientProperties(&acp);
                MICLOG("RAW mode not supported, using standard");
            } else {
                MICLOG("RAW mode enabled (OEM APOs bypassed)");
            }

            UINT32 def=0, fund=0, mn=0, mx=0;
            hr = ac3->GetSharedModeEnginePeriod(wfx, &def, &fund, &mn, &mx);
            MICLOG_HR("GetSharedModeEnginePeriod", hr);
            if (SUCCEEDED(hr))
            {
                MICLOG("  Periods: def=" << def << " fund=" << fund
                    << " min=" << mn << " max=" << mx << " frames"
                    << "  defMs=" << (def*1000.0/m_nativeSr)
                    << " minMs=" << (mn*1000.0/m_nativeSr));

                if (mn < def)
                {
                    hr = ac3->InitializeSharedAudioStream(
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, wfx, nullptr);
                    MICLOG_HR("InitializeSharedAudioStream(min)", hr);
                    if (SUCCEEDED(hr))
                    {
                        m_audioClient = ac3; iac3Success = true; m_usedIAC3.store(true);
                        WAVEFORMATEX* cur = nullptr; UINT32 curP = 0;
                        ac3->GetCurrentSharedModeEnginePeriod(&cur, &curP);
                        if (cur) CoTaskMemFree(cur);
                        m_actualPeriodMs.store((float)(curP?curP:mn) / m_nativeSr * 1000.f);
                        MICLOG("IAC3 small buffer OK: periodMs=" << m_actualPeriodMs.load());
                    }
                }
                else
                {
                    MICLOG("IAC3: min==default (" << (def*1000.0/m_nativeSr)
                        << "ms) — driver limited (generic USB). Cannot go below 10ms in shared.");
                    MICLOG("ACTION: Use exclusive mode OR get an audio interface with proper ASIO drivers");
                }
            }
            if (!iac3Success) { ac3->Release(); ac3 = nullptr; }
        }

        if (!iac3Success)
        {
            m_usedIAC3.store(false);
            hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
            MICLOG_HR("Activate(IAudioClient, shared)", hr);
            if (FAILED(hr)) { CoTaskMemFree(wfx); if(hwFmt) CoTaskMemFree(hwFmt); setError("Activate failed"); return false; }

            // Request the MINIMUM possible buffer size in shared mode.
            // GetDevicePeriod returns the minimum the device supports.
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            m_audioClient->GetDevicePeriod(&defPeriod, &minPeriod);
            MICLOG("GetDevicePeriod: def=" << (defPeriod/10000.0) << "ms min=" << (minPeriod/10000.0) << "ms");

            // In shared mode, hnsBufferDuration must be >= default period (10ms for USB)
            // but we request exactly the minimum to reduce buffer frames
            REFERENCE_TIME reqDuration = (minPeriod > 0 && minPeriod < defPeriod) ? minPeriod : defPeriod;
            MICLOG("Requesting buffer duration: " << (reqDuration/10000.0) << "ms");

            hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                reqDuration, 0, wfx, nullptr);
            MICLOG_HR("Initialize(SHARED, min duration)", hr);

            if (FAILED(hr))
            {
                // Fall back to standard 10ms
                MICLOG("Min duration failed, retrying with 10ms");
                hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                    100000, 0, wfx, nullptr);
                MICLOG_HR("Initialize(SHARED, 10ms fallback)", hr);
            }

            if (FAILED(hr))
            {
                char buf[64]; sprintf_s(buf, sizeof(buf), "Shared init failed: %s", MicLogger::hr(hr).c_str());
                setError(buf); CoTaskMemFree(wfx); if(hwFmt) CoTaskMemFree(hwFmt); return false;
            }

            UINT32 bufFrames = 0; m_audioClient->GetBufferSize(&bufFrames);
            // GetBufferSize returns TOTAL buffer (often 2× period for double-buffering)
            // The ACTUAL capture period (wakeup interval) = what GetDevicePeriod returned
            // Use defPeriod frames as the period, not the total buffer size
            UINT32 periodFrames = (UINT32)(defPeriod / 10000000.0 * m_nativeSr);
            if (periodFrames == 0 || periodFrames > bufFrames) periodFrames = bufFrames / 2;
            if (periodFrames == 0) periodFrames = bufFrames;
            m_actualPeriodMs.store((float)periodFrames / m_nativeSr * 1000.f);
            MICLOG("Shared OK: bufFrames=" << bufFrames
                << " periodFrames=" << periodFrames
                << " periodMs=" << m_actualPeriodMs.load()
                << " (GetBufferSize=" << (bufFrames*1000.0/m_nativeSr) << "ms)");
        }
    }

    hr = m_audioClient->SetEventHandle(m_eventHandle);
    MICLOG_HR("SetEventHandle", hr);
    if (FAILED(hr)) { CoTaskMemFree(wfx); if(hwFmt) CoTaskMemFree(hwFmt); setError("SetEventHandle failed"); return false; }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    MICLOG_HR("GetService(CaptureClient)", hr);
    if (FAILED(hr)) { CoTaskMemFree(wfx); if(hwFmt) CoTaskMemFree(hwFmt); setError("GetService failed"); return false; }

    REFERENCE_TIME lat = 0;
    m_audioClient->GetStreamLatency(&lat);
    m_streamLatencyMs.store((float)(lat/10000.0));
    MICLOG("StreamLatency=" << m_streamLatencyMs.load() << "ms");

    m_convBuf.resize(8192 * m_nativeCh, 0.f);
    m_stereoBuf.resize(8192 * 2, 0.f);

    MICLOG("\n=== OPEN SUCCESS ==="
        << "\n  nativeSr=" << m_nativeSr << " dawSr=" << m_dawSr
        << "\n  actualPeriodMs=" << m_actualPeriodMs.load()
        << "\n  exclusive=" << m_isExclusive.load()
        << "\n  usedIAC3=" << m_usedIAC3.load()
        << "\n  isFloat=" << m_isFloat << " bits=" << m_bitDepth
        << "\n  ACTION NEEDED: Set Bitwig to " << m_nativeSr << "Hz + 128 block"
        << " for ~" << (2.0 + m_actualPeriodMs.load() + 128.0/m_nativeSr*1000.0 + 1.5) << "ms total\n");

    CoTaskMemFree(wfx);
    if (hwFmt) CoTaskMemFree(hwFmt);
    return true;
}

// ─── Start / Stop / Close ─────────────────────────────────────────────────────
void WasapiCapture::start()
{
    if (m_running.load() || !m_audioClient) return;
    MICLOG("WasapiCapture::start()");
    m_stopFlag.store(false); m_running.store(true);
    MICLOG_HR("IAudioClient::Start", m_audioClient->Start());
    m_thread = std::thread(&WasapiCapture::captureThread, this);
}

void WasapiCapture::stop()
{
    if (!m_running.load()) return;
    MICLOG("WasapiCapture::stop()");
    m_stopFlag.store(true);
    if (m_eventHandle) SetEvent(m_eventHandle);
    if (m_thread.joinable()) m_thread.join();
    if (m_audioClient) m_audioClient->Stop();
    m_running.store(false);
}

void WasapiCapture::close()
{
    stop();
    if (m_captureClient) { m_captureClient->Release(); m_captureClient = nullptr; }
    if (m_audioClient)   { m_audioClient->Release();   m_audioClient   = nullptr; }
    if (m_device)        { m_device->Release();         m_device        = nullptr; }
    if (m_eventHandle)   { CloseHandle(m_eventHandle);  m_eventHandle   = nullptr; }
    m_ring = nullptr;
}

// ─── Capture thread ───────────────────────────────────────────────────────────
void WasapiCapture::captureThread()
{
    MICLOG("--- captureThread started ---");
    ThreadOptResult opts = applyThreadOptimisations();
    { std::lock_guard<std::mutex> lk(m_optMu); m_threadOptSummary = opts.summary; }
    MICLOG("Thread opts: " << opts.summary);

    UINT64 totalFrames = 0, packets = 0;
    while (!m_stopFlag.load(std::memory_order_relaxed))
    {
        DWORD wr = WaitForSingleObject(m_eventHandle, 200);
        if (wr == WAIT_TIMEOUT) continue;
        if (wr != WAIT_OBJECT_0) continue;
        if (m_stopFlag.load(std::memory_order_relaxed)) break;

        UINT32 pktSize = 0;
        while (SUCCEEDED(m_captureClient->GetNextPacketSize(&pktSize)) && pktSize > 0)
        {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            HRESULT hr = m_captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) { MICLOG("GetBuffer FAILED: " << MicLogger::hr(hr)); break; }
            processPacket(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            m_captureClient->ReleaseBuffer(frames);
            totalFrames += frames; ++packets;
        }
        if (packets > 0 && packets % 500 == 0)
            MICLOG("captureThread: " << packets << " pkts " << totalFrames
                << " frames ring=" << (m_ring ? m_ring->available() : 0)
                << " level=" << m_levelL.load(std::memory_order_relaxed));
    }
    MICLOG("--- captureThread exit: " << packets << " pkts ---");
}

void WasapiCapture::processPacket(const BYTE* data, UINT32 frames, bool silent)
{
    if (!frames || !m_ring) return;
    const size_t needed = (size_t)frames * m_nativeCh;
    if (m_convBuf.size() < needed) m_convBuf.resize(needed * 2);
    float* conv = m_convBuf.data();
    if (silent) std::memset(conv, 0, needed * sizeof(float));
    else        convertToStereoFloat(data, conv, frames);

    const size_t ss = (size_t)frames * 2;
    if (m_stereoBuf.size() < ss) m_stereoBuf.resize(ss * 2);
    float* stereo = m_stereoBuf.data();

    if (m_nativeCh == 1)
        for (size_t i=0;i<frames;++i){stereo[i*2]=conv[i];stereo[i*2+1]=conv[i];}
    else if (m_nativeCh == 2)
        std::memcpy(stereo, conv, ss*sizeof(float));
    else
        for (size_t i=0;i<frames;++i){stereo[i*2]=conv[i*m_nativeCh];stereo[i*2+1]=conv[i*m_nativeCh+1];}

    updateLevels(stereo, frames);
    m_ring->write(stereo, frames);
    // Feed direct monitor (bypasses Bitwig output buffer)
    if (m_directMonitor)
        m_directMonitor->write(stereo, frames);
}

void WasapiCapture::convertToStereoFloat(const BYTE* src, float* dst, UINT32 frames)
{
    const UINT32 s = frames * m_nativeCh;
    if (m_isFloat) { std::memcpy(dst, src, s*sizeof(float)); return; }
    if (m_bitDepth == 16)
    {
        const float k=1.f/32768.f; auto* p=(const int16_t*)src;
        for (UINT32 i=0;i<s;++i) dst[i]=p[i]*k;
    }
    else if (m_bitDepth == 24)
    {
        const float k=1.f/8388608.f; const uint8_t* p=src;
        for (UINT32 i=0;i<s;++i){
            int32_t v=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16);
            v=(v<<8)>>8; dst[i]=v*k; p+=3;}
    }
    else
    {
        const float k=1.f/2147483648.f; auto* p=(const int32_t*)src;
        for (UINT32 i=0;i<s;++i) dst[i]=p[i]*k;
    }
}

void WasapiCapture::updateLevels(const float* s, UINT32 frames)
{
    if (!frames) return;
    float sL=0,sR=0;
    for (UINT32 i=0;i<frames;++i){sL+=s[i*2]*s[i*2];sR+=s[i*2+1]*s[i*2+1];}
    const float inv=1.f/frames; constexpr float a=0.15f;
    m_levelL.store(m_levelL.load(std::memory_order_relaxed)*(1-a)+std::sqrt(sL*inv)*a,std::memory_order_relaxed);
    m_levelR.store(m_levelR.load(std::memory_order_relaxed)*(1-a)+std::sqrt(sR*inv)*a,std::memory_order_relaxed);
}
