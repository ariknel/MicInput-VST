#pragma once
#include <vector>
#include "audio/WhisperClient.h"
#include <JuceHeader.h>
#include "gui/LookAndFeel.h"
#include "gui/Colours.h"
#include "gui/components/LevelMeter.h"
#include "audio/WavPlayer.h"
#include "audio/ModelDownloader.h"

class MicInputProcessor;
enum class Tab { Record, Takes, Settings };

class MicInputEditor : public juce::AudioProcessorEditor,
                       private juce::Timer,
                       private juce::ComboBox::Listener,
                       private juce::Slider::Listener
{
public:
    explicit MicInputEditor(MicInputProcessor&);
    ~MicInputEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseUp(const juce::MouseEvent&) override;

    // Folder management helpers
    void renameFolderDialog(const juce::String& folderName);
    void deleteFolderDialog(const juce::String& folderName);

private:
    void timerCallback() override;
    void comboBoxChanged(juce::ComboBox*) override;
    void sliderValueChanged(juce::Slider*) override;

    void switchTab(Tab);
    void populateDeviceCombo();

    // Paint helpers
    void paintHeader(juce::Graphics&);
    void paintNav(juce::Graphics&);
    void paintStatusBar(juce::Graphics&);
    void paintRecord(juce::Graphics&);
    void paintTakes(juce::Graphics&);
    void paintSettings(juce::Graphics&);
    void paintLyrics(juce::Graphics&);   // lyrics/transcript editor panel

    // Record tab helpers
    void drawRecordButton(juce::Graphics&, juce::Rectangle<int> bounds);
    void drawMeter(juce::Graphics&, juce::Rectangle<int> bounds);

    // Takes helpers
    int  takesListStartY() const;
    void showRenameEditor(int index, juce::Rectangle<int> bounds);
    void commitRename();
    void cancelRename();
    void scanTakesFromDisk();

    // Lyrics panel helpers
    void openLyrics(int takeIndex);
    void closeLyrics();
    void saveLyrics();
    void deleteTake(int takeIndex);

    // ── Processor ref ──────────────────────────────────────────────────────
    MicInputProcessor&   m_proc;
    MicInputLookAndFeel  m_laf;
    Tab                  m_activeTab   = Tab::Record;
    bool                 m_lyricsOpen  = false;   // lyrics panel overlays Takes
    bool                 m_capturing   = false;
    uint64_t             m_lastUnderrun = 0;

    // ── Nav buttons ────────────────────────────────────────────────────────
    juce::TextButton m_navRecord   {"Record"};
    juce::TextButton m_navTakes    {"Takes"};
    juce::TextButton m_navSettings {"Settings"};

    // ── Record tab ─────────────────────────────────────────────────────────
    juce::ComboBox   m_deviceCombo;
    juce::TextButton m_deviceRefreshBtn {"R"};
    LevelMeter       m_meter;
    juce::TextButton m_recBtn      {"REC"};
    juce::TextButton m_monitorBtn  {"Direct monitor"};
    juce::Slider     m_monitorVol;

    // ── Takes tab ──────────────────────────────────────────────────────────
    juce::TextButton m_openFolderBtn   {"Open"};
    juce::TextButton m_changeFolderBtn {"Change"};
    juce::TextButton m_autoSaveToggle  {""};

    // ── Settings tab ───────────────────────────────────────────────────────
    juce::Slider     m_prebufSlider;
    juce::Label      m_prebufValueLabel;
    // Whisper model selection (no API key needed)
    juce::TextButton m_modelTiny       {"Tiny"};
    juce::TextButton m_modelBase       {"Base"};
    juce::TextButton m_modelSmall      {"Small"};
    juce::TextButton m_modelMedium     {"Medium"};
    juce::TextButton m_modelLarge      {"Large"};
    juce::TextButton m_modelDownloadBtn{"Download"};
    juce::TextButton m_modelUnloadBtn  {"Unload"};
    juce::TextButton m_modelCancelBtn {"Cancel"};  // cancel in-flight download
    // Whisper language + quality settings
    juce::ComboBox   m_langCombo;
    juce::Slider     m_noSpeechSlider;
    juce::Label      m_noSpeechLabel;

    // ── New feature controls (Settings) ───────────────────────────────────────
    juce::Slider     m_gainSlider;      // Input gain 0-2x
    juce::Label      m_gainLabel;
    juce::Slider     m_gateSlider;      // Noise gate threshold
    juce::Label      m_gateLabel;
    juce::TextButton m_loopRecBtn  {"Loop Rec"};
    juce::ComboBox   m_midiNoteCombo;   // MIDI note to trigger record

    // ── Peak hold / clip (Record tab) ─────────────────────────────────────────
    float            m_peakHoldVal   = 0.f;
    double           m_clipLatchMs   = 0.0;   // time clip was detected
    static constexpr double kClipLatchDuration = 2000.0; // ms

    // ── Post-recording analysis ────────────────────────────────────────────────
    void runPostRecordAnalysis(int takeIndex);  // pitch detect + peaks on bg thread

    // ── Download state ─────────────────────────────────────────────────────────
    ModelDownloader  m_downloader;
    std::atomic<float>   m_downloadProgress {0.f};  // 0..1
    std::atomic<int64_t> m_downloadedBytes  {0};
    std::atomic<int64_t> m_totalBytes       {-1};
    bool             m_downloadError = false;
    juce::String     m_downloadErrorMsg;

    // ── Lyrics panel ───────────────────────────────────────────────────────
    juce::TextButton m_lyricsBackBtn   {""};   // ← back arrow
    juce::TextButton m_lyricsCopyBtn   {"Copy all"};
    juce::TextButton m_lyricsSaveBtn   {"Save"};
    juce::TextButton m_lyricsDeleteBtn {"Delete"};
    juce::TextButton m_lyricsPlayBtn   {""};   // ► / ‖ play/pause
    juce::TextButton m_lyricsTxBtn     {"Transcribe"};  // in-panel transcribe
    juce::TextEditor m_lyricsEditor;   // the big text area

    int      m_lyricsIndex   = -1;     // which take is open
    bool     m_lyricsDirty   = false;  // unsaved edits
    float    m_waveformScrub = -1.f;   // -1 = not scrubbing
    WavPlayer m_player;                // WAV playback engine

    // ── Shared state ───────────────────────────────────────────────────────
    bool         m_saveEnabled   = false;
    bool         m_pendingFinish = false;
    bool         m_isRecording   = false;
    juce::String m_savePath;

    struct Take {
        juce::String name;
        juce::String duration;
        juce::String size;
        float        latencyMs    = 0.f;
        bool         live         = false;
        juce::String transcript;
        bool         transcribing = false;
        juce::String transcriptError;
        juce::String folder;      // relative subfolder name, empty = root
        float        bpm          = 0.f;   // DAW BPM when recorded
        juce::String key;                  // detected pitch key e.g. "A3"
        std::vector<float> peaks;          // waveform thumbnail (48 bars, 0-1)
        std::vector<WhisperWord> words;    // word-level timestamps from Whisper
    };
    juce::Array<Take> m_takes;
    juce::String      m_currentFolder;     // currently browsing folder ("" = root)
    int               m_takesScrollY = 0;  // pixel scroll offset in takes list
    int               m_takeListTotalH = 0;// total painted height for scroll
    juce::TextButton  m_newFolderBtn    {"+ Folder"};
    juce::TextButton  m_folderUpBtn    {"< Back"};   // navigate up to root
    juce::TextButton  m_renameFolderBtn{"Rename"};
    juce::TextButton  m_deleteFolderBtn{"Delete"};

    int  m_dragIndex      = -1;
    int  m_clickIndex     = -1;   // take index pending open on mouseUp (deferred from mouseDown)
    int  m_dragHoverFolder = -1;  // takes[] index of folder row currently hovered during drag
    bool m_isDraggingTake  = false; // true once drag threshold exceeded on a take row
    int  m_renameIndex = -1;
    juce::TextEditor               m_renameEditor;
    std::unique_ptr<juce::FileChooser> m_fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MicInputEditor)
};
