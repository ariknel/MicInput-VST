#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ModelDownloader — downloads a whisper.cpp ggml model file over HTTPS
// directly into %APPDATA%\MicInput\models\ with live progress reporting.
//
// No browser, no drag-drop. Uses WinHTTP streaming read loop.
// Thread-safe: start() spawns a background thread; callbacks fire on it,
// caller must marshal to message thread with callAsync if touching GUI.
// ─────────────────────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include <atomic>
#include <thread>
#include <functional>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

class ModelDownloader
{
public:
    // Progress: bytes downloaded, total bytes (-1 if unknown), 0.0-1.0 fraction
    using ProgressCb = std::function<void(int64_t done, int64_t total, float frac)>;
    using DoneCb     = std::function<void(bool success, juce::String error)>;

    ModelDownloader()  = default;
    ~ModelDownloader() { cancel(); }

    // ── Start download (message thread) ──────────────────────────────────────
    // url:     full HTTPS URL
    // dest:    destination file (parent dir will be created)
    // onProg:  called every ~100ms from background thread
    // onDone:  called once when finished or failed
    void start(const juce::String& url,
               const juce::File&   dest,
               ProgressCb          onProg,
               DoneCb              onDone)
    {
        cancel();   // stop any existing download

        m_cancelled.store(false);
        m_running  .store(true);

        m_thread = std::thread([this, url, dest, onProg, onDone] {
            juce::String err;
            bool ok = download(url, dest, onProg, err);
            m_running.store(false);
            onDone(ok, err);
        });
    }

    // ── Cancel (message thread) — blocks until thread exits ──────────────────
    void cancel()
    {
        m_cancelled.store(true);
        if (m_thread.joinable()) m_thread.join();
        m_running.store(false);
    }

    bool isRunning() const { return m_running.load(); }

private:
    bool download(const juce::String& url,
                  const juce::File&   dest,
                  ProgressCb          onProg,
                  juce::String&       outError)
    {
        // Parse URL
        URL_COMPONENTS uc = {};
        uc.dwStructSize          = sizeof(uc);
        uc.dwHostNameLength      = (DWORD)-1;
        uc.dwUrlPathLength       = (DWORD)-1;
        uc.dwExtraInfoLength     = (DWORD)-1;

        std::wstring wurl = url.toWideCharPointer();
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
            outError = "Invalid URL: " + url;
            return false;
        }

        std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path(uc.lpszUrlPath,  uc.dwUrlPathLength);
        if (uc.dwExtraInfoLength > 0)
            path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

        // WinHTTP handles
        HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;

        auto cleanup = [&] {
            if (hRequest) WinHttpCloseHandle(hRequest);
            if (hConnect) WinHttpCloseHandle(hConnect);
            if (hSession) WinHttpCloseHandle(hSession);
        };

        hSession = WinHttpOpen(L"MicInput/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { outError = "WinHttpOpen failed"; return false; }

        hConnect = WinHttpConnect(hSession, host.c_str(),
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { cleanup(); outError = "WinHttpConnect failed"; return false; }

        hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
        if (!hRequest) { cleanup(); outError = "WinHttpOpenRequest failed"; return false; }

        // Follow redirects automatically
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                         &redirectPolicy, sizeof(redirectPolicy));

        // Set reasonable timeout: 30s connect, 5min receive (large files)
        DWORD connectTimeout = 30000;
        DWORD receiveTimeout = 300000;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT,  &connectTimeout, sizeof(connectTimeout));
        WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT,  &receiveTimeout, sizeof(receiveTimeout));

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        { cleanup(); outError = "WinHttpSendRequest failed"; return false; }

        if (!WinHttpReceiveResponse(hRequest, nullptr))
        { cleanup(); outError = "Server did not respond"; return false; }

        // Check HTTP status
        DWORD statusCode = 0, statusLen = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusLen, WINHTTP_NO_HEADER_INDEX);
        if (statusCode != 200) {
            cleanup();
            outError = "HTTP " + juce::String((int)statusCode);
            return false;
        }

        // Content-Length for progress
        int64_t totalBytes = -1;
        {
            wchar_t lenBuf[32] = {};
            DWORD   lenBufSz   = sizeof(lenBuf);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    lenBuf, &lenBufSz, WINHTTP_NO_HEADER_INDEX))
                totalBytes = (int64_t)_wtoi64(lenBuf);
        }

        // Prepare destination
        dest.getParentDirectory().createDirectory();

        // Write to temp file first, rename on success to avoid partial files
        juce::File tmpFile = dest.withFileExtension(".tmp");
        tmpFile.deleteFile();

        auto stream = tmpFile.createOutputStream();
        if (!stream) { cleanup(); outError = "Cannot create file: " + tmpFile.getFullPathName(); return false; }

        // Stream download
        const size_t chunkSize = 256 * 1024;  // 256 KB chunks
        std::vector<char> buf(chunkSize);
        int64_t downloaded = 0;
        auto lastProgress  = juce::Time::getMillisecondCounterHiRes();

        DWORD avail = 0;
        while (!m_cancelled.load())
        {
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
            if (avail == 0) break;   // done

            DWORD toRead  = std::min((DWORD)chunkSize, avail);
            DWORD bytesRead = 0;

            if (!WinHttpReadData(hRequest, buf.data(), toRead, &bytesRead) || bytesRead == 0)
                break;

            stream->write(buf.data(), (size_t)bytesRead);
            downloaded += (int64_t)bytesRead;

            // Throttle progress callbacks to ~10/s
            double now = juce::Time::getMillisecondCounterHiRes();
            if (now - lastProgress >= 100.0) {
                lastProgress = now;
                float frac = (totalBytes > 0)
                    ? std::min(1.f, (float)downloaded / (float)totalBytes)
                    : 0.f;
                onProg(downloaded, totalBytes, frac);
            }
        }

        stream.reset();   // flush + close
        cleanup();

        if (m_cancelled.load()) {
            tmpFile.deleteFile();
            outError = "Cancelled";
            return false;
        }

        // Validate: must be at least 10 MB (smallest real model is 75 MB)
        if (tmpFile.getSize() < 10 * 1024 * 1024) {
            tmpFile.deleteFile();
            outError = "Download incomplete or corrupted ("
                     + juce::String(tmpFile.getSize() / 1024) + " KB received)";
            return false;
        }

        // Atomic rename
        dest.deleteFile();
        if (!tmpFile.moveFileTo(dest)) {
            outError = "Could not move file to: " + dest.getFullPathName();
            return false;
        }

        onProg(downloaded, totalBytes, 1.f);
        return true;
    }

    std::atomic<bool> m_running   {false};
    std::atomic<bool> m_cancelled {false};
    std::thread       m_thread;
};
