#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// DeviceProber — pure functions, no JUCE dependency
// ─────────────────────────────────────────────────────────────────────────────
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audiopolicy.h>
#include <string>
#include <vector>

struct DeviceProfile
{
    float    sharedDefaultPeriodMs  = 10.0f;
    float    sharedMinPeriodMs      = 10.0f;
    bool     supportsIAC3SmallBuf   = false;
    bool     supportsExclusive      = false;
    float    exclusiveMinPeriodMs   = 3.0f;
    float    streamLatencyMs        = 10.0f;
    bool     isUsbDevice            = false;
    bool     isGenericDriver        = false;
    std::wstring driverName;
    std::wstring deviceName;

    float bestSharedPeriodMs() const
    {
        return supportsIAC3SmallBuf ? sharedMinPeriodMs : sharedDefaultPeriodMs;
    }
};

struct OutputProfile
{
    bool         isUsb            = false;
    bool         isBluetooth      = false;
    float        latencyMs        = 1.5f;
    std::wstring deviceName;
    std::vector<std::wstring> activeAppNames;
};

// ─────────────────────────────────────────────────────────────────────────────
// Rename internal namespace to avoid clash with juce::detail
// ─────────────────────────────────────────────────────────────────────────────
namespace MicInputDetail
{
    inline std::wstring getPropString(IPropertyStore* props, const PROPERTYKEY& key)
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(key, &pv)) && pv.pwszVal)
        {
            std::wstring s = pv.pwszVal;
            PropVariantClear(&pv);
            return s;
        }
        PropVariantClear(&pv);
        return L"";
    }

    inline bool containsCI(const std::wstring& s, const wchar_t* sub)
    {
        std::wstring sl = s, subl = sub;
        for (auto& c : sl)   c = towlower(c);
        for (auto& c : subl) c = towlower(c);
        return sl.find(subl) != std::wstring::npos;
    }
}

inline DeviceProfile probeDevice(IMMDevice* device)
{
    DeviceProfile p;
    if (!device) return p;

    IPropertyStore* props = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)))
    {
        p.deviceName    = MicInputDetail::getPropString(props, PKEY_Device_FriendlyName);
        p.driverName    = MicInputDetail::getPropString(props, PKEY_Device_Driver);
        std::wstring en = MicInputDetail::getPropString(props, PKEY_Device_EnumeratorName);
        p.isUsbDevice      = MicInputDetail::containsCI(en, L"USB");
        p.isGenericDriver  = MicInputDetail::containsCI(p.driverName, L"usbaudio");
        props->Release();
    }

    IAudioClient3* ac3 = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient3),
                                    CLSCTX_ALL, nullptr, (void**)&ac3)))
    {
        WAVEFORMATEX* wfx = nullptr;
        if (SUCCEEDED(ac3->GetMixFormat(&wfx)))
        {
            AudioClientProperties acp = {};
            acp.cbSize     = sizeof(acp);
            acp.bIsOffload = FALSE;
            acp.eCategory  = AudioCategory_Media;
            acp.Options    = AUDCLNT_STREAMOPTIONS_MATCH_FORMAT;
            ac3->SetClientProperties(&acp);

            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(wfx, &def, &fund, &mn, &mx)))
            {
                double sr = wfx->nSamplesPerSec;
                p.sharedDefaultPeriodMs = static_cast<float>(def / sr * 1000.0);
                p.sharedMinPeriodMs     = static_cast<float>(mn  / sr * 1000.0);
                p.supportsIAC3SmallBuf  = (mn < def);
            }

            // Quick stream latency probe
            IAudioClient* ac1 = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient),
                                            CLSCTX_ALL, nullptr, (void**)&ac1)))
            {
                if (SUCCEEDED(ac1->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                    100000, 0, wfx, nullptr)))
                {
                    REFERENCE_TIME lat = 0;
                    if (SUCCEEDED(ac1->GetStreamLatency(&lat)))
                        p.streamLatencyMs = static_cast<float>(lat / 10000.0);
                }
                ac1->Release();
            }

            // Exclusive probe
            UINT32 fund2 = (fund > 0) ? fund : 480;
            REFERENCE_TIME excPeriod = static_cast<REFERENCE_TIME>(fund2)
                * 10000000LL / static_cast<REFERENCE_TIME>(wfx->nSamplesPerSec);
            IAudioClient* acE = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient),
                                            CLSCTX_ALL, nullptr, (void**)&acE)))
            {
                HRESULT hr = acE->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    excPeriod, excPeriod, wfx, nullptr);
                p.supportsExclusive = SUCCEEDED(hr);
                if (p.supportsExclusive && wfx->nSamplesPerSec > 0)
                    p.exclusiveMinPeriodMs = static_cast<float>(
                        fund2 / (double)wfx->nSamplesPerSec * 1000.0);
                acE->Release();
            }

            CoTaskMemFree(wfx);
        }
        ac3->Release();
    }
    return p;
}

