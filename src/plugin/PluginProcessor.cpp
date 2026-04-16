#include "audio/MicLog.h"
#include "audio/PitchDetector.h"
#include "plugin/PluginProcessor.h"
#include "gui/PluginEditor.h"
#include <algorithm>
#include "audio/WhisperClient.h"
#include <cmath>

static juce::PropertiesFile* getAppProps()
{
    static juce::ApplicationProperties appProps;
    if (!appProps.getUserSettings())
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "MicInput";
        opts.filenameSuffix      = "settings";
        opts.osxLibrarySubFolder = "Application Support";
        appProps.setStorageParameters(opts);
    }
    return appProps.getUserSettings();
}


thread_local std::vector<float> MicInputProcessor::s_interleavedBuf;

// ─────────────────────────────────────────────────────────────────────────────
MicInputProcessor::MicInputProcessor()
    : AudioProcessor(MicInputProcessor::getDefaultBuses())
    , apvts(*this, nullptr, "MicInputState", MicInput::Params::createLayout())
{
    MICLOG("=== MicInputProcessor created ===");
    // Detect host PDC capability immediately on construction
    m_hostAppliesPDC.store(hostAppliesPDC());
    MICLOG("Host: " << juce::PluginHostType().getHostDescription()
        << " autoPDC=" << m_hostAppliesPDC.load());
    MICLOG("Log: " << MicLogger::get().logPath());
    // Create models directory and load persisted whisper preferences
    WhisperClient::modelsDir().createDirectory();
    if (auto* props = getAppProps())
    {
        int modelIdx = props->getIntValue("whisper_model", (int)WhisperClient::ModelSize::Small);
        modelIdx = std::clamp(modelIdx, 0, 4);
        m_whisper.setModel((WhisperClient::ModelSize)modelIdx);
        m_whisper.setLanguage(props->getValue("whisper_language", "en"));
        m_whisper.setNoSpeechThold((float)props->getDoubleValue("whisper_no_speech_thold", 0.6));
    }

    refreshDevices();
    currentOutputProfile = probeDefaultOutput();
    outputIsUsb.store(currentOutputProfile.isUsb);
    outputIsBluetooth.store(currentOutputProfile.isBluetooth);
    outputLatMs.store(currentOutputProfile.latencyMs);
}

MicInputProcessor::~MicInputProcessor()
{
    MICLOG("=== MicInputProcessor destroyed ===");
    closeCapture();
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<CaptureDeviceInfo> MicInputProcessor::getAvailableDevices() const
{
    std::lock_guard<std::mutex> lk(m_devicesMu);
    return m_devices;
}

void MicInputProcessor::refreshDevices()
{
    auto devs = WasapiCapture::enumerateDevices();
    MICLOG("refreshDevices: " << devs.size() << " device(s)");
    std::lock_guard<std::mutex> lk(m_devicesMu);
    m_devices = std::move(devs);
}

void MicInputProcessor::selectDevice(int index)
{
    MICLOG("selectDevice(" << index << ")");
    {
        std::lock_guard<std::mutex> lk(m_devicesMu);
        m_selectedDeviceIndex = index;
        m_selectedDeviceId = (index >= 0 && index < (int)m_devices.size())
            ? juce::String(m_devices[index].id.c_str()).toStdString() : "";
    }
    // Probe device
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)))
    {
        IMMDevice* dev = nullptr;
        if (m_selectedDeviceId.empty())
            en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
        else
        {
            std::wstring w(m_selectedDeviceId.begin(), m_selectedDeviceId.end());
            en->GetDevice(w.c_str(), &dev);
        }
        if (dev) { currentDeviceProfile = probeDevice(dev); dev->Release(); }
        en->Release();
    }
    closeCapture(); openCapture();
    currentOutputProfile = probeDefaultOutput();
    outputIsUsb.store(currentOutputProfile.isUsb);
    outputIsBluetooth.store(currentOutputProfile.isBluetooth);
    outputLatMs.store(currentOutputProfile.latencyMs);
    recalcTotalLatency();
}

