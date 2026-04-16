#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MicLog — writes debug.log next to the VST3 bundle
// Falls back to the project source folder, then %APPDATA%/MicInput
// Tail live: powershell Get-Content "<path>/debug.log" -Wait
// ─────────────────────────────────────────────────────────────────────────────
#include <windows.h>
#include <shlobj.h>
#include <mmreg.h>
#include <audioclient.h>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <ctime>

class MicLogger
{
public:
    static MicLogger& get() { static MicLogger inst; return inst; }

    void log(const std::string& msg)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        ensureOpen();
        if (!m_file.is_open()) return;
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        struct tm tb{}; localtime_s(&tb, &t);
        m_file << std::put_time(&tb, "%H:%M:%S")
               << '.' << std::setw(3) << std::setfill('0') << ms.count()
               << "  " << msg << '\n';
        m_file.flush();
    }

    // Return log file path (for display in GUI)
    std::string logPath() const { return m_logPath; }

    static std::string hr(HRESULT h)
    {
        char buf[32]; sprintf_s(buf, sizeof(buf), "0x%08X", (unsigned)h);
        std::string s = buf;
        switch ((unsigned)h) {
        case 0x00000000: s += " S_OK"; break;
        case 0x88890008: s += " AUDCLNT_E_UNSUPPORTED_FORMAT"; break;
        case 0x88890003: s += " AUDCLNT_E_DEVICE_IN_USE"; break;
        case 0x88890004: s += " AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED"; break;
        case 0x88890001: s += " AUDCLNT_E_NOT_INITIALIZED"; break;
        case 0x88890019: s += " AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED"; break;
        case 0x88890021: s += " AUDCLNT_E_ENGINE_PERIODICITY_LOCKED"; break;
        case 0x80070005: s += " E_ACCESSDENIED"; break;
        case 0x80070057: s += " E_INVALIDARG"; break;
        case 0x80004005: s += " E_FAIL"; break;
        case 0x88890006: s += " AUDCLNT_E_ENDPOINT_CREATE_FAILED"; break;
        default: break;
        }
        return s;
    }

    static std::string fmtWfx(const WAVEFORMATEX* w)
    {
        if (!w) return "(null wfx)";
        char buf[256];
        const char* tag =
            w->wFormatTag == WAVE_FORMAT_PCM        ? "PCM" :
            w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? "Float32" :
            w->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? "Extensible" : "Unknown";
        sprintf_s(buf, sizeof(buf), "%s ch=%u sr=%u bits=%u blockAlign=%u",
            tag, w->nChannels, w->nSamplesPerSec,
            w->wBitsPerSample, w->nBlockAlign);
        if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const WAVEFORMATEXTENSIBLE* e = (const WAVEFORMATEXTENSIBLE*)w;
            char sub[64];
            sprintf_s(sub, sizeof(sub), " validBits=%u channelMask=0x%X",
                (unsigned)e->Samples.wValidBitsPerSample, (unsigned)e->dwChannelMask);
            return std::string(buf) + sub;
        }
        return std::string(buf);
    }

private:
    MicLogger() = default;

    void ensureOpen()
    {
        if (m_file.is_open()) return;

        // Try 1: next to the DLL/VST3 itself
        char dllPath[MAX_PATH]{};
        HMODULE hm = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&MicLogger::get, &hm);
        if (hm && GetModuleFileNameA(hm, dllPath, MAX_PATH))
        {
            std::string p = dllPath;
            auto slash = p.find_last_of("\\/");
            if (slash != std::string::npos)
            {
                std::string dir = p.substr(0, slash);
                m_logPath = dir + "\\MicInput_debug.log";
                m_file.open(m_logPath, std::ios::out | std::ios::trunc);
            }
        }

        // Try 2: %APPDATA%/MicInput/
        if (!m_file.is_open())
        {
            char appdata[MAX_PATH]{};
            SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
            std::string dir = std::string(appdata) + "\\MicInput";
            CreateDirectoryA(dir.c_str(), nullptr);
            m_logPath = dir + "\\debug.log";
            m_file.open(m_logPath, std::ios::out | std::ios::trunc);
        }

        if (m_file.is_open())
        {
            m_file << "=== MicInput VST debug.log ===\n";
            m_file << "Log location: " << m_logPath << "\n";
            m_file << "Tail: powershell Get-Content \"" << m_logPath << "\" -Wait\n";
            m_file << "=================================\n\n";
            m_file.flush();
        }
    }

    std::ofstream m_file;
    std::mutex    m_mu;
    std::string   m_logPath;
};

#define MICLOG(msg) \
    do { std::ostringstream _s; _s << msg; MicLogger::get().log(_s.str()); } while(0)
#define MICLOG_HR(label, h) \
    MICLOG(label << "  =>  " << MicLogger::hr(h))
#define MICLOG_WFX(label, w) \
    MICLOG(label << "  =>  " << MicLogger::fmtWfx(w))
