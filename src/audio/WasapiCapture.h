#pragma once
#include "AudioRingBuffer.h"
#include "MicLog.h"

class DirectMonitor;  // forward
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "avrt.lib")

struct CaptureDeviceInfo
{
    std::wstring id;
    std::wstring name;
    bool         isDefault = false;
};

class WasapiCapture
{
public:
    WasapiCapture();
    ~WasapiCapture();

    static std::vector<CaptureDeviceInfo> enumerateDevices();

    // mode: 0 = shared (IAC3 auto), 1 = exclusive
    // dawSampleRate: the DAW's sample rate so we can resample if needed
    bool open(const std::wstring& deviceId, AudioRingBuffer& ring,
              int mode = 0, double dawSampleRate = 48000.0);
    void start();
    void stop();
    void close();

    bool    isRunning()         const { return m_running.load(); }
    float   actualPeriodMs()    const { return m_actualPeriodMs.load(); }
    float   streamLatencyMs()   const { return m_streamLatencyMs.load(); }
    bool    usedIAC3()          const { return m_usedIAC3.load(); }
    bool    isExclusive()       const { return m_isExclusive.load(); }
    float   levelL()            const { return m_levelL.load(std::memory_order_relaxed); }
    float   levelR()            const { return m_levelR.load(std::memory_order_relaxed); }
    UINT32  nativeSampleRate()  const { return m_nativeSr; }  // expose for resampling check
    std::string lastError()     const { std::lock_guard<std::mutex> l(m_errMu); return m_lastError; }

    // Optional direct monitor — set before start()
    void setDirectMonitor(DirectMonitor* dm) { m_directMonitor = dm; }
    std::string threadOptSummary() const { std::lock_guard<std::mutex> l(m_optMu); return m_threadOptSummary; }

private:
    void captureThread();
    void processPacket(const BYTE* data, UINT32 frames, bool silent);
    void convertToStereoFloat(const BYTE* src, float* dst, UINT32 frames);
    void updateLevels(const float* stereo, UINT32 frames);
    void setError(const std::string& e) { std::lock_guard<std::mutex> l(m_errMu); m_lastError = e; }

    IMMDevice*           m_device        = nullptr;
    IAudioClient*        m_audioClient   = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    HANDLE               m_eventHandle   = nullptr;

    UINT32 m_nativeCh    = 2;
    UINT32 m_nativeSr    = 48000;
    UINT32 m_dawSr       = 48000;  // DAW project sample rate
    bool   m_isFloat     = true;
    UINT32 m_bitDepth    = 32;
    UINT32 m_blockAlign  = 8;

    AudioRingBuffer*  m_ring          = nullptr;
    DirectMonitor*    m_directMonitor = nullptr;

    std::vector<float> m_convBuf;
    std::vector<float> m_stereoBuf;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopFlag{false};

    std::atomic<float> m_actualPeriodMs{10.0f};
    std::atomic<float> m_streamLatencyMs{0.0f};
    std::atomic<bool>  m_usedIAC3{false};
    std::atomic<bool>  m_isExclusive{false};
    std::atomic<float> m_levelL{0.0f};
    std::atomic<float> m_levelR{0.0f};

    mutable std::mutex m_errMu;
    std::string        m_lastError;
    mutable std::mutex m_optMu;
    std::string        m_threadOptSummary;
};
