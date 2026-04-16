#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WhisperClient — local on-device speech-to-text via whisper.cpp
//
// No API key, no internet, no Python. Pure C++ inference.
// whisper.h is provided by the whisper.cpp FetchContent dependency.
// Models live in %APPDATA%\MicInput\models\ as ggml .bin files.
//
// Model sizes and typical CPU speed:
//   tiny    75 MB   ~1s   (any laptop)
//   base   142 MB   ~2s
//   small  466 MB   ~5s   (recommended default)
//   medium 1.5 GB  ~15s
//   large  2.9 GB  ~35s   (powerful PC only)
//
// Model download (Hugging Face):
//   https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-<size>.bin
// ─────────────────────────────────────────────────────────────────────────────
#include <JuceHeader.h>
#include <functional>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <mutex>

// whisper.h is included via target_include_directories in CMakeLists.
// This header is part of the whisper.cpp FetchContent dependency.
#include "whisper.h"

struct WhisperWord { std::string text; float t0 = 0.f, t1 = 0.f; };

class WhisperClient
{
public:
    using Callback = std::function<void(juce::String text, juce::String error, std::vector<WhisperWord> words)>;

    enum class ModelSize { Tiny, Base, Small, Medium, Large };

    static juce::String modelName(ModelSize s)
    {
        switch (s) {
            case ModelSize::Tiny:   return "tiny";
            case ModelSize::Base:   return "base";
            case ModelSize::Small:  return "small";
            case ModelSize::Medium: return "medium";
            case ModelSize::Large:  return "large-v3";
        }
        return "small";
    }

    static juce::String modelLabel(ModelSize s)
    {
        switch (s) {
            case ModelSize::Tiny:   return "Tiny (75 MB, ~1s)";
            case ModelSize::Base:   return "Base (142 MB, ~2s)";
            case ModelSize::Small:  return "Small (466 MB, ~5s)";
            case ModelSize::Medium: return "Medium (1.5 GB, ~15s)";
            case ModelSize::Large:  return "Large (2.9 GB, ~35s)";
        }
        return "Small";
    }

    static juce::File modelsDir()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("MicInput/models");
    }

    static juce::File modelFile(ModelSize s)
    {
        return modelsDir().getChildFile("ggml-" + modelName(s) + ".bin");
    }

    static bool modelExists(ModelSize s)
    {
        juce::File f = modelFile(s);
        return f.existsAsFile() && f.getSize() > 1024 * 1024;
    }

    static juce::String downloadUrl(ModelSize s)
    {
        return "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-"
             + modelName(s) + ".bin";
    }

    WhisperClient()  = default;
    ~WhisperClient() { cancelAndWait(); unloadModel(); }

    // ── Model management ──────────────────────────────────────────────────────
    void setModel(ModelSize size)
    {
        if (m_pendingSize == size) return;
        m_pendingSize = size;
        // Unload old model — new one loads lazily on next transcribe()
        unloadModel();
    }

    ModelSize getModel()      const { return m_pendingSize; }
    bool      isModelLoaded() const { return m_ctx != nullptr; }
    bool      hasModel()      const { return modelExists(m_pendingSize); }
    bool      isBusy()        const { return m_busy.load(); }
    // Returns a smoothly-advancing 0..1 progress estimate.
    // Uses time elapsed vs expected duration (audio_ms * model_speed_factor)
    // so it moves even when segments are few or zero.
    float getProgress() const
    {
        float p = m_progress.load();
        if (p >= 1.f || p <= 0.f) return p;
        // Time-based: whisper real-time factor ~3-8x for base model on CPU
        int64_t started = m_inferStartMs.load();
        int64_t durMs   = m_audioDurMs.load();
        if (started > 0 && durMs > 0)
        {
            int64_t elapsed = juce::Time::currentTimeMillis() - started;
            // Assume ~5x real-time factor → expected ms = durMs * 5
            float expected  = (float)(durMs * 5);
            float timeFrac  = (float)elapsed / expected;
            // Blend: take max of stored progress and time estimate, cap at 0.95
            float tProg = 0.1f + timeFrac * 0.85f;
            tProg = std::min(tProg, 0.95f);
            return std::max(p, tProg);
        }
        return p;
    }
    void      cancelTranscribe()    { m_cancelReq.store(true); }

    void unloadModel()
    {
        std::lock_guard<std::mutex> lk(m_ctxMutex);
        if (m_ctx) {
            whisper_free(m_ctx);
            m_ctx = nullptr;
            m_loadedSize = ModelSize::Small;
        }
    }

    // ── Transcription (async) ─────────────────────────────────────────────────
    bool transcribe(const juce::String& wavPath,
                    const juce::String& language,
                    Callback cb)
    {
        if (m_busy.load()) return false;

        if (!modelExists(m_pendingSize)) {
            cb({}, "Model not downloaded. Open Settings and click Download for "
                 + modelLabel(m_pendingSize) + ".", {});
            return false;
        }

        cancelAndWait();
        m_busy.store(true);

        m_thread = std::thread([this, wavPath, language, cb] {
            juce::String text, error;
            std::vector<WhisperWord> words;
            doTranscribe(wavPath, language, text, error, words);
            m_busy.store(false);
            cb(text, error, words);
        });
        return true;
    }

    // ── Language / settings ──────────────────────────────────────────────────
    void setLanguage(const juce::String& langCode)
    {
        // "auto" = whisper auto-detects; otherwise BCP-47 e.g. "en","nl","fr"
        std::lock_guard<std::mutex> lk(m_ctxMutex);
        m_language = langCode == "auto" ? "" : langCode.toStdString();
    }
    juce::String getLanguage() const
    {
        std::lock_guard<std::mutex> lk(m_ctxMutex);
        return m_language.empty() ? "auto" : juce::String(m_language);
    }

    // no_speech_thold: 0.0–1.0. Segments where Whisper is less confident than
    // this threshold are dropped. Default 0.6 suppresses [Music], [silence] etc.
    void setNoSpeechThold(float t)
    {
        std::lock_guard<std::mutex> lk(m_ctxMutex);
        m_noSpeechThold = std::clamp(t, 0.f, 1.f);
    }
    float getNoSpeechThold() const
    {
        std::lock_guard<std::mutex> lk(m_ctxMutex);
        return m_noSpeechThold;
    }

    void cancelAndWait()
    {
        if (m_thread.joinable()) {
            m_cancelReq.store(true);
            m_thread.join();
            m_cancelReq.store(false);
            m_progress.store(0.f);
        }
    }

