#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WavRecorder  —  lock-free SPSC ring  →  background WAV writer
//
// Design rules:
//  • pushSamples()  called from audio thread — wait-free, never blocks
//  • stop()         called from MESSAGE thread only — never from audio thread
//  • stopAsync()    called from AUDIO thread (auto-save) — sets flag, returns
//                   immediately; the message thread must poll isRecording() and
//                   call finishAsync() when it sees recording has ended
//
// Sample-rate contract:
//  • Caller passes pre-resampled audio already at nativeSr
//  • WAV header is written at nativeSr
//  • DAW resamples on import if its project rate differs
// ─────────────────────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <chrono>
#include <cstring>

class WavRecorder
{
public:
    // Fired on message thread after file is fully written and closed
    std::function<void(juce::String /*path*/, double /*secs*/, int64_t /*bytes*/)> onFileDone;

    WavRecorder()  = default;
    ~WavRecorder() { stopAndWait(); }

    // ── Start (message thread) ────────────────────────────────────────────────
    bool start(const juce::String& filePath, double sampleRate, int numChannels = 2)
    {
        stopAndWait();   // finish any previous recording first

        juce::File f(filePath);
        f.getParentDirectory().createDirectory();

        juce::WavAudioFormat fmt;
        auto streamOwner = f.createOutputStream();
        if (!streamOwner) return false;

        auto* writer = fmt.createWriterFor(streamOwner.get(),
                                            sampleRate,
                                            (unsigned)numChannels,
                                            16, {}, 0);
        if (!writer) return false;
        streamOwner.release();   // writer owns the stream now

        m_writer.reset(writer);
        m_filePath    = filePath;
        m_sampleRate  = sampleRate;
        m_numChannels = numChannels;

        // Size ring for 60 s at this rate
        const size_t cap = nextPow2((size_t)(60.0 * sampleRate) * (size_t)numChannels + 1);
        m_ring.assign(cap, 0.f);
        m_mask = cap - 1;

        // Reset positions
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos .store(0, std::memory_order_relaxed);
        m_frameCount.store(0, std::memory_order_relaxed);
        m_stopRequested.store(false, std::memory_order_relaxed);
        m_running.store(true,  std::memory_order_release);

        m_thread = std::thread(&WavRecorder::writerLoop, this);
        return true;
    }

    // ── Stop from MESSAGE thread ──────────────────────────────────────────────
    // Signals writer to do a final drain then exit.  Blocks until done.
    void stopAndWait()
    {
        if (!m_running.load(std::memory_order_acquire)) return;
        m_stopRequested.store(true, std::memory_order_release);
        if (m_thread.joinable()) m_thread.join();
        finalise();
    }

    // ── Stop from AUDIO thread ────────────────────────────────────────────────
    // Wait-free — just sets a flag.  GUI must poll isRecording() and call
    // finishAsync() from the message thread once it goes false.
    void requestStop() noexcept
    {
        m_stopRequested.store(true, std::memory_order_release);
    }

    // Call from message thread after requestStop() when isRecording() == false
    void finishAsync()
    {
        if (m_thread.joinable()) m_thread.join();
        finalise();
    }

    bool isRecording() const noexcept
    {
        return m_running.load(std::memory_order_acquire);
    }

    double secondsRecorded() const noexcept
    {
        return (double)m_frameCount.load(std::memory_order_relaxed)
             / std::max(m_sampleRate, 1.0);
    }

    // ── Audio thread — wait-free ──────────────────────────────────────────────
    void pushSamples(const float* interleavedStereo, size_t numFrames) noexcept
    {
        if (!m_running.load(std::memory_order_relaxed)) return;

        const size_t cap  = m_ring.size();
        const size_t w    = m_writePos.load(std::memory_order_relaxed);
        const size_t r    = m_readPos .load(std::memory_order_acquire);
        const size_t used = w - r;                       // monotonic, always w>=r
        if (used + numFrames * 2 > cap) return;          // ring full — drop silently

        const size_t n = numFrames * 2;   // numFrames already bounds-checked above
        for (size_t i = 0; i < n; ++i)
            m_ring[(w + i) & m_mask] = interleavedStereo[i];

        m_writePos.store(w + n, std::memory_order_release);
    }

private:
    // ── Writer thread ─────────────────────────────────────────────────────────
    void writerLoop()
    {
        juce::Thread::setCurrentThreadName("MicInput-WAVWriter");

        std::vector<float> bufL, bufR;
        const float* chans[2] = { nullptr, nullptr };

        while (!m_stopRequested.load(std::memory_order_acquire))
        {
            drainOnce(4096, bufL, bufR, chans);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // Final drain: consume everything the audio thread has pushed so far
        drainOnce(m_ring.size(), bufL, bufR, chans);

        m_running.store(false, std::memory_order_release);
    }

    void drainOnce(size_t maxFrames,
                   std::vector<float>& bufL, std::vector<float>& bufR,
                   const float** chans)
    {
        if (!m_writer) return;

        const size_t r     = m_readPos .load(std::memory_order_relaxed);
        const size_t w     = m_writePos.load(std::memory_order_acquire);
        const size_t avail = (w - r) / 2;   // floats → stereo frames
        if (avail == 0) return;

        const size_t frames = std::min(avail, maxFrames);
        if (bufL.size() < frames) { bufL.resize(frames); bufR.resize(frames); }
        chans[0] = bufL.data(); chans[1] = bufR.data();

        for (size_t i = 0; i < frames; ++i)
        {
            bufL[i] = m_ring[(r + i * 2)     & m_mask];
            bufR[i] = m_ring[(r + i * 2 + 1) & m_mask];
        }
        m_readPos.store(r + frames * 2, std::memory_order_release);

        m_writer->writeFromFloatArrays(chans, m_numChannels, (int)frames);
        m_frameCount.fetch_add((int64_t)frames, std::memory_order_relaxed);
    }

    void finalise()
    {
        m_writer.reset();   // destructor flushes and writes WAV header length
        if (onFileDone)
        {
            double  secs  = (double)m_frameCount.load() / std::max(m_sampleRate, 1.0);
            int64_t bytes = juce::File(m_filePath).getSize();
            if (bytes > 44)
                onFileDone(m_filePath, secs, bytes);
        }
    }

    static size_t nextPow2(size_t n) noexcept
    {
        if (n == 0) return 1;
        --n;
        n |= n >> 1; n |= n >> 2; n |= n >> 4;
        n |= n >> 8; n |= n >> 16;
        // Only shift by 32 on 64-bit platforms
        if (sizeof(size_t) > 4) n |= n >> 32;
        return n + 1;
    }

    std::unique_ptr<juce::AudioFormatWriter> m_writer;
    std::thread       m_thread;
    std::atomic<bool> m_running      {false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<size_t> m_writePos   {0};
    std::atomic<size_t> m_readPos    {0};
    std::atomic<int64_t> m_frameCount{0};

    std::vector<float> m_ring;
    size_t             m_mask       = 0;
    juce::String       m_filePath;
    double             m_sampleRate  = 48000.0;
    int                m_numChannels = 2;
};