void MicInputProcessor::setMode(int mode)
{
    MICLOG("setMode(" << mode << ")");
    m_captureMode = mode;
    closeCapture(); openCapture();
    recalcTotalLatency();
}

void MicInputProcessor::setPrebufMs(float ms)
{
    ms = std::max(1.0f, std::min(50.0f, ms));
    m_prebufMs.store(ms);

    float natSr = nativeSr.load();
    if (natSr <= 0) natSr = 48000.f;
    size_t newPrebuf = static_cast<size_t>(ms / 1000.f * natSr);
    newPrebuf = std::max(newPrebuf, static_cast<size_t>(32));
    m_prebufFrames.store(newPrebuf);

    // If ring has MORE than newPrebuf + 1 block, drain the excess now.
    // This makes the latency change take effect immediately instead of
    // waiting for old buffered audio to drain naturally (which takes seconds).
    float dawSr_ = dawSr.load();
    if (dawSr_ <= 0) dawSr_ = 48000.f;
    size_t blockFrames = static_cast<size_t>(m_blockSize);
    size_t targetMax   = newPrebuf + blockFrames * 2;  // keep 2 blocks worth after floor
    size_t avail       = m_ring.available();
    if (avail > targetMax)
    {
        // Drain the stale frames — they're old audio we no longer want to hold
        size_t toDrain = avail - targetMax;
        static thread_local std::vector<float> drainBuf;
        if (drainBuf.size() < toDrain * 2) drainBuf.resize(toDrain * 2);
        m_ring.read(drainBuf.data(), toDrain);
        // Reset resamplers — we skipped frames so their internal state is invalid
        m_resamplerL.reset(); m_resamplerR.reset();
        MICLOG("setPrebufMs: drained " << toDrain << " excess frames, ring now=" << m_ring.available());
    }

    MICLOG("setPrebufMs: " << ms << "ms = " << newPrebuf << " native frames");
    recalcTotalLatency();
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputProcessor::openCapture()
{
    if (m_sampleRate <= 0.0) { MICLOG("openCapture: deferred (no SR yet)"); return; }

    std::wstring wid;
    { std::lock_guard<std::mutex> lk(m_devicesMu);
      wid = std::wstring(m_selectedDeviceId.begin(), m_selectedDeviceId.end()); }

    m_ring.reset();
    m_resamplerL.reset(); m_resamplerR.reset();
    MICLOG("openCapture: mode=" << m_captureMode << " sr=" << m_sampleRate);

    if (m_capture.open(wid, m_ring, m_captureMode, m_sampleRate))
    {
        m_capture.start();
        isCapturing.store(true);
        captureMs.store(m_capture.actualPeriodMs());
        usedIAC3.store(m_capture.usedIAC3());
        isExclusive.store(m_capture.isExclusive());

        float natSr_ = (float)m_capture.nativeSampleRate();
        nativeSr.store(natSr_);
        dawSr.store((float)m_sampleRate);

        // Set resampling
        if (natSr_ > 0 && std::abs(natSr_ - (float)m_sampleRate) > 1.f)
        {
            m_resampleRatio = natSr_ / m_sampleRate;
            MICLOG("Resampling: ratio=" << m_resampleRatio
                << " nativeSr=" << natSr_ << " dawSr=" << m_sampleRate);
        }
        else
        {
            m_resampleRatio = 1.0;
            MICLOG("No resampling needed (rates match)");
        }

        // Calculate prebuf frames from current prebufMs setting
        // Refresh blockMs in case it was stale
        if (m_sampleRate > 0 && m_blockSize > 0)
            blockMs.store((float)m_blockSize / (float)m_sampleRate * 1000.f);
        setPrebufMs(m_prebufMs.load());

        // Restart direct monitor if it was enabled
        if (m_monitorEnabled.load())
        {
            m_capture.setDirectMonitor(&m_monitor);
            UINT32 monSr = (UINT32)m_capture.nativeSampleRate();
            if (monSr == 0) monSr = 48000;
            m_monitor.start(monSr, 2);
        }

        MICLOG("openCapture: SUCCESS captureMs=" << captureMs.load()
            << " prebufMs=" << m_prebufMs.load()
            << " prebufFrames=" << m_prebufFrames.load());
        recalcTotalLatency();
    }
    else
    {
        std::string err = m_capture.lastError();
        MICLOG("openCapture: FAILED — " << err);

        // Auto-fallback: if exclusive failed, retry with shared mode
        if (m_captureMode == 1)
        {
            MICLOG("openCapture: exclusive failed, auto-falling back to shared mode");
            m_ring.reset();
            m_resamplerL.reset(); m_resamplerR.reset();
            if (m_capture.open(wid, m_ring, 0, m_sampleRate))  // mode 0 = shared
            {
                m_capture.start();
                isCapturing.store(true);
                captureMs.store(m_capture.actualPeriodMs());
                usedIAC3.store(m_capture.usedIAC3());
                isExclusive.store(false);  // we are NOT in exclusive despite user request
                float natSr_ = (float)m_capture.nativeSampleRate();
                nativeSr.store(natSr_);
                dawSr.store((float)m_sampleRate);
                m_resampleRatio = (std::abs(natSr_ - (float)m_sampleRate) > 1.f)
                    ? natSr_ / m_sampleRate : 1.0;
                setPrebufMs(m_prebufMs.load());
                // Keep the exclusive error so GUI can show it
                m_exclusiveFailReason = err;
                MICLOG("openCapture: fallback to shared succeeded");
                recalcTotalLatency();
                return;
            }
        }
        isCapturing.store(false);
        m_exclusiveFailReason = err;
    }
}

void MicInputProcessor::closeCapture()
{
    m_monitor.stop();
    m_capture.setDirectMonitor(nullptr);
    m_capture.stop(); m_capture.close();
    isCapturing.store(false);
}

// Detect whether the current host applies PDC for instrument (IS_SYNTH) plugins.
// setLatencySamples() is always called — DAWs that honour it apply it automatically.
// Those that don't (Bitwig/Ableton/FL) ignore it; the GUI then shows a manual warning.
//
// AUTO (setLatencySamples honoured for IS_SYNTH instruments):
//   Reaper, Cubase, Nuendo, Studio One, Waveform/Tracktion, Cakewalk/Sonar
//   Pyramix, Samplitude, Sequoia
//
// MANUAL (IS_SYNTH offset ignored — user must set record offset in DAW):
//   Bitwig Studio, Ableton Live, FL Studio
//   Logic Pro / GarageBand (Mac — PDC for instruments is effects-chain only)
//   Pro Tools, Reason
//
namespace {
[[maybe_unused]] bool hostAppliesPDC()
{
    juce::PluginHostType host;
    // Strongly confirmed via JUCE isXxx() helpers:
    if (host.isReaper())         return true;
    if (host.isCubase())         return true;
    if (host.isNuendo())         return true;
    if (host.isStudioOne())      return true;

    // isWaveform() and isSonar() don't exist in JUCE 8 — use name matching instead
    juce::String desc = host.getHostDescription();
    if (desc.containsIgnoreCase("Waveform"))     return true;   // Tracktion / Waveform
    if (desc.containsIgnoreCase("Tracktion"))    return true;
    if (desc.containsIgnoreCase("Sonar"))        return true;   // Cakewalk / Sonar / BandLab
    if (desc.containsIgnoreCase("Cakewalk"))     return true;
    if (desc.containsIgnoreCase("Pyramix"))      return true;   // Merging Technologies
    if (desc.containsIgnoreCase("Samplitude"))   return true;   // Magix
    if (desc.containsIgnoreCase("Sequoia"))      return true;   // Magix broadcast

    // Logic / GarageBand: PDC exists but applies to effects chain, NOT instrument output.
    // With IS_SYNTH the host records the raw output — setLatencySamples is ignored.
    // So they fall through to manual (return false).

    return false;
}
} // anonymous namespace

void MicInputProcessor::recalcTotalLatency()
{
    // Estimated total monitoring latency:
    // USB mic ADC + WASAPI capture period + prebuffer hold + half block + output DAC
    const float usbMicMs = 2.0f;
    const float capMs_   = captureMs.load();
    const float preMs    = m_prebufMs.load();
    const float blkMs_   = blockMs.load() * 0.5f;
    const float outMs    = outputLatMs.load();
    const float total    = usbMicMs + capMs_ + preMs + blkMs_ + outMs;
    totalLatMs.store(total);
    prebufLatMs.store(preMs);

    if (m_sampleRate > 0.0)
    {
        const float recOffset = usbMicMs + capMs_ + preMs + blkMs_;
        const int   samples   = (int)(recOffset / 1000.f * (float)m_sampleRate);

        // Always call setLatencySamples — hosts that honour it will apply automatically
        setLatencySamples(samples);
        m_recordOffsetSamples.store(samples);

        // Detect if this host auto-applies PDC for instruments
        bool autoPDC = hostAppliesPDC();
        m_hostAppliesPDC.store(autoPDC);

        juce::PluginHostType host;
        MICLOG("recalcTotalLatency: monitoring=" << total << "ms"
            << "  recOffset=" << recOffset << "ms"
            << "  samples=-" << samples
            << "  host=" << host.getHostDescription()
            << "  autoPDC=" << autoPDC);

        measuredLatMs.store(total, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputProcessor::prepareToPlay(double sampleRate, int maxBlockSize)
{
    MICLOG("\n--- prepareToPlay: sr=" << sampleRate << " block=" << maxBlockSize << " ---");
    const bool srChanged  = std::abs(sampleRate - m_sampleRate) > 1.0;
    m_sampleRate = sampleRate;
    m_blockSize  = maxBlockSize;
    blockMs.store((float)maxBlockSize / (float)sampleRate * 1000.f);

    if (srChanged)
    {
        m_ring.resize((size_t)(sampleRate * 0.5)); // 500ms
        m_ring.reset();
    }

    if (srChanged || !isCapturing.load())
    {
        closeCapture(); openCapture();
    }
    else recalcTotalLatency();
}

void MicInputProcessor::releaseResources() {}

// ─────────────────────────────────────────────────────────────────────────────
// processBlock — hot path
// ─────────────────────────────────────────────────────────────────────────────
void MicInputProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = getTotalNumOutputChannels();
    buffer.clear();

    if (!isCapturing.load(std::memory_order_relaxed)) return;

    // ── MIDI trigger: note-on on any channel starts/stops manual recording ────
    for (const auto meta : midiMessages) {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn()) {
            // Signal via atomic flag — editor timerCallback handles actual start/stop
            // We use a dedicated atomic to avoid calling into JUCE/OS from audio thread
            bool cur = m_midiRecTrigger.load(std::memory_order_relaxed);
            m_midiRecTrigger.store(!cur, std::memory_order_relaxed);
        }
    }

    // ── Read transport info (BPM, loop position) ──────────────────────────────
    double currentBpm = 0.0, currentPpq = -1.0;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            if (auto bpm = pos->getBpm())         currentBpm = *bpm;
            if (auto ppq = pos->getPpqPosition()) currentPpq  = *ppq;
            // Loop detection done via ppqPosition backward jump (see below)
        }
    }
    if (currentBpm > 0.0) sessionBpm.store((float)currentBpm, std::memory_order_relaxed);

    // ── Auto-save: mirror DAW record arm state ────────────────────────────────
    if (m_saveEnabled.load(std::memory_order_relaxed))
    {
        bool dawRec = false;
        if (auto* ph = getPlayHead())
        {
            if (auto pos = ph->getPosition())
                dawRec = pos->getIsRecording();
        }
        isDawRecording.store(dawRec, std::memory_order_relaxed);

        bool wasArmed = m_wasArmed.load(std::memory_order_relaxed);
        // ── Loop record: detect ppqPosition jumping backward (loop restart) ────
        bool loopRecOn = getLoopRecEnabled();
        if (loopRecOn && dawRec && wasArmed && m_autoRecording.load(std::memory_order_relaxed)
            && currentPpq >= 0.0 && m_lastPpqPosition >= 0.0
            && currentPpq < m_lastPpqPosition - 1.0)
        {
            // Transport looped — end current take and start a new one
            m_recorder.requestStop();
            m_autoRecording.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(m_recPathMutex);
                m_currentRecordingName = {};
            }
            m_wasArmed.store(false, std::memory_order_relaxed);  // force re-trigger below
            wasArmed = false;
            m_loopTakeCount++;
            MICLOG("Loop record: pass " << m_loopTakeCount << " complete");
        }
        if (currentPpq >= 0.0) m_lastPpqPosition = currentPpq;

        if (dawRec && !wasArmed)
        {
            // DAW just started recording (or loop restarted)
            int pass = loopRecOn ? m_loopTakeCount : 0;
            juce::String fname = "take_"
                + juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S")
                + (pass > 0 ? ("_p" + juce::String(pass)) : "")
                + ".wav";
            // Ensure we have a valid save path — fall back to default if not set
            juce::String usePath = m_savePath;
            if (usePath.isEmpty())
                usePath = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                              .getChildFile("MicInput/Saves").getFullPathName();
            juce::File saveDir(usePath);
            saveDir.createDirectory();  // ensure it exists
            juce::String path  = saveDir.getChildFile(fname).getFullPathName();
            double sr = (double)m_capture.nativeSampleRate();
            if (sr <= 0.0) sr = m_sampleRate > 0.0 ? m_sampleRate : 48000.0;
            m_recorder.onFileDone = [this](juce::String p, double secs, int64_t bytes) {
                if (onRecordingDone) onRecordingDone(p, secs, bytes);
            };
            {
                std::lock_guard<std::mutex> lk(m_recPathMutex);
                m_currentRecordingName = fname;
            }
            m_recorder.start(path, sr, 2);
            m_autoRecording.store(true, std::memory_order_relaxed);
            MICLOG("Auto-save: started " << path.toStdString());
        }
        else if (!dawRec && wasArmed && m_autoRecording.load(std::memory_order_relaxed))
        {
            // Signal stop — wait-free, no blocking on audio thread.
            // The message thread (timerCallback) polls isRecording() and calls
            // finishAsync() once the writer thread has drained and exited.
            m_recorder.requestStop();
            m_autoRecording.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(m_recPathMutex);
                m_currentRecordingName = {};
            }
            MICLOG("Auto-save: stop requested");
        }
        m_wasArmed.store(dawRec, std::memory_order_relaxed);
    }

    const size_t avail       = m_ring.available();
    const size_t prebufNeed  = m_prebufFrames.load(std::memory_order_relaxed);

    // Update live ring fill display (in ms at native rate)
    float natSr_ = nativeSr.load(std::memory_order_relaxed);
    if (natSr_ > 0)
        ringFillMs.store((float)avail / natSr_ * 1000.f, std::memory_order_relaxed);

    // Wait for ring to fill to prebuf threshold
    if (avail <= prebufNeed)
    {
        if (avail == 0) underruns.fetch_add(1, std::memory_order_relaxed);
        return;  // output silence — building up buffer
    }

    // Usable frames = everything above prebuf floor
    const size_t usable = avail - prebufNeed;

    float* outL = buffer.getWritePointer(0);
    float* outR = numChannels > 1 ? buffer.getWritePointer(1) : nullptr;

    size_t framesReadFromRing = 0;  // track exact frames for recorder

    if (m_resampleRatio <= 1.001 && m_resampleRatio >= 0.999)
    {
        // ── No resampling — rates match ──────────────────────────────────────
        const size_t toRead = std::min((size_t)numSamples, usable);
        if (s_interleavedBuf.size() < toRead * 2) s_interleavedBuf.resize(toRead * 2);
        const size_t got = m_ring.read(s_interleavedBuf.data(), toRead);
        framesReadFromRing = got;
        for (size_t i = 0; i < got; ++i)
        {
            outL[i] = s_interleavedBuf[i * 2];
            if (outR) outR[i] = s_interleavedBuf[i * 2 + 1];
        }
    }
    else
    {
        // ── Resampling (e.g. 48kHz mic → 44.1kHz DAW) ────────────────────────
        const size_t srcNeeded = (size_t)std::ceil(numSamples * m_resampleRatio) + 4;
        const size_t srcToRead = std::min(usable, srcNeeded);
        if (srcToRead == 0) { underruns.fetch_add(1, std::memory_order_relaxed); return; }

        if (s_interleavedBuf.size() < srcToRead * 2 + 16)
            s_interleavedBuf.resize(srcToRead * 2 + 16);
        const size_t got = m_ring.read(s_interleavedBuf.data(), srcToRead);
        if (got == 0) return;
        framesReadFromRing = got;

        if (m_resampleBufL.size() < got + 8)
        {
            m_resampleBufL.assign(got + 8, 0.f);
            m_resampleBufR.assign(got + 8, 0.f);
        }
        for (size_t i = 0; i < got; ++i)
        {
            m_resampleBufL[i] = s_interleavedBuf[i * 2];
            m_resampleBufR[i] = s_interleavedBuf[i * 2 + 1];
        }
        for (size_t i = got; i < got + 8; ++i)
            { m_resampleBufL[i] = 0.f; m_resampleBufR[i] = 0.f; }

        m_resamplerL.process(m_resampleRatio, m_resampleBufL.data(), outL, numSamples);
        if (outR) m_resamplerR.process(m_resampleRatio, m_resampleBufR.data(), outR, numSamples);
    }

    // Latency = prebuffer + capture period + half block + output
    // This is stable and doesn't oscillate with ring fill
    // (ring fill oscillates ±captureMs which would look jittery)
    const float stableLatency = m_prebufMs.load(std::memory_order_relaxed)
        + captureMs.load(std::memory_order_relaxed)
        + blockMs.load(std::memory_order_relaxed) * 0.5f
        + 2.0f   // USB mic ADC
        + outputLatMs.load(std::memory_order_relaxed);
    measuredLatMs.store(stableLatency, std::memory_order_relaxed);

    // Ring fill diagnostic (for settings tab)
    // Clamp display to not show false high values during drain
    const float maxReasonableFill = m_prebufMs.load(std::memory_order_relaxed) * 2.f + 5.f;
    const float fill = std::min(ringFillMs.load(std::memory_order_relaxed), maxReasonableFill);
    ringFillMs.store(fill, std::memory_order_relaxed);

    // ── Input gain (Capture dial) — applied to output and recorder ──────────────
    float gain = getInputGain();
    if (gain != 1.f) {
        for (int i = 0; i < numSamples; ++i) {
            outL[i] *= gain;
            if (outR) outR[i] *= gain;
        }
        // Also apply to s_interleavedBuf so the recorded WAV has gain applied
        for (size_t i = 0; i < framesReadFromRing * 2; ++i)
            s_interleavedBuf[i] *= gain;
    }

    // ── Noise gate — applied after gain, before recorder push ─────────────────
    float gateThr = getNoiseGateThreshold();  // 0 = off, 1 = maximum suppression
    if (gateThr > 0.001f) {
        // RMS in this block
        float rms = 0.f;
        for (int i = 0; i < numSamples; ++i) rms += outL[i] * outL[i];
        rms = std::sqrt(rms / (float)std::max(1, numSamples));
        // Threshold: 0.0 = –inf, 1.0 = –20 dBFS (linear 0.1)
        float linThr = gateThr * 0.1f;
        float targetGain = (rms > linThr) ? 1.f : 0.f;
        // Smooth: ~5ms attack, ~50ms release at 48kHz block size
        const float attack  = 0.9f, release = 0.98f;
        m_gateGain += (targetGain > m_gateGain)
            ? (1.f - attack)  * (targetGain - m_gateGain)
            : (1.f - release) * (targetGain - m_gateGain);
        if (m_gateGain < 0.001f) m_gateGain = 0.f;
        float g = m_gateGain;
        for (int i = 0; i < numSamples; ++i) {
            outL[i] *= g;
            if (outR) outR[i] *= g;
        }
        for (size_t i = 0; i < framesReadFromRing * 2; ++i)
            s_interleavedBuf[i] *= g;
    }

    // ── Peak hold / clip detection ────────────────────────────────────────────
    float peak = 0.f;
    for (int i = 0; i < numSamples; ++i)
        peak = std::max(peak, std::abs(outL[i]));
    if (peak >= 0.999f) clipDetected.store(true, std::memory_order_relaxed);
    // Decay peak hold: ~3s at 30Hz repaint → handled in timerCallback
    float curPeak = peakHold.load(std::memory_order_relaxed);
    if (peak > curPeak) peakHold.store(peak, std::memory_order_relaxed);

    levelL.store(m_capture.levelL(), std::memory_order_relaxed);
    levelR.store(m_capture.levelR(), std::memory_order_relaxed);
    processCallCount.fetch_add(1, std::memory_order_relaxed);

    // Feed WAV recorder with the NATIVE-RATE captured audio.
    // We use s_interleavedBuf which holds the frames read from the ring
    // (already at nativeSr) BEFORE any resampling. This ensures the WAV
    // sample rate header matches the actual audio content exactly.
    // If rates match (ratio==1) this is identical to outL/outR.
    // If rates differ, this avoids writing resampled audio at the wrong rate.
    // Always push to the recorder — pushSamples() is a no-op when not
    // recording. Removing the isRecording() gate here ensures the final
    // partial block reaches the ring before stop() drains it.
    if (framesReadFromRing > 0)
        m_recorder.pushSamples(s_interleavedBuf.data(), framesReadFromRing);
}

// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
void MicInputProcessor::refreshDeviceList()
{
    refreshDevices();
    MICLOG("refreshDeviceList: " << m_devices.size() << " devices");
}

// ─────────────────────────────────────────────────────────────────────────────
// API key stored in per-user app properties (not project state XML)
// so it survives across different DAW sessions and projects.

void MicInputProcessor::setWhisperLanguage(const juce::String& lang)
{
    m_whisper.setLanguage(lang);
    if (auto* props = getAppProps())
    {
        props->setValue("whisper_language", lang);
        props->saveIfNeeded();
    }
}

void MicInputProcessor::setNoSpeechThold(float t)
{
    m_whisper.setNoSpeechThold(t);
    if (auto* props = getAppProps())
    {
        props->setValue("whisper_no_speech_thold", (double)t);
        props->saveIfNeeded();
    }
}

void MicInputProcessor::setWhisperModel(WhisperClient::ModelSize size)
{
    m_whisper.setModel(size);
    if (auto* props = getAppProps())
    {
        props->setValue("whisper_model", (int)size);
        props->saveIfNeeded();
    }
}

bool MicInputProcessor::startRecording(const juce::String& filePath)
{
    double sr = (double)m_capture.nativeSampleRate();
    if (sr <= 0.0) sr = m_sampleRate > 0.0 ? m_sampleRate : 48000.0;

    {
        std::lock_guard<std::mutex> lk(m_recPathMutex);
        m_currentRecordingName = juce::File(filePath).getFileName();
    }
    m_recorder.onFileDone = [this](juce::String path, double secs, int64_t bytes) {
        if (onRecordingDone) onRecordingDone(path, secs, bytes);
    };
    bool ok = m_recorder.start(filePath, sr, 2);
    MICLOG("startRecording: " << filePath.toStdString()
        << " sr=" << sr << " ok=" << ok);
    return ok;
}