inline OutputProfile probeDefaultOutput()
{
    OutputProfile p;
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enumerator)))
        return p;

    IMMDevice* dev = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
    { enumerator->Release(); return p; }

    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
    {
        p.deviceName          = MicInputDetail::getPropString(props, PKEY_Device_FriendlyName);
        std::wstring en       = MicInputDetail::getPropString(props, PKEY_Device_EnumeratorName);
        p.isUsb               = MicInputDetail::containsCI(en, L"USB");
        p.isBluetooth         = MicInputDetail::containsCI(en, L"BTHENUM") ||
                                MicInputDetail::containsCI(p.deviceName, L"Bluetooth");
        if (p.isBluetooth)    p.latencyMs = 150.0f;
        else if (p.isUsb)     p.latencyMs = 1.5f;
        else                  p.latencyMs = 0.5f;
        props->Release();
    }

    // Detect active apps on the output device
    IAudioSessionManager2* mgr = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2),
                                 CLSCTX_ALL, nullptr, (void**)&mgr)))
    {
        IAudioSessionEnumerator* sessions = nullptr;
        if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions)))
        {
            int count = 0; sessions->GetCount(&count);
            for (int i = 0; i < count; ++i)
            {
                IAudioSessionControl* ctrl = nullptr;
                if (FAILED(sessions->GetSession(i, &ctrl))) continue;
                IAudioSessionControl2* ctrl2 = nullptr;
                if (SUCCEEDED(ctrl->QueryInterface(&ctrl2)))
                {
                    AudioSessionState state; DWORD pid = 0;
                    if (SUCCEEDED(ctrl2->GetProcessId(&pid)) &&
                        SUCCEEDED(ctrl->GetState(&state)) &&
                        state == AudioSessionStateActive && pid != 0)
                    {
                        HANDLE hProc = OpenProcess(
                            PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (hProc)
                        {
                            wchar_t path[MAX_PATH] = {}; DWORD sz = MAX_PATH;
                            if (QueryFullProcessImageNameW(hProc, 0, path, &sz))
                            {
                                std::wstring full = path;
                                auto sl = full.find_last_of(L"\\/");
                                std::wstring exe = (sl != std::wstring::npos)
                                    ? full.substr(sl + 1) : full;
                                struct { const wchar_t* e; const wchar_t* n; } known[] = {
                                    {L"Discord.exe",  L"Discord"},
                                    {L"chrome.exe",   L"Chrome"},
                                    {L"spotify.exe",  L"Spotify"},
                                    {L"msedge.exe",   L"Edge"},
                                    {L"Teams.exe",    L"Teams"},
                                    {L"obs64.exe",    L"OBS"},
                                    {L"firefox.exe",  L"Firefox"},
                                    {L"Zoom.exe",     L"Zoom"},
                                };
                                for (auto& k : known)
                                    if (MicInputDetail::containsCI(exe, k.e))
                                    { p.activeAppNames.push_back(k.n); break; }
                            }
                            CloseHandle(hProc);
                        }
                    }
                    ctrl2->Release();
                }
                ctrl->Release();
            }
            sessions->Release();
        }
        mgr->Release();
    }
    dev->Release();
    enumerator->Release();
    return p;
}
