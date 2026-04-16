#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// AudioRingBuffer — lock-free single-producer / single-consumer ring buffer
//
// Frame = one stereo sample pair = 2 x float32 = 8 bytes
// Producer (capture thread): calls write()
// Consumer (audio thread):   calls read()
//
// Zero JUCE dependency — portable to ESP32 firmware later.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>

class AudioRingBuffer
{
public:
    // capacityFrames is rounded up to next power-of-2 internally
    explicit AudioRingBuffer(size_t capacityFrames = 48000)
    {
        resize(capacityFrames);
    }

    // Call from message thread only (not audio-safe)
    void resize(size_t capacityFrames)
    {
        size_t cap = nextPow2(capacityFrames + 1);
        m_buf.assign(cap * 2, 0.0f);   // 2 floats per stereo frame
        m_cap  = cap;
        m_mask = cap - 1;
        reset();
    }

    // ── Producer (capture thread) ──────────────────────────────────────────
    // stereoFrames: interleaved L,R,L,R... (numFrames * 2 floats)
    void write(const float* stereoFrames, size_t numFrames)
    {
        size_t w     = m_write.load(std::memory_order_relaxed);
        size_t avail = m_cap - 1 - (w - m_read.load(std::memory_order_acquire));
        if (numFrames > avail) numFrames = avail;   // drop oldest if full
        if (numFrames == 0) return;

        for (size_t i = 0; i < numFrames; ++i)
        {
            size_t idx = (w + i) & m_mask;
            m_buf[idx * 2]     = stereoFrames[i * 2];
            m_buf[idx * 2 + 1] = stereoFrames[i * 2 + 1];
        }
        m_write.store(w + numFrames, std::memory_order_release);
    }

    // ── Consumer (audio thread) ────────────────────────────────────────────
    // Returns number of frames actually read (may be less than requested)
    // dst: interleaved L,R,L,R... (numFrames * 2 floats)
    size_t read(float* dst, size_t numFrames)
    {
        size_t r     = m_read.load(std::memory_order_relaxed);
        size_t avail = m_write.load(std::memory_order_acquire) - r;
        if (numFrames > avail) numFrames = avail;
        if (numFrames == 0) return 0;

        for (size_t i = 0; i < numFrames; ++i)
        {
            size_t idx = (r + i) & m_mask;
            dst[i * 2]     = m_buf[idx * 2];
            dst[i * 2 + 1] = m_buf[idx * 2 + 1];
        }
        m_read.store(r + numFrames, std::memory_order_release);
        return numFrames;
    }

    // Frames available to read (approx — for safety margin logic only)
    size_t available() const noexcept
    {
        return m_write.load(std::memory_order_acquire)
             - m_read.load(std::memory_order_acquire);
    }

    size_t capacity() const noexcept { return m_cap - 1; }

    // Call only when capture is stopped (message thread safe)
    void reset()
    {
        m_read.store(0,  std::memory_order_relaxed);
        m_write.store(0, std::memory_order_relaxed);
        std::fill(m_buf.begin(), m_buf.end(), 0.0f);
    }

private:
    static size_t nextPow2(size_t n)
    {
        if (n == 0) return 1;
        --n;
        n |= n >> 1; n |= n >> 2; n |= n >> 4;
        n |= n >> 8; n |= n >> 16; n |= n >> 32;
        return n + 1;
    }

    size_t              m_cap  = 0;
    size_t              m_mask = 0;
    std::vector<float>  m_buf;
    std::atomic<size_t> m_write{0};
    std::atomic<size_t> m_read{0};
};