void MicInputProcessor::stopRecording()
{
    // Clear auto-recording flag so processBlock doesn't try to stop again via requestStop
    m_autoRecording.store(false, std::memory_order_relaxed);
    m_wasArmed     .store(false, std::memory_order_relaxed);  // reset so next arm triggers fresh start
    {
        std::lock_guard<std::mutex> lk(m_recPathMutex);
        m_currentRecordingName = {};
    }
    m_recorder.stopAndWait();   // blocks until writer drains
    m_resamplerL.reset();
    m_resamplerR.reset();
    MICLOG("stopRecording done");
}

void MicInputProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
    {
        xml->setAttribute("deviceIndex", m_selectedDeviceIndex);
        xml->setAttribute("captureMode", m_captureMode);
        xml->setAttribute("prebufMs",    (double)m_prebufMs.load());
        xml->setAttribute("monitorOn",   m_monitorEnabled.load() ? 1 : 0);
        xml->setAttribute("monitorVol",  (double)m_monitor.getVolume());
        xml->setAttribute("savePath",    m_savePath);
        xml->setAttribute("saveEnabled", m_saveEnabled.load() ? 1 : 0);
        copyXmlToBinary(*xml, destData);
    }
}

void MicInputProcessor::setStateInformation(const void* data, int size)
{
    auto xml = getXmlFromBinary(data, size);
    if (!xml) return;
    if (xml->hasAttribute("captureMode"))
        m_captureMode = xml->getIntAttribute("captureMode", 0);
    if (xml->hasAttribute("prebufMs"))
        m_prebufMs.store((float)xml->getDoubleAttribute("prebufMs", 10.0));
    if (xml->hasAttribute("monitorVol"))
        m_monitor.setVolume((float)xml->getDoubleAttribute("monitorVol", 0.8));
    if (xml->hasAttribute("monitorOn") && xml->getIntAttribute("monitorOn", 0))
        setMonitorEnabled(true);
    if (xml->hasAttribute("deviceIndex"))
        selectDevice(xml->getIntAttribute("deviceIndex", -1));
    if (xml->hasAttribute("savePath"))
        m_savePath = xml->getStringAttribute("savePath");
    if (xml->hasAttribute("saveEnabled"))
        m_saveEnabled.store(xml->getIntAttribute("saveEnabled", 0) != 0);
}

void MicInputProcessor::setMonitorEnabled(bool on)
{
    m_monitorEnabled.store(on);
    if (on)
    {
        // Wire capture → monitor, then start
        m_capture.setDirectMonitor(&m_monitor);
        UINT32 natSr = (UINT32)m_capture.nativeSampleRate();
        if (natSr == 0) natSr = 48000;
        if (m_monitor.start(natSr, 2))
            MICLOG("Direct monitor ON: ~" << m_monitor.latencyMs() << "ms output latency");
        else
        {
            MICLOG("Direct monitor FAILED: " << m_monitor.lastError());
            m_monitorEnabled.store(false);
        }
    }
    else
    {
        m_monitor.stop();
        m_capture.setDirectMonitor(nullptr);
        MICLOG("Direct monitor OFF");
    }
}

void MicInputProcessor::setMonitorVolume(float v)
{
    m_monitor.setVolume(v);
}

juce::AudioProcessorEditor* MicInputProcessor::createEditor()
{
    return new MicInputEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MicInputProcessor();
}