private:
    // ── Inference ─────────────────────────────────────────────────────────────
    void doTranscribe(const juce::String& wavPath,
                      const juce::String& /*language*/,
                      juce::String& outText,
                      juce::String& outError,
                      std::vector<WhisperWord>& outWords)
    {
        m_progress.store(0.f);
        m_cancelReq.store(false);

        // ── Step 1: Load model (under lock) ──────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(m_ctxMutex);
            if (m_ctx == nullptr || m_loadedSize != m_pendingSize)
            {
                if (m_ctx) { whisper_free(m_ctx); m_ctx = nullptr; }
                juce::File mf = modelFile(m_pendingSize);
                if (!mf.existsAsFile()) {
                    outError = "Model file not found: " + mf.getFullPathName();
                    return;
                }
                std::string pathStr = mf.getFullPathName().toStdString();
                whisper_context_params cparams = whisper_context_default_params();
                cparams.use_gpu = false;
                m_ctx = whisper_init_from_file_with_params(pathStr.c_str(), cparams);
                if (!m_ctx) {
                    outError = "Failed to load model: " + mf.getFileName();
                    return;
                }
                m_loadedSize = m_pendingSize;
            }
        } // mutex released — UI can cancel without deadlock from here

        m_progress.store(0.05f);  // model loaded

        // ── Step 2: Decode WAV → f32 mono 16kHz ──────────────────────────────
        std::vector<float> samples;
        if (!loadWavAs16kMono(wavPath, samples, outError)) return;
        if (samples.empty()) { outError = "WAV file is empty"; return; }
        if (m_cancelReq.load()) { outError = "Cancelled"; return; }

        m_progress.store(0.1f);  // audio decoded

        // ── Step 3: Run whisper_full inference (no lock held) ─────────────────
        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.print_progress      = false;
        params.print_realtime      = false;
        params.print_timestamps    = false;
        params.print_special       = false;
        params.translate           = false;
        params.single_segment      = false;
        params.no_timestamps       = false;
        params.token_timestamps    = true;
        params.n_threads           = std::max(1, (int)std::thread::hardware_concurrency() - 1);
        params.no_speech_thold     = m_noSpeechThold;

        // Progress callback — updates m_progress during inference
        // whisper_full_params has a new_segment_callback fired per segment
        params.new_segment_callback       = [](whisper_context*, whisper_state*, int, void* ud) {
            auto* self = static_cast<WhisperClient*>(ud);
            // Increment progress between 0.1 and 0.95 per segment
            float cur = self->m_progress.load();
            if (cur < 0.92f) self->m_progress.store(cur + 0.04f);
        };
        params.new_segment_callback_user_data = this;

        std::string langStr = m_language.empty() ? "" : m_language;
        params.language = langStr.empty() ? nullptr : langStr.c_str();

        // Store timing info for time-based progress estimation
        m_audioDurMs.store((int64_t)(samples.size() * 1000LL / 16000LL));
        m_inferStartMs.store(juce::Time::currentTimeMillis());

        // Acquire lock only for the inference call itself — ctx is not thread-safe
        std::unique_lock<std::mutex> infLk(m_ctxMutex);
        if (m_cancelReq.load()) { outError = "Cancelled"; return; }
        int rc = whisper_full(m_ctx, params, samples.data(), (int)samples.size());
        infLk.unlock();
        m_inferStartMs.store(0);  // inference done — stop time-based estimation

        if (rc != 0) {
            outError = "Whisper inference failed";
            return;
        }

        // Collect segments and word-level timestamps
        const int n = whisper_full_n_segments(m_ctx);
        juce::String result;
        for (int i = 0; i < n; ++i) {
            const char* seg = whisper_full_get_segment_text(m_ctx, i);
            if (!seg) continue;
            juce::String line = juce::String::fromUTF8(seg).trim();
            if (line.startsWithChar('[') && line.endsWithChar(']')) continue;
            if (line.startsWithChar('(') && line.endsWithChar(')')) continue;
            if (line.isEmpty()) continue;
            result += line + "\n";

            // Collect word-level tokens for alignment view
            const int nt = whisper_full_n_tokens(m_ctx, i);
            for (int k = 0; k < nt; ++k) {
                const char* tok = whisper_full_get_token_text(m_ctx, i, k);
                auto tdata      = whisper_full_get_token_data(m_ctx, i, k);
                if (!tok) continue;
                juce::String w = juce::String::fromUTF8(tok).trim();
                if (w.isEmpty() || w.startsWithChar('[')) continue;
                WhisperWord ww;
                ww.text = w.toStdString();
                ww.t0   = (float)(tdata.t0 / 100.0);  // centiseconds → seconds
                ww.t1   = (float)(tdata.t1 / 100.0);
                outWords.push_back(ww);
            }
        }
        outText = result.trim();
        m_progress.store(1.f);
    }

    // ── WAV → f32 mono 16kHz ─────────────────────────────────────────────────
    bool loadWavAs16kMono(const juce::String& wavPath,
                          std::vector<float>& out,
                          juce::String& outError)
    {
        juce::WavAudioFormat fmt;
        auto streamOwner = juce::File(wavPath).createInputStream();
        if (!streamOwner) { outError = "Cannot open: " + wavPath; return false; }

        auto* rawStream = streamOwner.release();
        std::unique_ptr<juce::AudioFormatReader> reader(
            fmt.createReaderFor(rawStream, true));
        if (!reader) { outError = "Cannot read WAV: " + wavPath; return false; }

        const double  srcSr     = reader->sampleRate;
        const int     srcCh     = (int)std::min(reader->numChannels, 2u);
        const int64_t maxFrames = (int64_t)(30.0 * 60.0 * srcSr); // 30 min cap
        const int64_t readFrames = std::min(reader->lengthInSamples, maxFrames);

        if (readFrames == 0) { outError = "Empty WAV"; return false; }

        // Read into buffer
        juce::AudioBuffer<float> srcBuf(srcCh, (int)readFrames);
        reader->read(&srcBuf, 0, (int)readFrames, 0, true, srcCh > 1);

        // Mix to mono
        std::vector<float> mono((size_t)readFrames);
        const float* L = srcBuf.getReadPointer(0);
        const float* R = (srcCh > 1) ? srcBuf.getReadPointer(1) : L;
        for (int64_t i = 0; i < readFrames; ++i)
            mono[(size_t)i] = (L[i] + R[i]) * 0.5f;

        // Resample to 16000 Hz if needed
        const double targetSr = 16000.0;
        if (std::abs(srcSr - targetSr) < 1.0) {
            out = std::move(mono);
        } else {
            const double ratio   = srcSr / targetSr;
            const size_t outLen  = (size_t)((double)readFrames / ratio) + 8;
            out.resize(outLen);
            juce::LagrangeInterpolator resampler;
            resampler.reset();
            // process() returns INPUT samples consumed — output size is outLen
            resampler.process(ratio, mono.data(), out.data(),
                              (int)outLen, (int)readFrames, 0);
            // out.resize stays at outLen — already the correct output size
        }
        return true;
    }

    mutable std::mutex m_ctxMutex;   // guards m_ctx access from both threads
    whisper_context*  m_ctx         = nullptr;
    ModelSize         m_loadedSize  = ModelSize::Small;
    ModelSize         m_pendingSize = ModelSize::Small;
    std::string       m_language    = "en";   // BCP-47 code or "auto"
    float             m_noSpeechThold = 0.6f; // segments below this prob suppressed
    std::thread       m_thread;
    std::atomic<bool>  m_busy        {false};
    std::atomic<float>    m_progress        {0.f};   // 0..1 transcription progress
    std::atomic<bool>     m_cancelReq      {false}; // set true to abort inference
    std::atomic<int64_t>  m_inferStartMs   {0};     // wall-clock ms when inference began
    std::atomic<int64_t>  m_audioDurMs     {0};     // duration of audio being transcribed
};
