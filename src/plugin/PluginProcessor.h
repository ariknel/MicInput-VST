#pragma once
#include <JuceHeader.h>
#include "audio/WasapiCapture.h"
#include "audio/AudioRingBuffer.h"
#include "audio/DeviceProber.h"
#include "audio/DirectMonitor.h"
#include "plugin/Parameters.h"
#include "audio/WavRecorder.h"
#include "audio/WhisperClient.h"
#include <mmdeviceapi.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>

class MicInputProcessor : public juce::AudioProcessor
{
public:
    // ── Bus layout: no inputs, stereo out ────────────────────────────────────
    static BusesProperties getDefaultBuses()
    {
        return BusesProperties()
            .withOutput("Output", juce::AudioChannelSet::stereo(), true);
    }

    MicInputProcessor();
    ~MicInputProcessor() override;

    // ── AudioProcessor ────────────────────────────────────────────────────────
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MicInput"; }
    bool   acceptsMidi()  const override { return true; }  // MIDI trigger for record
    bool   producesMidi() const override { return false; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // ── Device management (message thread) ───────────────────────────────────
    std::vector<CaptureDeviceInfo> getAvailableDevices() const;
    void selectDevice(int index);
    void setMode(int mode);          // 0=shared, 1=exclusive
    void refreshDevices();

    // ── Buffer size control (message thread) ─────────────────────────────────
    // targetPrebufMs: how many ms to hold in ring before outputting
    // Lower = less latency, more risk of underruns
    // Higher = more stable, more latency
    // Range: 1ms–50ms. Default: 10ms.
    void   setPrebufMs(float ms);
    void   refreshDeviceList();   // safe to call from message thread
    float  getPrebufMs() const { return m_prebufMs.load(); }

    // ── Whisper STT (local, no API key required) ─────────────────────────────
    void setWhisperModel(WhisperClient::ModelSize size);  // defined in .cpp
    void         setWhisperLanguage(const juce::String& lang);
    juce::String getWhisperLanguage() const { return m_whisper.getLanguage(); }
    void         setNoSpeechThold(float t);
    float        getNoSpeechThold() const { return m_whisper.getNoSpeechThold(); }
    WhisperClient::ModelSize getWhisperModel() const { return m_whisper.getModel(); }
    bool hasModel()      const { return m_whisper.hasModel(); }
    bool isModelLoaded() const { return m_whisper.isModelLoaded(); }
    void unloadModel()         { m_whisper.unloadModel(); }

    bool transcribe(const juce::String& wavPath, WhisperClient::Callback cb)
    {
        return m_whisper.transcribe(wavPath, m_whisper.getLanguage().toStdString(), cb);
    }
    float getTranscribeProgress() const { return m_whisper.getProgress(); }
    bool  isTranscribing()        const { return m_whisper.isBusy(); }
    void  cancelTranscribe()            { m_whisper.cancelTranscribe(); }

    // ── WAV recording (Saves tab) ─────────────────────────────────────────────
    // Call from message thread only.
    bool  startRecording(const juce::String& filePath);
    void  stopRecording();
    bool  isRecording()        const { return m_recorder.isRecording(); }
    // Called from message thread to finalise an async-stopped recording
    void  finishRecorderAsync()       { m_recorder.finishAsync(); }
    double recordingSeconds()  const { return m_recorder.secondsRecorded(); }

    // Save path + auto-save preference (persisted in state XML)
    void        setSavePath(const juce::String& p) { m_savePath = p; }
    juce::String getSavePath()    const { return m_savePath; }
    void        setSaveEnabled(bool b) { m_saveEnabled.store(b); }
    bool        getSaveEnabled()  const { return m_saveEnabled.load(); }

    // Callback: fires on message thread when a file finishes
    std::function<void(juce::String, double, int64_t)> onRecordingDone;

    // The filename currently being recorded (set when recording starts)
    // Read by the editor to create a matching live take entry
    juce::String getCurrentRecordingName() const
    {
        std::lock_guard<std::mutex> lk(m_recPathMutex);
        return m_currentRecordingName;
    }

    // ── Direct monitor ────────────────────────────────────────────────────────
    void setMonitorEnabled(bool on);
    void setMonitorVolume(float v);
    bool isMonitorEnabled() const { return m_monitorEnabled.load(); }
    float getMonitorVolume() const { return m_monitor.getVolume(); }
    float getMonitorLatencyMs() const { return m_monitor.latencyMs(); }

    // ── Accessors ─────────────────────────────────────────────────────────────
    std::string getCaptureError()     const { return m_capture.lastError(); }
    std::string getExclusiveFailReason() const { return m_exclusiveFailReason; }
    bool hostAppliesPDC()         const { return m_hostAppliesPDC.load(); }
    int  getRecordOffsetSamples() const { return m_recordOffsetSamples.load(); }
    juce::String getHostName() const
    {
        juce::PluginHostType host;
        juce::String desc = host.getHostDescription();
        // Fallback: if JUCE returns "Unknown" try the type name
        if (desc.isEmpty() || desc == "Unknown")
            desc = "Unknown DAW";
        return desc;
    }
    std::string getThreadOptSummary() const { return m_capture.threadOptSummary(); }

    // ── Runtime state (all atomic, safe from any thread) ─────────────────────
    std::atomic<bool>     isCapturing      {false};
    std::atomic<bool>     isDawRecording   {false};  // DAW transport isRecording
    std::atomic<float>    levelL           {0.f};
    std::atomic<float>    levelR           {0.f};

    // Latency breakdown — measured live, not estimated
    std::atomic<float>    captureMs        {10.f};   // WASAPI capture period
    std::atomic<float>    blockMs          {0.f};    // set in prepareToPlay
    std::atomic<float>    outputLatMs      {1.5f};   // output DAC latency
    std::atomic<float>    prebufLatMs      {10.f};   // our ring buffer hold time
    std::atomic<float>    totalLatMs       {26.f};   // sum of all above

    // Measured live in processBlock
    std::atomic<float>    ringFillMs       {0.f};    // current ring fill in ms
    std::atomic<float>    measuredLatMs    {0.f};    // actual measured end-to-end
    std::atomic<uint64_t> underruns        {0};
    std::atomic<uint64_t> processCallCount {0};      // for FPS/timing display

    std::atomic<bool>     usedIAC3         {false};
    std::atomic<bool>     isExclusive      {false};
    std::atomic<int>      loopRecTake      {0};    // current loop-record pass counter
    std::atomic<float>    peakHold         {0.f};  // peak hold for clip indicator
    std::atomic<bool>     clipDetected     {false}; // latches on clip, cleared by editor
    std::atomic<float>    sessionBpm       {0.f};  // BPM at time of recording start

    // Gain / gate accessors (APVTS params — automatable from DAW)
    float getInputGain() const
    {
        if (auto* p = apvts.getRawParameterValue(MicInput::Params::GAIN.data()))
            return p->load();
        return 1.f;
    }
    bool getMidiRecTrigger()
    {
        // Returns and clears the trigger flag (edge-detect from MIDI)
        return m_midiRecTrigger.exchange(false, std::memory_order_relaxed);
    }
    bool getLoopRecEnabled() const
    {
        if (auto* p = apvts.getRawParameterValue(MicInput::Params::LOOP_REC.data()))
            return *p > 0.5f;
        return false;
    }
    float getNoiseGateThreshold() const
    {
        if (auto* p = apvts.getRawParameterValue(MicInput::Params::GATE.data()))
            return p->load();
        return 0.f;
    }
    std::atomic<bool>     outputIsUsb      {true};
    std::atomic<bool>     outputIsBluetooth{false};
    std::atomic<float>    nativeSr         {48000.f};
    std::atomic<float>    dawSr            {48000.f};

    DeviceProfile  currentDeviceProfile;
    OutputProfile  currentOutputProfile;

    juce::AudioProcessorValueTreeState apvts;

private:
    void openCapture();
    void closeCapture();
    void recalcTotalLatency();

    WasapiCapture    m_capture;
    DirectMonitor    m_monitor;
    AudioRingBuffer  m_ring{9600};

    std::vector<CaptureDeviceInfo> m_devices;
    mutable std::mutex             m_devicesMu;
    std::string                    m_selectedDeviceId;
    int                            m_captureMode         = 0;
    int                            m_selectedDeviceIndex = -1;

    double m_sampleRate = 0.0;
    int    m_blockSize  = 512;

    // Prebuffer: how many native frames to hold before outputting
    // Exposed to GUI. Calculated from m_prebufMs.
    std::atomic<float>   m_prebufMs     {1.f};   // default to minimum — user can increase if dropouts
    std::atomic<size_t>  m_prebufFrames {480};   // in native frames

    juce::LagrangeInterpolator m_resamplerL;
    juce::LagrangeInterpolator m_resamplerR;
    std::vector<float>         m_resampleBufL;
    std::vector<float>         m_resampleBufR;
    double                     m_resampleRatio = 1.0;

    static thread_local std::vector<float> s_interleavedBuf;
    WavRecorder   m_recorder;
    mutable std::mutex m_recPathMutex;
    juce::String       m_currentRecordingName;  // filename only, not full path
    WhisperClient m_whisper;
    juce::String m_savePath;
    std::atomic<bool> m_saveEnabled  {false};
    std::atomic<bool> m_wasArmed     {false};  // last known DAW record state
    std::atomic<bool> m_autoRecording{false};  // we started recording from auto-save

    std::string m_exclusiveFailReason;
    std::atomic<bool> m_monitorEnabled{false};
    std::atomic<bool> m_midiRecTrigger{false};  // toggled by MIDI note-on in processBlock
    // Loop-record & gate state (audio thread only)
    double             m_lastPpqPosition = -1.0;
    int                m_loopTakeCount   = 0;
    float              m_gateGain        = 0.f;
    std::atomic<bool>  m_hostAppliesPDC{false};
    std::atomic<int>   m_recordOffsetSamples{0};

    // Live latency measurement
    using Clock = std::chrono::steady_clock;
    std::atomic<int64_t>  m_lastCaptureWriteNs  {0};
    std::atomic<int64_t>  m_lastProcessReadNs   {0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MicInputProcessor)
};
