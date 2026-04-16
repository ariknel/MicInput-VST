#include <cmath>
#include "gui/PluginEditor.h"
#include "gui/NativeFileDrag.h"
#include "plugin/PluginProcessor.h"
#include "audio/MicLog.h"
#include "audio/WhisperClient.h"
#include "audio/PitchDetector.h"
#include <vector>

// ── Layout constants ──────────────────────────────────────────────────────────
static constexpr int W   = 420;
static constexpr int H   = 500;
static constexpr int HDR = 40;   // header bar
static constexpr int NAV = 36;   // nav bar
static constexpr int STA = 22;   // status bar
static constexpr int PAD = 12;
static constexpr int BODY_Y = HDR + NAV;
static constexpr int BODY_H = H - BODY_Y - STA;

// ─────────────────────────────────────────────────────────────────────────────
MicInputEditor::MicInputEditor(MicInputProcessor& p)
    : AudioProcessorEditor(&p), m_proc(p)
{
    setSize(W, H);
    setResizable(false, false);
    setOpaque(true);
    setLookAndFeel(&m_laf);

    // Set onRecordingDone FIRST — processBlock can fire it before any UI is ready
    m_proc.onRecordingDone = [this](juce::String, double secs, int64_t bytes)
    {
        juce::MessageManager::callAsync([this, secs, bytes] {
            for (auto& t : m_takes) {
                if (t.live) {
                    t.live     = false;
                    int m2 = (int)(secs / 60.0), s2 = (int)secs % 60;
                    t.duration = juce::String::formatted("%d:%02d", m2, s2);
                    t.size     = juce::String(bytes / (1024*1024.f), 1) + " MB";
                    break;
                }
            }
            m_isRecording = false;
            m_recBtn.setToggleState(false, juce::dontSendNotification);
            scanTakesFromDisk();
            // Run pitch detection + waveform peaks on the newly finished take
            // Find it by scanning for a non-live take with no peaks yet
            for (int ti = 0; ti < (int)m_takes.size(); ++ti) {
                auto& tk = m_takes.getReference(ti);
                if (!tk.live && tk.peaks.empty() && tk.key.isEmpty())
                    runPostRecordAnalysis(ti);
            }
            repaint();
        });
    };

    // ── Nav buttons ───────────────────────────────────────────────────────────
    for (auto* b : { &m_navRecord, &m_navTakes, &m_navSettings })
    {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(1);
        addAndMakeVisible(b);
    }
    m_navRecord  .setToggleState(true, juce::dontSendNotification);
    m_navRecord  .onClick = [this] { switchTab(Tab::Record);   };
    m_navTakes   .onClick = [this] { switchTab(Tab::Takes);    };
    m_navSettings.onClick = [this] { switchTab(Tab::Settings); };

    // Nav buttons: transparent bg, underline drawn manually in paintNav
    for (auto* b : { &m_navRecord, &m_navTakes, &m_navSettings })
    {
        b->setColour(juce::TextButton::buttonColourId,   juce::Colour(0x00000000));
        b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0x00000000));
        b->setColour(juce::TextButton::textColourOffId,  MicInput::Colours::HINT);
        b->setColour(juce::TextButton::textColourOnId,   MicInput::Colours::TEXT);
    }

    // ── Device combo ──────────────────────────────────────────────────────────
    m_deviceCombo.setTextWhenNothingSelected("Select microphone...");
    m_deviceCombo.addListener(this);
    addAndMakeVisible(m_deviceCombo);

    m_deviceRefreshBtn.onClick = [this] {
        m_proc.refreshDeviceList();
        populateDeviceCombo();
    };
    addAndMakeVisible(m_deviceRefreshBtn);
    addAndMakeVisible(m_meter);

    // ── Record button ─────────────────────────────────────────────────────────
    m_recBtn.setClickingTogglesState(true);
    m_recBtn.onClick = [this] {
        bool wantsRecord = m_recBtn.getToggleState();

        if (wantsRecord)
        {
            // ── Start manual recording ─────────────────────────────────────
            // If already recording (e.g. auto-save in progress), stop it first
            if (m_isRecording || m_proc.isRecording()) {
                m_proc.stopRecording();
                m_isRecording  = false;
                m_pendingFinish = false;  // stopRecording is synchronous — no pending needed
                // Mark any live take as abandoned
                for (auto& t : m_takes) if (t.live) { t.live = false; t.size = "stopped"; }
                scanTakesFromDisk();
            }

            juce::String fname = "take_"
                + juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S") + ".wav";
            juce::String path  = juce::File(m_savePath).getChildFile(fname).getFullPathName();
            bool ok = m_proc.startRecording(path);
            if (ok) {
                m_isRecording = true;
                Take t;
                t.name      = m_proc.getCurrentRecordingName();
                if (t.name.isEmpty()) t.name = fname;
                t.duration  = "0:00";
                t.size      = "saving...";
                t.latencyMs = m_proc.measuredLatMs.load();
                t.live      = true;
                m_takes.insert(0, t);
            } else {
                m_isRecording = false;
                m_recBtn.setToggleState(false, juce::dontSendNotification);
            }
        }
        else
        {
            // ── Stop manual recording ──────────────────────────────────────
            m_isRecording   = false;
            m_pendingFinish  = false;
            m_recBtn.setEnabled(true);
            m_proc.stopRecording();
        }
        repaint();
    };

    m_recBtn.setName("REC_BTN");
    // Make record button invisible — we paint it manually in paintRecord()
    // The button is still interactive (click target) but has no visual background
    m_recBtn.setOpaque(false);
    addAndMakeVisible(m_recBtn);

    // ── Monitor ───────────────────────────────────────────────────────────────
    m_monitorBtn.setClickingTogglesState(true);
    m_monitorBtn.onClick = [this] {
        m_proc.setMonitorEnabled(m_monitorBtn.getToggleState());
    };
    addAndMakeVisible(m_monitorBtn);

    m_monitorVol.setRange(0.0, 1.0, 0.01);
    m_monitorVol.setValue(0.8, juce::dontSendNotification);
    m_monitorVol.setSliderStyle(juce::Slider::LinearHorizontal);
    m_monitorVol.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_monitorVol.onValueChange = [this] {
        m_proc.setMonitorVolume((float)m_monitorVol.getValue());
    };
    addAndMakeVisible(m_monitorVol);

    // ── Takes tab ─────────────────────────────────────────────────────────────
    m_savePath = m_proc.getSavePath();
    if (m_savePath.isEmpty())
        m_savePath = juce::File::getSpecialLocation(
            juce::File::userMusicDirectory).getChildFile("MicInput/Saves").getFullPathName();

    // Load persisted take history from previous sessions
    scanTakesFromDisk();   // populate from disk on open

    m_openFolderBtn.onClick = [this] {
        juce::File(m_savePath).createDirectory();
        juce::File(m_savePath).revealToUser();
    };
    addAndMakeVisible(m_openFolderBtn);

    m_changeFolderBtn.onClick = [this] {
        m_fileChooser = std::make_unique<juce::FileChooser>("Choose save folder",
                                                             juce::File(m_savePath));
        m_fileChooser->launchAsync(
            juce::FileBrowserComponent::canSelectDirectories,
            [this](const juce::FileChooser& fc) {
                auto r = fc.getResult();
                if (r.isDirectory()) {
                    m_savePath      = r.getFullPathName();
                    m_currentFolder = {};   // reset to root of new location
                    m_proc.setSavePath(m_savePath);
                    scanTakesFromDisk();
                    repaint();
                }
            });
    };
    addAndMakeVisible(m_changeFolderBtn);

    m_newFolderBtn.onClick = [this] {
        // Prompt for folder name via a simple AlertWindow
        auto* alert = new juce::AlertWindow("New folder",
            "Enter a name for the new folder:",
            juce::MessageBoxIconType::NoIcon);
        alert->addTextEditor("name", "", "Folder name:");
        alert->addButton("Create", 1);
        alert->addButton("Cancel", 0);
        juce::Component::SafePointer<MicInputEditor> safeThis(this);
        alert->enterModalState(true, juce::ModalCallbackFunction::create(
            [safeThis, alert](int result) {
                if (result == 1 && safeThis != nullptr) {
                    juce::String name = alert->getTextEditorContents("name").trim();
                    name = name.replaceCharacters("/:*?<>|", "________");
                    if (name.isNotEmpty()) {
                        juce::File newDir(juce::File(safeThis->m_savePath).getChildFile(name));
                        newDir.createDirectory();
                        safeThis->m_currentFolder = name;
                        safeThis->scanTakesFromDisk();
                        // Show Back/Rename/Delete, hide + Folder immediately
                        safeThis->m_newFolderBtn    .setVisible(false);
                        safeThis->m_folderUpBtn     .setVisible(true);
                        safeThis->m_renameFolderBtn .setVisible(true);
                        safeThis->m_deleteFolderBtn .setVisible(true);
                        safeThis->resized();
                        safeThis->repaint();
                    }
                }
                delete alert;
            }), false);
    };
    addAndMakeVisible(m_newFolderBtn);

    m_folderUpBtn.onClick = [this] {
        m_currentFolder = {};
        m_takesScrollY  = 0;
        scanTakesFromDisk();
        // Update button visibility: hide Back/Rename/Delete, show + Folder
        m_folderUpBtn    .setVisible(false);
        m_renameFolderBtn.setVisible(false);
        m_deleteFolderBtn.setVisible(false);
        m_newFolderBtn   .setVisible(true);
        resized();
        repaint();
    };
    addAndMakeVisible(m_folderUpBtn);

    m_renameFolderBtn.onClick = [this] { renameFolderDialog(m_currentFolder); };
    addAndMakeVisible(m_renameFolderBtn);

    m_deleteFolderBtn.setColour(juce::TextButton::textColourOffId, MicInput::Colours::RED_LT);
    m_deleteFolderBtn.onClick = [this] { deleteFolderDialog(m_currentFolder); };
    addAndMakeVisible(m_deleteFolderBtn);

    m_saveEnabled = m_proc.getSaveEnabled();
    m_autoSaveToggle.setClickingTogglesState(true);
    m_autoSaveToggle.setToggleState(m_saveEnabled, juce::dontSendNotification);
    m_autoSaveToggle.onClick = [this] {
        m_saveEnabled = m_autoSaveToggle.getToggleState();
        m_proc.setSaveEnabled(m_saveEnabled);
        repaint();
    };
    addAndMakeVisible(m_autoSaveToggle);

    // ── Settings tab ──────────────────────────────────────────────────────────
    m_prebufSlider.setRange(1.0, 50.0, 0.5);
    m_prebufSlider.setValue(m_proc.getPrebufMs(), juce::dontSendNotification);
    m_prebufSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    m_prebufSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_prebufSlider.addListener(this);
    addAndMakeVisible(m_prebufSlider);

    m_prebufValueLabel.setFont(juce::Font(juce::FontOptions(11.f)));
    m_prebufValueLabel.setColour(juce::Label::textColourId, MicInput::Colours::ACCENT_LT);
    m_prebufValueLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(m_prebufValueLabel);

    // ── Whisper model selector buttons ───────────────────────────────────────
    using MS = WhisperClient::ModelSize;
    for (auto* b : { &m_modelTiny, &m_modelBase, &m_modelSmall,
                     &m_modelMedium, &m_modelLarge })
    {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(2);
        addAndMakeVisible(b);
    }
    // Set initial toggle from persisted model
    auto setModelToggle = [&] {
        MS cur = m_proc.getWhisperModel();
        m_modelTiny  .setToggleState(cur == MS::Tiny,   juce::dontSendNotification);
        m_modelBase  .setToggleState(cur == MS::Base,   juce::dontSendNotification);
        m_modelSmall .setToggleState(cur == MS::Small,  juce::dontSendNotification);
        m_modelMedium.setToggleState(cur == MS::Medium, juce::dontSendNotification);
        m_modelLarge .setToggleState(cur == MS::Large,  juce::dontSendNotification);
    };
    setModelToggle();

    m_modelTiny  .onClick = [this] { m_proc.setWhisperModel(MS::Tiny);   repaint(); };
    m_modelBase  .onClick = [this] { m_proc.setWhisperModel(MS::Base);   repaint(); };
    m_modelSmall .onClick = [this] { m_proc.setWhisperModel(MS::Small);  repaint(); };
    m_modelMedium.onClick = [this] { m_proc.setWhisperModel(MS::Medium); repaint(); };
    m_modelLarge .onClick = [this] { m_proc.setWhisperModel(MS::Large);  repaint(); };

    m_modelDownloadBtn.onClick = [this] {
        if (m_downloader.isRunning()) return;
        m_downloadError    = false;
        m_downloadErrorMsg = {};
        m_downloadProgress.store(0.f);
        m_downloadedBytes .store(0);
        m_totalBytes      .store(-1);

        using MS = WhisperClient::ModelSize;
        MS   model = m_proc.getWhisperModel();
        juce::String url  = WhisperClient::downloadUrl(model);
        juce::File   dest = WhisperClient::modelFile(model);

        juce::Component::SafePointer<MicInputEditor> safeThis(this);

        m_downloader.start(url, dest,
            // Progress callback — fires on download thread
            [safeThis](int64_t done, int64_t total, float frac) {
                if (safeThis == nullptr) return;
                safeThis->m_downloadedBytes .store(done);
                safeThis->m_totalBytes      .store(total);
                safeThis->m_downloadProgress.store(frac);
                juce::MessageManager::callAsync([safeThis] {
                    if (safeThis != nullptr) safeThis->repaint();
                });
            },
            // Done callback — fires on download thread
            [safeThis](bool ok, juce::String err) {
                juce::MessageManager::callAsync([safeThis, ok, err] {
                    if (safeThis == nullptr) return;
                    if (ok) {
                        safeThis->m_downloadProgress.store(1.f);
                        safeThis->m_downloadError    = false;
                        safeThis->m_downloadErrorMsg = {};
                    } else if (err != "Cancelled") {
                        safeThis->m_downloadProgress.store(0.f);
                        safeThis->m_downloadError    = true;
                        safeThis->m_downloadErrorMsg = err;
                    } else {
                        // User cancelled — clear progress bar silently
                        safeThis->m_downloadProgress.store(0.f);
                        safeThis->m_downloadError    = false;
                    }
                    safeThis->resized();  // swap Download/Cancel buttons back
                    safeThis->repaint();
                });
            });

        resized();   // immediately swap Download→Cancel button
        repaint();
    };
    addAndMakeVisible(m_modelDownloadBtn);

    m_modelCancelBtn.onClick = [this] {
        m_downloader.cancel();
        m_downloadProgress.store(0.f);
        repaint();
    };
    addAndMakeVisible(m_modelCancelBtn);

    m_modelUnloadBtn.onClick = [this] { m_proc.unloadModel(); repaint(); };
    addAndMakeVisible(m_modelUnloadBtn);

    // ── Language selector ─────────────────────────────────────────────────────
    // Language codes parallel to combo box items (1-indexed)
    static const char* kLangCodes[] = {
        "auto","en","nl","de","fr","es","it","pt","ru",
        "ja","ko","zh","ar","hi","tr","pl","sv","da"
    };
    static const char* kLangLabels[] = {
        "Auto-detect","English","Dutch","German","French","Spanish",
        "Italian","Portuguese","Russian","Japanese","Korean","Chinese",
        "Arabic","Hindi","Turkish","Polish","Swedish","Danish"
    };
    static constexpr int kNumLangs = 18;

    int langId = 2;  // default English
    for (int i = 0; i < kNumLangs; ++i) {
        m_langCombo.addItem(kLangLabels[i], i + 1);
        if (m_proc.getWhisperLanguage() == juce::String(kLangCodes[i]))
            langId = i + 1;
    }
    m_langCombo.setSelectedId(langId, juce::dontSendNotification);
    m_langCombo.onChange = [this] {
        int id = m_langCombo.getSelectedId();
        if (id >= 1 && id <= kNumLangs)
            m_proc.setWhisperLanguage(kLangCodes[id - 1]);
    };
    addAndMakeVisible(m_langCombo);

    // ── No-speech threshold slider ────────────────────────────────────────────
    m_noSpeechSlider.setRange(0.0, 1.0, 0.05);
    m_noSpeechSlider.setValue(m_proc.getNoSpeechThold(), juce::dontSendNotification);
    m_noSpeechSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    m_noSpeechSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_noSpeechSlider.onValueChange = [this] {
        m_proc.setNoSpeechThold((float)m_noSpeechSlider.getValue());
        repaint();
    };
    addAndMakeVisible(m_noSpeechSlider);

    // ── Input gain slider ─────────────────────────────────────────────────────
    m_gainSlider.setRange(0.0, 2.0, 0.01);
    m_gainSlider.setValue(1.0, juce::dontSendNotification);
    m_gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    m_gainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_gainSlider.onValueChange = [this] {
        if (auto* p = m_proc.apvts.getRawParameterValue(MicInput::Params::GAIN.data()))
            p->store((float)m_gainSlider.getValue());
    };
    addAndMakeVisible(m_gainSlider);
    m_gainLabel.setFont(juce::Font(juce::FontOptions(10.f)));
    m_gainLabel.setColour(juce::Label::textColourId, MicInput::Colours::ACCENT_LT);
    m_gainLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(m_gainLabel);

    // ── Noise gate slider ─────────────────────────────────────────────────────
    m_gateSlider.setRange(0.0, 1.0, 0.01);
    m_gateSlider.setValue(0.0, juce::dontSendNotification);
    m_gateSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    m_gateSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    m_gateSlider.onValueChange = [this] {
        if (auto* p = m_proc.apvts.getRawParameterValue(MicInput::Params::GATE.data()))
            p->store((float)m_gateSlider.getValue());
    };
    addAndMakeVisible(m_gateSlider);
    m_gateLabel.setFont(juce::Font(juce::FontOptions(10.f)));
    m_gateLabel.setColour(juce::Label::textColourId, MicInput::Colours::ACCENT_LT);
    m_gateLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(m_gateLabel);

    // ── Loop record button ─────────────────────────────────────────────────────
    m_loopRecBtn.setClickingTogglesState(true);
    m_loopRecBtn.onClick = [this] {
        if (auto* p = m_proc.apvts.getRawParameterValue(MicInput::Params::LOOP_REC.data()))
            p->store(m_loopRecBtn.getToggleState() ? 1.f : 0.f);
        repaint();
    };
    addAndMakeVisible(m_loopRecBtn);

    // ── MIDI note selector ────────────────────────────────────────────────────
    m_midiNoteCombo.addItem("Off", 1);
    static const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    for (int n = 36; n <= 84; ++n) {
        juce::String label = juce::String(noteNames[n % 12]) + juce::String(n/12 - 1)
                           + " (MIDI " + juce::String(n) + ")";
        m_midiNoteCombo.addItem(label, n - 34);  // id = n-34 so "Off"=1, C2=2...
    }
    m_midiNoteCombo.setSelectedId(1, juce::dontSendNotification); // Off
    addAndMakeVisible(m_midiNoteCombo);

    m_noSpeechLabel.setFont(juce::Font(juce::FontOptions(10.f)));
    m_noSpeechLabel.setColour(juce::Label::textColourId, MicInput::Colours::ACCENT_LT);
    m_noSpeechLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(m_noSpeechLabel);

    // ── Rename editor ─────────────────────────────────────────────────────────
    m_renameEditor.setVisible(false);
    m_renameEditor.setFont(juce::Font(juce::FontOptions(11.f)));
    m_renameEditor.setColour(juce::TextEditor::backgroundColourId,  MicInput::Colours::CARD);
    m_renameEditor.setColour(juce::TextEditor::outlineColourId,     MicInput::Colours::ACCENT);
    m_renameEditor.setColour(juce::TextEditor::textColourId,        MicInput::Colours::TEXT);
    m_renameEditor.onReturnKey = [this] { commitRename(); };
    m_renameEditor.onEscapeKey = [this] { cancelRename(); };
    m_renameEditor.onFocusLost = [this] { commitRename(); };
    addChildComponent(m_renameEditor);

    // ── Lyrics panel widgets ──────────────────────────────────────────────────
    // Back button — drawn manually (arrow shape), no text needed
    m_lyricsBackBtn.setName("ICON_BTN");
    addChildComponent(m_lyricsBackBtn);

    // Copy / Save / Delete buttons
    for (auto* b : { &m_lyricsCopyBtn, &m_lyricsSaveBtn, &m_lyricsDeleteBtn })
        addChildComponent(b);

    m_lyricsCopyBtn.onClick = [this] {
        juce::SystemClipboard::copyTextToClipboard(m_lyricsEditor.getText());
    };
    m_lyricsSaveBtn.setColour(juce::TextButton::buttonColourId,  MicInput::Colours::GREEN.withAlpha(0.1f));
    m_lyricsSaveBtn.setColour(juce::TextButton::buttonOnColourId, MicInput::Colours::GREEN.withAlpha(0.1f));
    m_lyricsSaveBtn.setColour(juce::TextButton::textColourOffId,  MicInput::Colours::GREEN_LT);
    m_lyricsSaveBtn.setColour(juce::TextButton::textColourOnId,   MicInput::Colours::GREEN_LT);
    m_lyricsSaveBtn.onClick = [this] { saveLyrics(); };
    m_lyricsDeleteBtn.onClick = [this] {
        if (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_takes.size())
            deleteTake(m_lyricsIndex);
    };

    // Play button — icon-only
    m_lyricsPlayBtn.setName("ICON_BTN");
    m_lyricsPlayBtn.onClick = [this] {
        if (m_player.isPlaying()) m_player.pause();
        else                      m_player.play();
        repaint();
    };
    addChildComponent(m_lyricsPlayBtn);

    // Transcribe button inside lyrics panel
    m_lyricsTxBtn.onClick = [this] {
        if (m_lyricsIndex < 0 || m_lyricsIndex >= (int)m_takes.size()) return;
        auto& t = m_takes.getReference(m_lyricsIndex);
        if (t.transcribing || !m_proc.hasModel()) return;
        t.transcribing = true; repaint();
        juce::String wavPath = juce::File(m_savePath).getChildFile(t.name).getFullPathName();
        int idx = m_lyricsIndex;
        m_proc.transcribe(wavPath, [this, idx](juce::String text, juce::String err,
                                              std::vector<WhisperWord> words) {
            juce::MessageManager::callAsync([this, idx, text, err, words] {
                if (idx < (int)m_takes.size()) {
                    auto& t2 = m_takes.getReference(idx);
                    t2.transcribing = false;
                    if (err.isEmpty()) {
                        t2.transcript = text;
                        t2.words      = words;
                        // Build sidecar with metadata header
                        juce::String header;
                        if (t2.bpm > 0.f)        header += "bpm: " + juce::String((int)t2.bpm) + "\n";
                        if (t2.key.isNotEmpty()) header += "key: " + t2.key + "\n";
                        if (header.isNotEmpty())  header += "\n";
                        juce::File af = m_currentFolder.isEmpty()
                            ? juce::File(m_savePath)
                            : juce::File(m_savePath).getChildFile(m_currentFolder);
                        af.getChildFile(t2.name).withFileExtension("txt")
                          .replaceWithText(header + text);
                        if (m_lyricsOpen && m_lyricsIndex == idx)
                            m_lyricsEditor.setText(text, false);
                    } else {
                        t2.transcriptError = err;
                    }
                }
                repaint();
            });
        });
    };
    addChildComponent(m_lyricsTxBtn);

    // Back button action
    m_lyricsBackBtn.onClick = [this] { closeLyrics(); };

    // Lyrics text editor — large, multi-line
    m_lyricsEditor.setMultiLine(true, true);
    m_lyricsEditor.setReturnKeyStartsNewLine(true);
    m_lyricsEditor.setScrollbarsShown(true);
    m_lyricsEditor.setFont(juce::Font(juce::FontOptions(13.f)));
    m_lyricsEditor.setColour(juce::TextEditor::backgroundColourId,  juce::Colour(0xff1a1d24));
    m_lyricsEditor.setColour(juce::TextEditor::textColourId,        juce::Colour(0xd0e8eaf0));
    m_lyricsEditor.setColour(juce::TextEditor::outlineColourId,     juce::Colour(0x1affffff));
    m_lyricsEditor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0x663b82f6));
    // caretColourId removed in JUCE 8 — caret uses CaretComponent::caretColourId
    m_lyricsEditor.setColour(juce::CaretComponent::caretColourId,   juce::Colour(0xff3b82f6));
    m_lyricsEditor.setColour(juce::TextEditor::highlightColourId,   juce::Colour(0x333b82f6));
    m_lyricsEditor.onTextChange = [this] { m_lyricsDirty = true; repaint(); };
    addChildComponent(m_lyricsEditor);

    populateDeviceCombo();
    m_deviceCombo.setSelectedId(1, juce::dontSendNotification);
    switchTab(Tab::Record);
    startTimerHz(30);
}

MicInputEditor::~MicInputEditor()
{
    stopTimer();
    m_player.stop();   // stop background WavPlayer thread before destructor exits
    m_deviceCombo.removeListener(this);
    m_prebufSlider.removeListener(this);
    setLookAndFeel(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::resized()
{
    const int tabW = W / 3;
    m_navRecord  .setBounds(0,       HDR, tabW,      NAV);
    m_navTakes   .setBounds(tabW,    HDR, tabW,      NAV);
    m_navSettings.setBounds(tabW*2,  HDR, W-tabW*2,  NAV);

    if (m_activeTab == Tab::Record)
    {
        int y  = BODY_Y + PAD;
        int cw = W - PAD * 2;

        // Device row
        m_deviceRefreshBtn.setBounds(W - PAD - 28, y, 28, 28);
        m_deviceCombo.setBounds(PAD, y, cw - 32, 28); y += 36;

        // Meter (slim, full width)
        m_meter.setBounds(PAD + 44, y, cw - 44, 8); y += 18;

        // Record button zone — full zone is the click target
        const int recZoneH = 140;
        m_recBtn.setBounds(PAD, y, cw, recZoneH);  // full zone = click anywhere
        y += recZoneH + 6;

        // Monitor button full width
        const int monY = H - STA - 32 - 6 - 26 - PAD;
        m_monitorBtn.setBounds(PAD, monY, cw, 26);
        m_monitorVol.setBounds(PAD, monY + 30, cw, 26);
    }
    else if (m_activeTab == Tab::Takes)
    {
        int y  = BODY_Y + PAD;
        int cw = W - PAD * 2;

        // Path row: Open | Change buttons
        m_openFolderBtn  .setBounds(W - PAD - 56,       y, 56, 26);
        m_changeFolderBtn.setBounds(W - PAD - 56 - 60,  y, 58, 26);
        y += 34;

        // Auto-save toggle hit area
        m_autoSaveToggle.setBounds(W - PAD - 46, y - 2, 46, 28);
        // Loop record button left of auto-save label
        m_loopRecBtn.setBounds(W - PAD - 46 - 78 - 4, y - 2, 76, 24);
        // meter
        m_meter.setBounds(PAD + 44, y + 32, cw - 44, 8);
        // Folder breadcrumb row — y = BODY_Y+PAD+34+30+18 = 170, h = 24
        // Root view:      [              ] [+ Folder]
        // Subfolder view: [< Back] [name…] [Rename] [Delete]
        const int folderRowY = BODY_Y + PAD + 34 + 30 + 18;  // 170
        if (m_currentFolder.isEmpty()) {
            // Root: only "+ Folder" on the right
            m_newFolderBtn  .setBounds(W - PAD - 72, folderRowY, 70, 20);
            m_folderUpBtn   .setBounds(0, -40, 1, 1);  // off-screen
            m_renameFolderBtn.setBounds(0, -40, 1, 1);
            m_deleteFolderBtn.setBounds(0, -40, 1, 1);
        } else {
            // Subfolder: Back | <name> | Rename | Delete
            m_folderUpBtn    .setBounds(PAD,               folderRowY, 58, 20);
            m_renameFolderBtn.setBounds(W - PAD - 124,     folderRowY, 58, 20);
            m_deleteFolderBtn.setBounds(W - PAD - 62,      folderRowY, 58, 20);
            m_newFolderBtn   .setBounds(0, -40, 1, 1);  // off-screen
        }
    }
    else // Settings
    {
        int y  = BODY_Y + PAD + 14;
        int cw = W - PAD * 2;
        y += 34;  // input mode pills
        y += 14;  // "BUFFER LATENCY" section label
        m_prebufSlider    .setBounds(PAD,       y, cw - 60, 26);
        m_prebufValueLabel.setBounds(W-PAD-56,  y,      56, 26);
        y += 28;  // slider height (matches paintSettings)
        y += 28;  // preset chips height
        y += 14;  // "RECORD OFFSET" section label
        y += 42;  // record offset pill height
        y += 14;  // "WHISPER TRANSCRIPTION" section label
        // 5 model pill buttons — placed at current y (tracks paint exactly)
        const int pilW = (cw - 4) / 5;
        m_modelTiny  .setBounds(PAD + pilW*0, y, pilW-2, 24);
        m_modelBase  .setBounds(PAD + pilW*1, y, pilW-2, 24);
        m_modelSmall .setBounds(PAD + pilW*2, y, pilW-2, 24);
        m_modelMedium.setBounds(PAD + pilW*3, y, pilW-2, 24);
        m_modelLarge .setBounds(PAD + pilW*4, y, pilW-2, 24);
        // (y after pills = 276+24 = 300; status line follows in paint)

        // ── Fixed-position widgets (all anchored from top of fixed area) ────────
        // Download btn at y=336 (just after model status line in paint).
        // Rows below are compacted to fit within the 478px body.
        const int downloadY = 336;
        const int halfW     = (cw - 8) / 2;

        bool downloading = m_downloader.isRunning();
        if (downloading) {
            m_modelDownloadBtn.setBounds(PAD,          downloadY, cw - 60, 24);
            m_modelCancelBtn  .setBounds(W - PAD - 56, downloadY, 54,      24);
        } else {
            m_modelDownloadBtn.setBounds(PAD,          downloadY, cw,      24);
            m_modelCancelBtn  .setBounds(0, -40, 1, 1);
        }
        m_modelUnloadBtn.setBounds(0, -40, 1, 1);

        // Row 1 (y=382): Language left | Noise Gate threshold right
        m_langCombo     .setBounds(PAD,              382, halfW - 38, 24);
        m_noSpeechSlider.setBounds(PAD + halfW + 4,  382, halfW - 38, 24);
        m_noSpeechLabel .setBounds(W - PAD - 36,     382, 34,         24);

        // Row 2 (y=424): Input Gain left | MIDI trigger right
        m_gainSlider    .setBounds(PAD,              424, halfW - 38, 24);
        m_gainLabel     .setBounds(PAD + halfW - 36, 424, 34,         24);
        m_midiNoteCombo .setBounds(PAD + halfW + 4,  424, halfW,      24);

        // Gate slider is the noSpeechSlider — gateSlider/gateLabel hidden
        m_gateSlider    .setBounds(0, -40, 1, 1);
        m_gateLabel     .setBounds(0, -40, 1, 1);
    }

    // ── Lyrics panel layout (overlays the whole body when open) ──────────────
    if (m_lyricsOpen)
    {
        const int cw = W - PAD * 2;
        // Header row: back | title ... | copy
        // Save button is in toolbar only (avoids double-setBounds conflict)
        m_lyricsBackBtn  .setBounds(PAD,           BODY_Y + 7,  26, 26);
        m_lyricsCopyBtn  .setBounds(W - PAD - 62,  BODY_Y + 8,  50, 22);

        int y = BODY_Y + NAV + PAD;  // below fake header

        // Audio strip: play btn + waveform scrub area
        m_lyricsPlayBtn  .setBounds(PAD, y, 30, 30);
        // waveform area is painted — no JUCE component needed
        y += 38;

        // Transcribe bar (only if no transcript)
        bool hasTx = (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_takes.size()
                      && m_takes[m_lyricsIndex].transcript.isNotEmpty());
        if (!hasTx && m_proc.hasModel()) {
            m_lyricsTxBtn.setBounds(W - PAD - 80, y + 6, 76, 20);
            y += 36;
        } else {
            m_lyricsTxBtn.setBounds(0, -40, 1, 1);  // hide off-screen
        }

        // LYRICS label takes 14px, then the editor starts
        y += 14;  // matches paintLyrics y += 14 after drawing "LYRICS" label

        const int toolbarH = 34;
        const int editorH  = H - STA - toolbarH - y - 2;
        m_lyricsEditor   .setBounds(PAD, y, cw, editorH);
        y += editorH + 2;

        // Toolbar buttons
        m_lyricsDeleteBtn.setBounds(W - PAD - 178, y + 6, 50, 22);
        m_lyricsSaveBtn  .setBounds(W - PAD - 80,  y + 6, 78, 22);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paint(juce::Graphics& g)
{
    using namespace MicInput::Colours;

    // ── Base background ───────────────────────────────────────────────────────
    g.fillAll(BG);

    // ── Subtle dot-grid texture over body area ────────────────────────────────
    {
        const int gridSpacing = 18;
        const int bodyTop = BODY_Y, bodyBot = H - STA;
        g.setColour(juce::Colour(0x08ffffff));
        for (int x = gridSpacing; x < W; x += gridSpacing)
            for (int y = bodyTop + gridSpacing; y < bodyBot; y += gridSpacing)
                g.fillRect(x, y, 1, 1);
    }

    // ── Radial vignette — darkens corners for depth ───────────────────────────
    {
        juce::ColourGradient vg(juce::Colour(0x00000000), W * 0.5f, H * 0.5f,
                                 juce::Colour(0x40000000), 0.f, 0.f, true);
        g.setGradientFill(vg);
        g.fillRect(0, BODY_Y, W, H - BODY_Y - STA);
    }

    paintHeader(g);
    paintNav(g);
    paintStatusBar(g);

    if (m_lyricsOpen)
        paintLyrics(g);
    else if (m_activeTab == Tab::Record)   paintRecord(g);
    else if (m_activeTab == Tab::Takes)    paintTakes(g);
    else                                   paintSettings(g);

    // ── During transcription: dim locked tabs (Record + Settings) ──────────────
    if (m_proc.isTranscribing()) {
        const int tabW2 = W / 3;
        // Dim Record tab (index 0) and Settings tab (index 2)
        g.setColour(juce::Colour(0x70000000));
        g.fillRect(0,         HDR, tabW2,     NAV);  // Record
        g.fillRect(tabW2 * 2, HDR, W-tabW2*2, NAV);  // Settings
        // Small lock indicator on each dimmed tab
        g.setColour(HINT.withAlpha(0.5f));
        g.setFont(juce::Font(juce::FontOptions(8.f)));
        g.drawText("transcribing...", tabW2, HDR, tabW2, NAV, juce::Justification::centred);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paintHeader(juce::Graphics& g)
{
    using namespace MicInput::Colours;

    // Gradient header: deep black → slightly blue-tinted
    juce::ColourGradient hdr(juce::Colour(0xff070a10), 0.f, 0.f,
                              juce::Colour(0xff0d1018), 0.f, (float)HDR, false);
    g.setGradientFill(hdr);
    g.fillRect(0, 0, W, HDR);

    // Subtle bottom edge glow line (blue when capturing)
    bool ok = m_capturing && !m_proc.getCaptureError().length();
    juce::Colour lineCol = m_isRecording ? RED
                         : ok            ? ACCENT.withAlpha(0.6f)
                                         : BORDER;
    g.setColour(lineCol);
    g.fillRect(0, HDR - 1, W, 1);

    // Status dot with soft outer ring
    float dotCy = HDR * 0.5f;
    juce::Colour dotCol = m_isRecording ? RED : (ok ? GREEN : juce::Colour(0xff666677));
    g.setColour(dotCol.withAlpha(0.2f));
    g.fillEllipse(10.f, dotCy - 6.f, 12.f, 12.f);
    g.setColour(dotCol);
    g.fillEllipse(12.f, dotCy - 4.f, 8.f, 8.f);

    // Title — tracking-spaced uppercase
    g.setColour(TEXT);
    g.setFont(juce::Font(juce::FontOptions(12.f, juce::Font::bold)));
    g.drawText("MicInput", 28, 0, 100, HDR, juce::Justification::centredLeft);

    // Device info
    juce::String info;
    if (m_capturing)
        info = m_deviceCombo.getText() + "  â  "
             + juce::String((int)m_proc.dawSr.load() / 1000) + " kHz";
    g.setColour(HINT);
    g.setFont(juce::Font(juce::FontOptions(10.f)));
    g.drawText(info, 136, 0, W - 180, HDR, juce::Justification::centredLeft);

    // Version badge
    auto vBadge = juce::Rectangle<float>((float)(W - 44), (HDR - 14) * 0.5f, 36.f, 14.f);
    g.setColour(BORDER);
    g.drawRoundedRectangle(vBadge, 3.f, 0.5f);
    g.setColour(HINT);
    g.setFont(juce::Font(juce::FontOptions(9.f)));
    g.drawText("v0.3", (int)vBadge.getX(), (int)vBadge.getY(),
               (int)vBadge.getWidth(), (int)vBadge.getHeight(),
               juce::Justification::centred);
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paintNav(juce::Graphics& g)
{
    using namespace MicInput::Colours;

    // Nav bar — slightly lighter than header
    juce::ColourGradient nav(juce::Colour(0xff0c0f17), 0.f, (float)HDR,
                              juce::Colour(0xff0a0d14), 0.f, (float)(HDR+NAV), false);
    g.setGradientFill(nav);
    g.fillRect(0, HDR, W, NAV);

    // Bottom separator
    g.setColour(BORDER);
    g.drawLine(0.f, (float)(HDR + NAV), (float)W, (float)(HDR + NAV), 1.f);

    // Active tab: pill highlight + coloured bottom bar
    const int tabW = W / 3;
    Tab tabs[] = { Tab::Record, Tab::Takes, Tab::Settings };
    Tab drawActive = m_lyricsOpen ? Tab::Takes : m_activeTab;
    for (int i = 0; i < 3; ++i) {
        if (tabs[i] == drawActive) {
            juce::Colour ac = (tabs[i] == Tab::Record && m_isRecording) ? RED : ACCENT;
            // Pill background
            auto pill = juce::Rectangle<float>((float)(i*tabW + 6), (float)(HDR + 4),
                                               (float)(tabW - 12), (float)(NAV - 8));
            g.setColour(ac.withAlpha(0.10f));
            g.fillRoundedRectangle(pill, 5.f);
            g.setColour(ac.withAlpha(0.25f));
            g.drawRoundedRectangle(pill.reduced(0.5f), 5.f, 0.5f);
            // Bottom glow bar
            g.setColour(ac);
            g.fillRect(i * tabW + 6, HDR + NAV - 2, tabW - 12, 2);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paintStatusBar(juce::Graphics& g)
{
    using namespace MicInput::Colours;
    const int sy = H - STA;
    // Match header gradient style
    juce::ColourGradient sb(juce::Colour(0xff0c0f16), 0.f, (float)sy,
                             juce::Colour(0xff090c12), 0.f, (float)(sy+STA), false);
    g.setGradientFill(sb);
    g.fillRect(0, sy, W, STA);
    g.setColour(BORDER);
    g.drawLine(0.f, (float)sy, (float)W, (float)sy, 1.f);

    juce::Colour dotCol = GREEN;
    juce::String msg;

    if (!m_capturing) {
        dotCol = juce::Colour(0xff555566);
        msg = m_proc.getCaptureError().length()
            ? juce::String(m_proc.getCaptureError().c_str())
            : "Not capturing";
    } else if (m_isRecording || m_proc.isRecording()) {
        dotCol = RED;
        juce::String fname;
        for (auto& t : m_takes) if (t.live) { fname = t.name; break; }
        msg = "Recording  -  " + fname;
    } else if (m_proc.underruns.load() > m_lastUnderrun) {
        dotCol = RED;
        msg = "Underrun - increase buffer";
    } else if (std::abs(m_proc.nativeSr.load() - m_proc.dawSr.load()) > 1.f) {
        dotCol = AMBER;
        msg = "SR mismatch - set DAW to " + juce::String((int)m_proc.nativeSr.load()) + " Hz";
    } else {
        int frames = (int)(m_proc.blockMs.load() * m_proc.dawSr.load() / 1000.f);
        msg = "Capturing  -  "
            + juce::String((int)m_proc.dawSr.load() / 1000) + " kHz  -  "
            + juce::String(frames) + " frames  -  "
            + (m_proc.isExclusive.load() ? "Exclusive" : "Shared");
    }
    if (m_proc.isDawRecording.load() && !m_isRecording)
        msg = "[DAW REC]  " + msg;

    g.setColour(dotCol);
    g.fillEllipse((float)PAD - 2.f, sy + (STA - 7) * 0.5f, 7.f, 7.f);
    g.setFont(juce::Font(juce::FontOptions(10.f)));
    g.setColour(HINT);
    g.drawText(msg, PAD + 10, sy + 3, W - PAD*2 - 10, STA - 6,
                juce::Justification::centredLeft);
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paintRecord(juce::Graphics& g)
{
    using namespace MicInput::Colours;
    const int cw = W - PAD * 2;
    int y = BODY_Y + PAD;

    // "MICROPHONE" label
    g.setColour(HINT);
    g.setFont(juce::Font(juce::FontOptions(9.f)));
    g.drawText("MICROPHONE", PAD, y, cw, 12, juce::Justification::centredLeft);
    y += 14;

    // (device combo + refresh button drawn by JUCE at y)
    y += 36;

    // dB label left of meter
    {
        float lvl = std::max(m_proc.levelL.load(), m_proc.levelR.load());
        float db  = lvl > 0.0001f ? 20.f * std::log10(lvl) : -90.f;
        juce::String dbStr = db < -60.f ? "-inf" : (juce::String(db, 0) + " dB");
        g.setColour(HINT);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.drawText(dbStr, PAD, y, 40, 8, juce::Justification::centredLeft);
    }
    y += 18;

    // ── Record button zone ────────────────────────────────────────────────────
    const int recZoneH = 140;
    const int recZoneY = y;
    // Zone background — gradient from deep card to slightly lighter
    {
        juce::ColourGradient zg(juce::Colour(0xff13161f), (float)(W/2), (float)recZoneY,
                                 juce::Colour(0xff0f1219), (float)(W/2), (float)(recZoneY+recZoneH), false);
        g.setGradientFill(zg);
        g.fillRoundedRectangle((float)PAD, (float)recZoneY, (float)cw, (float)recZoneH, 10.f);
    }
    // Border — glows red when recording
    if (m_isRecording)
        g.setColour(RED.withAlpha(0.35f));
    else
        g.setColour(BORDER);
    g.drawRoundedRectangle((float)PAD + 0.5f, (float)recZoneY + 0.5f,
                            (float)cw - 1.f, (float)recZoneH - 1.f, 10.f, 1.f);

    const int cx = W / 2;
    const int cy = recZoneY + recZoneH / 2;

    // Outer glow rings when recording
    if (m_isRecording) {
        g.setColour(RED.withAlpha(0.06f));
        g.fillEllipse((float)(cx - 52), (float)(cy - 52), 104.f, 104.f);
        g.setColour(RED.withAlpha(0.12f));
        g.fillEllipse((float)(cx - 44), (float)(cy - 44), 88.f, 88.f);
    }

    // Outer ring
    g.setColour(m_isRecording ? RED.withAlpha(0.6f) : BORDER2);
    g.drawEllipse((float)(cx - 38), (float)(cy - 38), 76.f, 76.f,
                   m_isRecording ? 1.5f : 1.f);

    // Button fill circle
    {
        juce::ColourGradient bg(
            m_isRecording ? RED.darker(0.2f) : juce::Colour(0xff1e2330),
            (float)(cx - 20), (float)(cy - 20),
            m_isRecording ? RED.darker(0.5f) : juce::Colour(0xff141720),
            (float)(cx + 20), (float)(cy + 20), false);
        g.setGradientFill(bg);
        g.fillEllipse((float)(cx - 28), (float)(cy - 28), 56.f, 56.f);
    }
    g.setColour(m_isRecording ? RED.withAlpha(0.8f) : BORDER2);
    g.drawEllipse((float)(cx - 28), (float)(cy - 28), 56.f, 56.f, 1.f);

    // Inner icon
    if (m_isRecording) {
        g.setColour(juce::Colours::white);
        g.fillRoundedRectangle((float)(cx - 9), (float)(cy - 9), 18.f, 18.f, 3.f);
    } else {
        g.setColour(RED.withAlpha(0.9f));
        g.fillEllipse((float)(cx - 10), (float)(cy - 10), 20.f, 20.f);
    }

    // Timer / label below button
    y = recZoneY + recZoneH - 28;
    if (m_isRecording) {
        double secs = m_proc.recordingSeconds();
        int mins = (int)(secs / 60.0), sec2 = (int)secs % 60;
        juce::String timer = juce::String::formatted("%d:%02d", mins, sec2);

        // "RECORDING" above circle
        g.setColour(RED_LT);
        g.setFont(juce::Font(juce::FontOptions(10.f, juce::Font::bold)));
        g.drawText("RECORDING", W/2 - 60, cy - 50, 120, 14, juce::Justification::centred);

        // Timer below circle
        g.setColour(TEXT);
        g.setFont(juce::Font(juce::FontOptions(22.f, juce::Font::plain)));
        g.drawText(timer, W/2 - 60, cy + 38, 120, 26, juce::Justification::centred);
    } else {
        // "PRESS TO RECORD" below circle
        g.setColour(HINT);
        g.setFont(juce::Font(juce::FontOptions(10.f)));
        g.drawText("PRESS TO RECORD", W/2 - 70, cy + 38, 140, 16, juce::Justification::centred);
    }
    y = recZoneY + recZoneH + 6;

    // ── Clip indicator + peak hold on meter ──────────────────────────────────
    {
        const int meterX = PAD + 44, meterW = cw - 44, meterY = BODY_Y + PAD + 14 + 36 + 4;
        // Peak hold line
        if (m_peakHoldVal > 0.02f) {
            int px = meterX + (int)(m_peakHoldVal * (float)meterW);
            g.setColour(ACCENT_LT.withAlpha(0.7f));
            g.fillRect(px - 1, meterY, 2, 8);
        }
        // Clip badge
        bool clipping = (juce::Time::getMillisecondCounterHiRes() - m_clipLatchMs) < kClipLatchDuration;
        if (clipping) {
            auto badge = juce::Rectangle<float>((float)(W - PAD - 34), (float)(meterY - 1), 32.f, 10.f);
            g.setColour(RED);
            g.fillRoundedRectangle(badge, 3.f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(7.f, juce::Font::bold)));
            g.drawText("CLIP", (int)badge.getX(), (int)badge.getY(),
                       (int)badge.getWidth(), (int)badge.getHeight(), juce::Justification::centred);
        }
    }

    // ── Info chips row ────────────────────────────────────────────────────────
    float ms = m_proc.measuredLatMs.load();
    struct Chip { juce::String label; juce::String val; };
    Chip chips[3];
    if (m_isRecording) {
        juce::String fname, sz;
        for (auto& t : m_takes) if (t.live) { fname = t.name; sz = t.size; break; }
        chips[0] = {"File", fname.isEmpty() ? "-" : fname.fromLastOccurrenceOf("_", false, false)};
        chips[1] = {"Size", sz.isEmpty() ? "..." : sz};
        chips[2] = {"Latency", juce::String(ms, 1) + " ms"};
    } else {
        chips[0] = {"Latency", juce::String(ms, 1) + " ms"};
        chips[1] = {"Mode", m_proc.isExclusive.load() ? "Exclusive" : "Shared"};
        // Offset: negative because clips are delayed by this many samples
        int offSmp = m_proc.getRecordOffsetSamples();
        chips[2] = {"Offset", "-" + juce::String(offSmp) + " smp"};
    }

    float chipW = (float)(cw) / 3.f;
    for (int i = 0; i < 3; ++i) {
        auto cb = juce::Rectangle<float>((float)PAD + chipW * i, (float)y, chipW - 4.f, 38.f);
        g.setColour(CARD);
        g.fillRoundedRectangle(cb, 6.f);
        g.setColour(BORDER);
        g.drawRoundedRectangle(cb.reduced(0.5f), 6.f, 1.f);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.setColour(HINT);
        g.drawText(chips[i].label, (int)cb.getX() + 2, y + 5,
                    (int)cb.getWidth() - 4, 11, juce::Justification::centred);
        g.setFont(juce::Font(juce::FontOptions(11.f)));
        g.setColour(TEXT);
        g.drawText(chips[i].val, (int)cb.getX() + 2, y + 18,
                    (int)cb.getWidth() - 4, 14, juce::Justification::centred, true);
    }
    y += 46;

    // ── Offset warning (if manual DAW needed) ────────────────────────────────
    if (!m_proc.hostAppliesPDC()) {
        auto& p = static_cast<MicInputProcessor&>(*getAudioProcessor());
        juce::String host = p.getHostName();
        juce::String instr;
        if      (host.containsIgnoreCase("Bitwig"))  instr = "Track inspector (I) - Record Offset";
        else if (host.containsIgnoreCase("Ableton")) instr = "Arrangement - clip - Offset";
        else if (host.containsIgnoreCase("FL"))      instr = "Track properties - Latency";
        else                                          instr = host + ": set record offset manually";

        juce::Rectangle<float> pill((float)PAD, (float)y, (float)cw, 30.f);
        g.setColour(AMBER.withAlpha(0.08f));
        g.fillRoundedRectangle(pill, 5.f);
        g.setColour(AMBER.withAlpha(0.35f));
        g.drawRoundedRectangle(pill.reduced(0.5f), 5.f, 1.f);
        g.setFont(juce::Font(juce::FontOptions(10.f, juce::Font::bold)));
        g.setColour(AMBER_LT);
        g.drawText(juce::String(host + "  -  set -")
                    + juce::String(m_proc.getRecordOffsetSamples()) + " samples",
                    PAD + 8, y + 4, cw - 16, 13, juce::Justification::centredLeft);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.setColour(HINT);
        g.drawText(instr, PAD + 8, y + 17, cw - 16, 11, juce::Justification::centredLeft);
        y += 36;
    }

    // ── Monitor section ───────────────────────────────────────────────────────
    const int monY = H - STA - 32 - 6 - 26 - PAD;
    bool monOn = m_proc.isMonitorEnabled();

    // Monitor button label drawn by LookAndFeel — but we override text here
    // by reprogramming button text in timer
    // Vol label
    if (monOn) {
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.setColour(HINT);
        g.drawText("Vol", PAD, monY + 36, 26, 14, juce::Justification::centredLeft);
        g.setColour(ACCENT_LT);
        g.drawText("~" + juce::String(m_proc.getMonitorLatencyMs(), 0) + "ms",
                    W - PAD - 40, monY + 36, 38, 14, juce::Justification::centredRight);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paintTakes(juce::Graphics& g)
{
    using namespace MicInput::Colours;
    const int cw = W - PAD * 2;
    int y = BODY_Y + PAD;

    // Path row background
    {
        auto box = juce::Rectangle<float>((float)PAD, (float)y, (float)cw, 26.f);
        g.setColour(CARD);
        g.fillRoundedRectangle(box, 5.f);
        g.setColour(BORDER2);
        g.drawRoundedRectangle(box.reduced(0.5f), 5.f, 1.f);

        // Truncate path from left
        juce::String dp = m_savePath;
        g.setFont(juce::Font(juce::FontOptions(10.f)));
        const int pathW = cw - 130;
        {
            juce::GlyphArrangement ga;
            ga.addLineOfText(g.getCurrentFont(), dp, 0.f, 0.f);
            while (dp.length() > 4 && (int)ga.getBoundingBox(0,-1,true).getWidth() > pathW) {
                dp = dp.substring(1);
                ga.clear();
                ga.addLineOfText(g.getCurrentFont(), "..." + dp, 0.f, 0.f);
            }
            if (dp != m_savePath) dp = "..." + dp;
        }
        g.setColour(DIM);
        g.drawText(dp, PAD + 8, y + 1, pathW, 24, juce::Justification::centredLeft, false);
        // Open / Change buttons drawn by JUCE at right side
    }
    y += 34;

    // Auto-save row
    g.setFont(juce::Font(juce::FontOptions(11.f)));
    g.setColour(DIM);
    g.drawText("Auto-save every recording", PAD, y + 4, 220, 18,
                juce::Justification::centredLeft);
    // iOS toggle pill
    {
        const int tx = W - PAD - 46, ty = y + 2;
        if (m_saveEnabled) {
            g.setColour(ACCENT);
            g.fillRoundedRectangle((float)tx, (float)ty, 44.f, 22.f, 11.f);
            g.setColour(juce::Colours::white);
            g.fillEllipse((float)(tx + 22 + 2), (float)(ty + 2), 18.f, 18.f);
        } else {
            g.setColour(BORDER2);
            g.fillRoundedRectangle((float)tx, (float)ty, 44.f, 22.f, 11.f);
            g.setColour(juce::Colours::white);
            g.fillEllipse((float)(tx + 2), (float)(ty + 2), 18.f, 18.f);
        }
    }
    y += 30;

    // Meter strip
    {
        float lvl = std::max(m_proc.levelL.load(), m_proc.levelR.load());
        float db  = lvl > 0.0001f ? 20.f * std::log10(lvl) : -90.f;
        juce::String dbStr = db < -60.f ? "-inf" : (juce::String(db, 0) + " dB");
        g.setColour(HINT);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.drawText(dbStr, PAD, y, 40, 8, juce::Justification::centredLeft);
    }
    y += 18;

    // ── Folder breadcrumb row ─────────────────────────────────────────────────
    // Buttons (Back / Rename / Delete / + Folder) are JUCE widgets placed by
    // resized() — we only paint the current folder name in the middle.
    if (m_currentFolder.isNotEmpty()) {
        g.setFont(juce::Font(juce::FontOptions(10.f, juce::Font::bold)));
        g.setColour(ACCENT_LT);
        // Name sits between Back btn (left, 60px) and action btns (right, 140px)
        g.drawText(m_currentFolder, PAD + 64, y + 3, cw - 64 - 144, 18,
                    juce::Justification::centredLeft, true);
    }
    y += 24;

    // ── Takes list (scrollable) ───────────────────────────────────────────────
    if (m_takes.isEmpty()) {
        g.setColour(HINT);
        g.setFont(juce::Font(juce::FontOptions(12.f)));
        g.drawText("No recordings yet", PAD, y + 20, cw, 20, juce::Justification::centred);
        g.setFont(juce::Font(juce::FontOptions(10.f)));
        g.drawText("Enable auto-save or press Record", PAD, y + 42, cw, 16,
                    juce::Justification::centred);
        m_takeListTotalH = 0;
        return;
    }

    // Section header
    int wavCount = 0;
    for (auto& t : m_takes) if (t.folder != "__DIR__" && !t.live) ++wavCount;
    g.setColour(HINT);
    g.setFont(juce::Font(juce::FontOptions(9.f)));
    juce::String hdrText = m_currentFolder.isEmpty()
        ? juce::String(wavCount) + " TAKES"
        : m_currentFolder.toUpperCase() + "  -  " + juce::String(wavCount) + " TAKES";
    g.drawText(hdrText, PAD, y, cw - 80, 14, juce::Justification::centredLeft);
    y += 16;

    // Clip region for scrollable list
    const int listTop    = y;
    const int listBottom = H - STA - 4;
    const int listH      = listBottom - listTop;
    g.saveState();
    g.reduceClipRegion(PAD, listTop, cw, listH);

    const int rowH    = 36;
    const int dirRowH = 30;
    int totalH = 0;
    for (auto& t : m_takes)
        totalH += (t.folder == "__DIR__") ? dirRowH : rowH;
    m_takeListTotalH = totalH;

    // Clamp scroll
    int maxScroll = std::max(0, totalH - listH);
    m_takesScrollY = std::min(m_takesScrollY, maxScroll);
    m_takesScrollY = std::max(0, m_takesScrollY);

    int iy = y - m_takesScrollY;

    for (int i = 0; i < (int)m_takes.size(); ++i)
    {
        auto& t = m_takes.getReference(i);
        bool isDir = (t.folder == "__DIR__");
        const int rh = isDir ? dirRowH : rowH;

        // Skip rows fully above clip
        if (iy + rh < listTop)  { iy += rh; continue; }
        // Stop rows fully below clip
        if (iy > listBottom)     break;

        if (isDir)
        {
            // ── Folder row ──────────────────────────────────────────────────
            bool isDragTarget = (m_isDraggingTake && i == m_dragHoverFolder);
            auto fb = juce::Rectangle<float>((float)PAD, (float)(iy+2), (float)cw, (float)(rh-4));
            // Gradient card background
            if (isDragTarget) {
                g.setColour(ACCENT.withAlpha(0.18f));
                g.fillRoundedRectangle(fb, 5.f);
            } else {
                juce::ColourGradient fg(juce::Colour(0xff1c2030), fb.getX(), fb.getY(),
                                         juce::Colour(0xff161a28), fb.getX(), fb.getBottom(), false);
                g.setGradientFill(fg);
                g.fillRoundedRectangle(fb, 5.f);
            }
            g.setColour(isDragTarget ? ACCENT.withAlpha(0.6f) : BORDER2);
            g.drawRoundedRectangle(fb.reduced(0.5f), 5.f, isDragTarget ? 1.5f : 0.5f);
            // Left accent stripe
            g.setColour(ACCENT.withAlpha(0.4f));
            g.fillRoundedRectangle((float)PAD, (float)(iy+2), 2.5f, (float)(rh-4), 1.f);
            // Folder icon — drawn as simple shape (no emoji, avoids font issues)
            {
                float fx = (float)(PAD + 6), fy = (float)(iy + 8);
                g.setColour(ACCENT.withAlpha(0.6f));
                // Folder body
                g.fillRoundedRectangle(fx, fy + 3.f, 18.f, 12.f, 2.f);
                // Folder tab
                g.fillRoundedRectangle(fx, fy, 9.f, 5.f, 1.5f);
            }
            g.setFont(juce::Font(juce::FontOptions(11.f)));
            g.setColour(TEXT);
            g.drawText(t.name, PAD+30, iy+4, cw-110, 14,
                        juce::Justification::centredLeft, true);
            g.setFont(juce::Font(juce::FontOptions(9.f)));
            g.setColour(HINT);
            // Keep duration text clear of the chevron (chevron at W-PAD-16)
            g.drawText(t.duration, W-PAD-90, iy+8, 68, 12,
                        juce::Justification::centredRight);
            // Right chevron drawn as path
            {
                float ax = (float)(W - PAD - 16), ay = (float)(iy + 11);
                juce::Path chevron;
                chevron.startNewSubPath(ax, ay);
                chevron.lineTo(ax + 5.f, ay + 4.f);
                chevron.lineTo(ax, ay + 8.f);
                g.setColour(ACCENT_LT);
                g.strokePath(chevron, juce::PathStrokeType(1.5f,
                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
        else
        {
            // ── Take row ────────────────────────────────────────────────────
            const int ry = iy;
            {
                // Alternating row bg
                g.setColour(i % 2 == 0 ? juce::Colour(0x0cffffff) : juce::Colour(0x05ffffff));
                g.fillRect(PAD, ry, cw, rowH - 1);
                // Left accent bar (blue for non-live, red for live)
                g.setColour(t.live ? RED.withAlpha(0.5f) : ACCENT.withAlpha(0.25f));
                g.fillRect(PAD, ry, 2, rowH - 1);
            }
            // WAV badge
            g.setColour(ACCENT.withAlpha(t.live ? 1.f : 0.5f));
            g.fillRoundedRectangle((float)(PAD+6), (float)(ry+8), 20.f, 20.f, 3.f);
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(juce::FontOptions(7.f, juce::Font::bold)));
            g.drawText("WAV", PAD+6, ry+11, 20, 10, juce::Justification::centred);
            // Name
            g.setFont(juce::Font(juce::FontOptions(11.f)));
            g.setColour(t.live ? RED_LT : TEXT);
            g.drawText(t.name, PAD+32, ry+4, cw-140, 14,
                        juce::Justification::centredLeft, true);
            // Waveform thumbnail (if peaks available) or transcript preview
            if (!t.peaks.empty() && !t.live) {
                const int wfX = PAD + 32, wfW = cw - 140 - 4, wfY = ry + 19, wfH = 10;
                const int nB = (int)t.peaks.size();
                float bw = (float)wfW / (float)nB;
                for (int b = 0; b < nB; ++b) {
                    float ph = std::max(2.f, t.peaks[b] * (float)wfH);
                    g.setColour(ACCENT.withAlpha(0.5f));
                    g.fillRect((float)(wfX + b*bw), (float)(wfY + wfH - ph), bw - 0.5f, ph);
                }
            } else if (t.transcript.isNotEmpty()) {
                g.setFont(juce::Font(juce::FontOptions(9.f)));
                g.setColour(HINT);
                g.drawText(t.transcript, PAD+32, ry+19, cw-140, 12,
                            juce::Justification::centredLeft, true);
            }
            // BPM + Key badges
            if (!t.live && (t.bpm > 0.f || t.key.isNotEmpty())) {
                g.setFont(juce::Font(juce::FontOptions(8.f)));
                juce::String badge;
                if (t.bpm > 0.f) badge += juce::String((int)t.bpm) + " bpm";
                if (t.key.isNotEmpty()) badge += (badge.isEmpty() ? "" : "  ") + t.key;
                g.setColour(ACCENT.withAlpha(0.6f));
                g.drawText(badge, W-PAD-100, ry+19, 94, 11, juce::Justification::centredRight);
            }
            // Right badges
            if (t.live) {
                g.setColour(RED.withAlpha(0.12f));
                g.fillRoundedRectangle((float)(W-PAD-34), (float)(ry+10), 28.f, 14.f, 3.f);
                g.setColour(RED_LT);
                g.setFont(juce::Font(juce::FontOptions(8.f, juce::Font::bold)));
                g.drawText("live", W-PAD-34, ry+11, 28, 12, juce::Justification::centred);
            } else {
                g.setFont(juce::Font(juce::FontOptions(9.f)));
                g.setColour(HINT);
                g.drawText(t.duration + "  " + t.size,
                            W-PAD-100, ry+6, 96, 12, juce::Justification::centredRight);
                if (m_proc.hasModel()) {
                    if (t.transcribing) {
                        // Progress bar while transcribing
                        float prog = m_proc.getTranscribeProgress();
                        const int bx = W-PAD-100, bw = 96, by = ry+21, bh = 7;
                        g.setColour(BORDER2);
                        g.fillRoundedRectangle((float)bx, (float)by, (float)bw, (float)bh, 3.f);
                        g.setColour(ACCENT);
                        float fillW = std::max(6.f, prog * (float)bw);
                        g.fillRoundedRectangle((float)bx, (float)by, fillW, (float)bh, 3.f);
                    } else {
                        juce::String txLabel = t.transcript.isEmpty() ? "Transcribe" : "✓";
                        g.setColour(t.transcript.isEmpty() ? ACCENT_LT : GREEN);
                        g.drawText(txLabel, W-PAD-100, ry+20, 96, 11,
                                    juce::Justification::centredRight);
                    }
                }
            }
        }
        iy += rh;
    }

    g.restoreState();

    // ── Drag ghost — show take name following cursor ──────────────────────────
    if (m_isDraggingTake && m_dragIndex >= 0 && m_dragIndex < (int)m_takes.size())
    {
        auto& dt = m_takes.getReference(m_dragIndex);
        auto mp  = getMouseXYRelative();
        auto ghost = juce::Rectangle<float>((float)(mp.x - 60), (float)(mp.y - 10),
                                             120.f, 20.f);
        g.setColour(CARD.withAlpha(0.88f));
        g.fillRoundedRectangle(ghost, 4.f);
        g.setColour(ACCENT.withAlpha(0.5f));
        g.drawRoundedRectangle(ghost.reduced(0.5f), 4.f, 1.f);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.setColour(TEXT);
        juce::String ghostLabel = dt.name.upToLastOccurrenceOf(".wav", false, true);
        if (ghostLabel.isEmpty()) ghostLabel = dt.name;
        g.drawText(ghostLabel, (int)ghost.getX() + 4, (int)ghost.getY() + 3,
                   (int)ghost.getWidth() - 8, 14, juce::Justification::centredLeft, true);
    }

    // Scrollbar
    if (totalH > listH) {
        float sbFrac  = (float)listH / (float)totalH;
        float sbPos   = (float)m_takesScrollY / (float)totalH;
        int   sbH     = (int)(sbFrac * listH);
        int   sbY     = listTop + (int)(sbPos * listH);
        g.setColour(BORDER2.withAlpha(0.7f));
        g.fillRoundedRectangle((float)(W - 5), (float)sbY, 3.f, (float)sbH, 1.5f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paintSettings(juce::Graphics& g)
{
    using namespace MicInput::Colours;
    const int cw = W - PAD * 2;
    int y = BODY_Y + PAD;

    auto sectionLabel = [&](const char* lbl) {
        g.setColour(HINT);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.drawText(lbl, PAD, y, cw, 12, juce::Justification::centredLeft);
        y += 14;
    };

    // ── Input mode ────────────────────────────────────────────────────────────
    sectionLabel("INPUT MODE");
    bool excl = m_proc.isExclusive.load();
    float modeW = (float)cw / 2.f;
    struct { juce::String label; bool active; juce::String sub; } modes[] = {
        { "Shared", !excl, juce::String(m_proc.captureMs.load(), 0) + " ms" },
        { "Exclusive", excl, "faster (if supported)" },
    };
    for (int i = 0; i < 2; ++i) {
        auto mb = juce::Rectangle<float>((float)PAD + modeW*i, (float)y, modeW-4.f, 28.f);
        if (modes[i].active) {
            g.setColour(ACCENT.withAlpha(0.15f));
            g.fillRoundedRectangle(mb, 5.f);
            g.setColour(ACCENT.withAlpha(0.4f));
            g.drawRoundedRectangle(mb.reduced(0.5f), 5.f, 1.f);
            g.setColour(ACCENT_LT);
        } else {
            g.setColour(CARD);
            g.fillRoundedRectangle(mb, 5.f);
            g.setColour(BORDER2);
            g.drawRoundedRectangle(mb.reduced(0.5f), 5.f, 1.f);
            g.setColour(HINT);
        }
        g.setFont(juce::Font(juce::FontOptions(11.f)));
        g.drawText(modes[i].label + "  " + modes[i].sub,
                    (int)mb.getX()+4, y+3, (int)mb.getWidth()-8, 22,
                    juce::Justification::centred);
    }
    y += 34;

    // ── Buffer latency ────────────────────────────────────────────────────────
    sectionLabel("BUFFER LATENCY");
    // Slider + value drawn by JUCE at y
    y += 28;
    // Preset chips
    struct { float ms; const char* lbl; } presets[] = {
        {1.f,"1 ms  rap"}, {5.f,"5 ms  singing"}, {10.f,"10 ms  podcast"}, {25.f,"25 ms  stable"}
    };
    float chipW2 = (float)(cw) / 4.f;
    for (int i = 0; i < 4; ++i) {
        bool active = std::abs(m_proc.getPrebufMs() - presets[i].ms) < 0.6f;
        auto pb = juce::Rectangle<float>((float)PAD + chipW2*i, (float)y, chipW2-4.f, 22.f);
        g.setColour(active ? ACCENT.withAlpha(0.15f) : CARD.withAlpha(0.5f));
        g.fillRoundedRectangle(pb, 4.f);
        g.setColour(active ? ACCENT.withAlpha(0.4f) : BORDER);
        g.drawRoundedRectangle(pb.reduced(0.5f), 4.f, 1.f);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.setColour(active ? ACCENT_LT : HINT);
        g.drawText(presets[i].lbl, (int)pb.getX()+2, y+4, (int)pb.getWidth()-4, 14,
                    juce::Justification::centred);
    }
    y += 28;

    // ── Record offset ─────────────────────────────────────────────────────────
    sectionLabel("RECORD OFFSET");
    {
        juce::Rectangle<float> pill((float)PAD, (float)y, (float)cw, 34.f);
        bool auto_ = m_proc.hostAppliesPDC();
        g.setColour(auto_ ? GREEN.withAlpha(0.08f) : AMBER.withAlpha(0.08f));
        g.fillRoundedRectangle(pill, 6.f);
        g.setColour(auto_ ? GREEN.withAlpha(0.3f) : AMBER.withAlpha(0.3f));
        g.drawRoundedRectangle(pill.reduced(0.5f), 6.f, 1.f);
        g.setFont(juce::Font(juce::FontOptions(11.f, juce::Font::bold)));
        g.setColour(auto_ ? GREEN_LT : AMBER_LT);
        juce::String host = m_proc.getHostName();
        g.drawText((auto_ ? "Auto  -  " : host + "  -  set ") + "-"
                    + juce::String(m_proc.getRecordOffsetSamples()) + " samples",
                    PAD+8, y+5, cw-16, 14, juce::Justification::centredLeft);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.setColour(HINT);
        juce::String sub = auto_ ? "Clips land on beat automatically"
                                 : "Track inspector (I) - Record Offset";
        g.drawText(sub, PAD+8, y+19, cw-16, 12, juce::Justification::centredLeft);
    }
    y += 42;

    // ── Whisper local model ───────────────────────────────────────────────────
    sectionLabel("WHISPER TRANSCRIPTION  (local, no internet)");

    // Model pills drawn by JUCE above — draw labels/status below them
    y += 26;   // height of pill row

    // Per-model: check if downloaded, show size hint
    using MS = WhisperClient::ModelSize;
    MS cur = m_proc.getWhisperModel();
    struct { MS m; const char* hint; } metas[] = {
        { MS::Tiny,   "75 MB" },
        { MS::Base,   "142 MB" },
        { MS::Small,  "466 MB" },
        { MS::Medium, "1.5 GB" },
        { MS::Large,  "2.9 GB" },
    };
    const int pilW2 = (cw - 4) / 5;
    for (int i = 0; i < 5; ++i) {
        bool exists = WhisperClient::modelExists(metas[i].m);
        bool active = (metas[i].m == cur);
        g.setFont(juce::Font(juce::FontOptions(8.f)));
        g.setColour(exists ? (active ? ACCENT_LT : GREEN) : HINT);
        g.drawText(exists ? (active ? "loaded" : "ready") : metas[i].hint,
                    PAD + pilW2*i, y, pilW2-2, 12, juce::Justification::centred);
    }
    y += 14;

    // Status line
    bool hasModel = m_proc.hasModel();
    bool isLoaded = m_proc.isModelLoaded();
    g.setColour(hasModel ? (isLoaded ? GREEN : ACCENT) : AMBER);
    g.fillEllipse((float)PAD, (float)(y+2), 6.f, 6.f);
    g.setFont(juce::Font(juce::FontOptions(10.f)));
    g.setColour(DIM);
    juce::String statusMsg;
    bool downloadFinished = (m_downloadProgress.load() >= 1.f && !m_downloader.isRunning());
    if (downloadFinished) {
        statusMsg = WhisperClient::modelName(cur) + " model — latest version installed";
    } else if (!hasModel) {
        statusMsg = "Not downloaded — click Download";
    } else if (isLoaded) {
        statusMsg = WhisperClient::modelLabel(cur) + " — loaded in RAM";
    } else {
        statusMsg = WhisperClient::modelLabel(cur) + " — click Transcribe to load";
    }
    g.drawText(statusMsg, PAD+10, y, cw-80, 14, juce::Justification::centredLeft);
    // y is ~316 here. Download button is at fixed y=336 (placed by resized).

    // ── Download progress / error bar (between status and download btn) ───────
    // These are painted-only in the gap between y=336 (download btn) and y=364.
    // We use fixed coordinates matching resized() constants.
    const int downloadY = 336;
    const int progY     = downloadY + 28;   // just below download button

    bool downloading = m_downloader.isRunning();
    float prog = m_downloadProgress.load();

    if (downloading || (prog > 0.f && prog < 1.f))
    {
        auto track = juce::Rectangle<float>((float)PAD, (float)progY, (float)cw, 8.f);
        g.setColour(CARD);
        g.fillRoundedRectangle(track, 4.f);
        g.setColour(BORDER2);
        g.drawRoundedRectangle(track.reduced(0.5f), 4.f, 1.f);
        if (prog > 0.f) {
            g.setColour(ACCENT);
            g.fillRoundedRectangle(track.withWidth(track.getWidth() * prog), 4.f);
        }
        int64_t done  = m_downloadedBytes.load();
        int64_t total = m_totalBytes.load();
        auto fmtMB = [](int64_t b) -> juce::String {
            return b < 1024*1024
                ? juce::String(b / 1024) + " KB"
                : juce::String(b / (1024*1024.f), 1) + " MB";
        };
        juce::String progLabel = prog >= 1.f ? "Download complete"
                               : fmtMB(done) + (total > 0 ? " / " + fmtMB(total) : "");
        g.setColour(HINT);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.drawText(progLabel, PAD, progY + 10, cw, 12, juce::Justification::centredLeft);
    }
    else if (m_downloadError)
    {
        auto errBox = juce::Rectangle<float>((float)PAD, (float)progY, (float)cw, 18.f);
        g.setColour(RED.withAlpha(0.08f));
        g.fillRoundedRectangle(errBox, 4.f);
        g.setColour(RED_LT);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.drawText("Error: " + m_downloadErrorMsg, PAD+4, progY+3, cw-8, 12,
                    juce::Justification::centredLeft, true);
    }

    // ── Row labels (12px above each widget row) ──────────────────────────────
    g.setColour(HINT);
    g.setFont(juce::Font(juce::FontOptions(9.f)));
    const int halfW2 = (cw - 8) / 2;
    // Row 1 label strip (widgets at 382, label at 370)
    g.drawText("LANGUAGE",    PAD,              370, halfW2, 11, juce::Justification::centredLeft);
    g.drawText("NOISE GATE",  PAD + halfW2 + 4, 370, halfW2, 11, juce::Justification::centredLeft);
    // Row 2 label strip (widgets at 424, label at 412)
    g.drawText("INPUT GAIN",  PAD,              412, halfW2, 11, juce::Justification::centredLeft);
    g.drawText("MIDI TRIGGER",PAD + halfW2 + 4, 412, halfW2, 11, juce::Justification::centredLeft);

    // ── Diagnostics ───────────────────────────────────────────────────────────
    g.setColour(BORDER2);
    g.drawLine((float)PAD, 456.f, (float)(W-PAD), 456.f, 1.f);
    g.setColour(HINT.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions(9.f)));
    g.drawText("Ring fill: " + juce::String(m_proc.ringFillMs.load(), 1) + " ms    "
               "Underruns: " + juce::String((int64_t)m_proc.underruns.load()),
               PAD, 462, cw, 12, juce::Justification::centredLeft);
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::paintLyrics(juce::Graphics& g)
{
    using namespace MicInput::Colours;
    const int cw = W - PAD * 2;

    // ── Fake header bar (replaces normal header chrome) ───────────────────────
    g.setColour(CHROME);
    g.fillRect(0, BODY_Y, W, NAV);
    g.setColour(BORDER);
    g.drawLine(0.f, (float)(BODY_Y + NAV), (float)W, (float)(BODY_Y + NAV), 1.f);

    // Back arrow icon inside m_lyricsBackBtn area
    {
        auto bb = m_lyricsBackBtn.getBounds();
        g.setColour(CARD);
        g.fillRoundedRectangle(bb.toFloat(), 5.f);
        g.setColour(BORDER2);
        g.drawRoundedRectangle(bb.toFloat().reduced(0.5f), 5.f, 1.f);
        // Arrow
        float bx = (float)bb.getCentreX(), by = (float)bb.getCentreY();
        juce::Path arrow;
        arrow.startNewSubPath(bx + 4.f, by - 5.f);
        arrow.lineTo(bx - 2.f, by);
        arrow.lineTo(bx + 4.f, by + 5.f);
        g.setColour(DIM);
        g.strokePath(arrow, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    // Title: take filename (room for back btn + copy btn)
    juce::String title = (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_takes.size())
        ? m_takes[m_lyricsIndex].name : "";
    g.setColour(TEXT);
    g.setFont(juce::Font(juce::FontOptions(12.f, juce::Font::bold)));
    g.drawText(title, PAD + 32, BODY_Y, W - PAD*2 - 32 - 70, NAV,
                juce::Justification::centredLeft, true);

    // ── Body background ───────────────────────────────────────────────────────
    g.setColour(BG);
    g.fillRect(0, BODY_Y + NAV, W, H - BODY_Y - NAV - STA);

    int y = BODY_Y + NAV + PAD;

    // ── Audio strip ───────────────────────────────────────────────────────────
    {
        auto strip = juce::Rectangle<float>((float)PAD, (float)y, (float)cw, 34.f);
        g.setColour(CARD);
        g.fillRoundedRectangle(strip, 7.f);
        g.setColour(BORDER);
        g.drawRoundedRectangle(strip.reduced(0.5f), 7.f, 1.f);

        // Play/pause icon inside m_lyricsPlayBtn
        auto pb = m_lyricsPlayBtn.getBounds();
        g.setColour(ACCENT.withAlpha(0.2f));
        g.fillEllipse(pb.toFloat().reduced(1.f));
        g.setColour(ACCENT.withAlpha(0.5f));
        g.drawEllipse(pb.toFloat().reduced(1.f), 1.f);
        if (m_player.isPlaying()) {
            // Pause bars
            float bx = (float)pb.getCentreX(), by2 = (float)pb.getCentreY();
            g.setColour(ACCENT_LT);
            g.fillRect((int)(bx - 5), (int)(by2 - 5), 4, 10);
            g.fillRect((int)(bx + 1), (int)(by2 - 5), 4, 10);
        } else {
            // Play triangle
            juce::Path tri;
            float pbcx = pb.getCentreX() + 1.f, pbcy = (float)pb.getCentreY();
            tri.addTriangle(pbcx - 4.f, pbcy - 6.f, pbcx - 4.f, pbcy + 6.f, pbcx + 6.f, pbcy);
            g.setColour(ACCENT_LT);
            g.fillPath(tri);
        }

        // Waveform bars
        const int wfX  = PAD + 36;
        const int wfW  = cw - 36 - 50;
        const int wfY  = y + 5;
        const int wfH  = 24;
        const auto& peaks = m_player.peaks();
        const float progress = m_player.getProgress();

        if (!peaks.empty()) {
            const int nBars = (int)peaks.size();
            const float barW = (float)wfW / (float)nBars;
            const int playedBars = (int)(progress * nBars);
            for (int b = 0; b < nBars; ++b) {
                float ph = std::max(3.f, peaks[b] * (float)(wfH - 4));
                float bx2 = wfX + b * barW + 1.f;
                float ht  = wfY + (wfH - ph) * 0.5f;
                g.setColour(b < playedBars ? ACCENT.withAlpha(0.7f)
                                           : juce::Colour(0x22ffffff));
                g.fillRoundedRectangle(bx2, ht, std::max(1.5f, barW - 1.f), ph, 1.f);
            }
            // Progress line
            float lx = wfX + progress * wfW;
            g.setColour(ACCENT);
            g.fillRect((int)lx, wfY, 2, wfH);
        } else {
            // No peaks yet — draw placeholder track
            g.setColour(BORDER2);
            g.fillRoundedRectangle((float)wfX, (float)(y + 13), (float)wfW, 8.f, 2.f);
        }

        // Time display
        double posSecs = m_player.getPositionSecs();
        double durSecs = m_player.getDurationSecs();
        auto fmt = [](double s) -> juce::String {
            int m = (int)(s / 60.0), sec = (int)s % 60;
            return juce::String::formatted("%d:%02d", m, sec);
        };
        g.setColour(HINT);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.drawText(fmt(posSecs) + " / " + fmt(durSecs),
                    wfX + wfW + 4, y + 11, 44, 12, juce::Justification::centredLeft);
    }
    y += 38;

    // ── Transcribe bar (when no transcript) ───────────────────────────────────
    bool hasTx = (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_takes.size()
                  && m_takes[m_lyricsIndex].transcript.isNotEmpty());
    bool isTxing = (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_takes.size()
                    && m_takes[m_lyricsIndex].transcribing);

    if (!hasTx && m_proc.hasModel()) {
        auto bar = juce::Rectangle<float>((float)PAD, (float)y, (float)cw, 32.f);
        g.setColour(ACCENT.withAlpha(0.05f));
        g.fillRoundedRectangle(bar, 6.f);
        g.setColour(ACCENT.withAlpha(0.15f));
        g.drawRoundedRectangle(bar.reduced(0.5f), 6.f, 1.f);

        if (isTxing) {
            // Progress bar fills the transcribe area
            float prog = m_proc.getTranscribeProgress();
            g.setColour(ACCENT.withAlpha(0.18f));
            g.fillRoundedRectangle((float)PAD, (float)y,
                                    std::max(8.f, prog * (float)cw), 32.f, 6.f);
            g.setColour(ACCENT_LT);
            g.setFont(juce::Font(juce::FontOptions(10.f)));
            int pct = (int)(prog * 100.f);
            g.drawText("Transcribing...  " + juce::String(pct) + "%",
                        PAD + 8, y + 9, cw - 20, 14, juce::Justification::centredLeft);
        } else {
            g.setColour(HINT);
            g.setFont(juce::Font(juce::FontOptions(10.f)));
            g.drawText("No transcript — click Transcribe to run local Whisper, or type manually",
                        PAD + 8, y + 9, cw - 100, 14, juce::Justification::centredLeft);
        }
        y += 36;
    }

    // ── Lyrics label ──────────────────────────────────────────────────────────
    g.setColour(HINT);
    g.setFont(juce::Font(juce::FontOptions(9.f)));
    g.drawText("LYRICS", PAD, y, 50, 12, juce::Justification::centredLeft);
    if (hasTx) {
        // "Whisper" badge
        auto badge = juce::Rectangle<float>((float)(PAD + 54), (float)(y), 44.f, 12.f);
        g.setColour(ACCENT.withAlpha(0.12f));
        g.fillRoundedRectangle(badge, 3.f);
        g.setColour(ACCENT_LT);
        g.setFont(juce::Font(juce::FontOptions(8.f)));
        g.drawText("Whisper", (int)badge.getX(), (int)badge.getY(),
                    (int)badge.getWidth(), (int)badge.getHeight(),
                    juce::Justification::centred);
    }
    if (m_lyricsDirty) {
        g.setColour(AMBER);
        g.setFont(juce::Font(juce::FontOptions(9.f)));
        g.drawText("unsaved", W - PAD - 48, y, 46, 12, juce::Justification::centredRight);
    }
    y += 14;
    // ── Word alignment view (when word timestamps available) ─────────────────
    if (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_takes.size()) {
        const auto& curTake = m_takes.getReference(m_lyricsIndex);
        if (!curTake.words.empty() && m_player.isLoaded()) {
            const float dur = (float)m_player.getDurationSecs();
            const float pos = (float)m_player.getPositionSecs();
            // Draw compact word chips over the waveform area
            // They sit at the very bottom of the waveform strip, 12px tall
            const int wfX = PAD + 36, wfW = cw - 36 - 50;
            const int wfY = BODY_Y + NAV + PAD + 5 + 12;  // just below waveform centre
            g.setFont(juce::Font(juce::FontOptions(8.f)));
            for (const auto& w : curTake.words) {
                if (dur <= 0.f || w.t1 <= 0.f) continue;
                int wx = wfX + (int)(w.t0 / dur * (float)wfW);
                bool active = (pos >= w.t0 && pos < w.t1);
                g.setColour(active ? ACCENT : DIM.withAlpha(0.5f));
                juce::String label = juce::String(w.text);
                g.drawText(label, wx, wfY, 40, 10, juce::Justification::centredLeft, false);
            }
        }
    }

    // (m_lyricsEditor JUCE component fills from y to toolbar)

    // ── Toolbar ───────────────────────────────────────────────────────────────
    const int toolbarY = H - STA - 34;
    g.setColour(CHROME);
    g.fillRect(0, toolbarY, W, 34);
    g.setColour(BORDER);
    g.drawLine(0.f, (float)toolbarY, (float)W, (float)toolbarY, 1.f);

    // Char count
    int chars = m_lyricsEditor.getText().length();
    g.setColour(HINT);
    g.setFont(juce::Font(juce::FontOptions(10.f)));
    g.drawText(juce::String(chars) + " chars", PAD, toolbarY + 9, 80, 14,
                juce::Justification::centredLeft);

    // Delete button — drawn manually (danger style)
    auto delBounds = m_lyricsDeleteBtn.getBounds().toFloat();
    g.setColour(RED.withAlpha(0.08f));
    g.fillRoundedRectangle(delBounds, 5.f);
    g.setColour(RED.withAlpha(0.3f));
    g.drawRoundedRectangle(delBounds.reduced(0.5f), 5.f, 1.f);
    g.setColour(RED_LT);
    g.setFont(juce::Font(juce::FontOptions(10.f)));
    g.drawText("Delete", m_lyricsDeleteBtn.getBounds(), juce::Justification::centred);

    // Save button — drawn by LookAndFeel (accent style)
    // already placed via setBounds; LookAndFeel draws it
}

// ─────────────────────────────────────────────────────────────────────────────
// Scan save folder for .wav files and populate m_takes from disk.
// Duration is read from the WAV header (data chunk size / (sr * channels * bps)).
// Transcripts are stored as .txt sidecars: take_name.wav -> take_name.txt
// In-progress live entries are preserved.
// ─────────────────────────────────────────────────────────────────────────────
// Background analysis after recording: build waveform peaks + detect pitch key
void MicInputEditor::runPostRecordAnalysis(int takeIndex)
{
    if (takeIndex < 0 || takeIndex >= (int)m_takes.size()) return;
    juce::String takeName = m_takes.getReference(takeIndex).name;
    juce::File activeDir  = m_currentFolder.isEmpty()
        ? juce::File(m_savePath)
        : juce::File(m_savePath).getChildFile(m_currentFolder);
    juce::String wavPath  = activeDir.getChildFile(takeName).getFullPathName();

    juce::Component::SafePointer<MicInputEditor> safeThis(this);

    std::thread([safeThis, wavPath, takeIndex] {
        // Load WAV into memory
        juce::WavAudioFormat fmt;
        auto streamOwner = juce::File(wavPath).createInputStream();
        if (!streamOwner) return;
        auto* rawStream = streamOwner.release();
        std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(rawStream, true));
        if (!reader) return;

        const int   nch     = (int)std::min(reader->numChannels, 2u);
        const int64_t total = std::min(reader->lengthInSamples, (int64_t)(30.0 * 60.0 * reader->sampleRate));
        if (total <= 0) return;

        juce::AudioBuffer<float> buf(nch, (int)total);
        reader->read(&buf, 0, (int)total, 0, true, nch > 1);

        // Mix to mono for pitch detection
        const float* L = buf.getReadPointer(0);
        const float* R = nch > 1 ? buf.getReadPointer(1) : L;
        std::vector<float> mono((size_t)total);
        for (int64_t i = 0; i < total; ++i)
            mono[(size_t)i] = (L[i] + R[i]) * 0.5f;

        // Detect pitch key
        std::string key = PitchDetector::analyseFile(mono, reader->sampleRate);

        // Build 48-bar peak thumbnail
        const int nBars = 48;
        std::vector<float> peaks(nBars, 0.f);
        int64_t barLen = std::max((int64_t)1, total / nBars);
        for (int b = 0; b < nBars; ++b) {
            int64_t start = (int64_t)b * barLen;
            int64_t end   = std::min(start + barLen, total);
            float   pk    = 0.f;
            for (int64_t s = start; s < end; ++s)
                pk = std::max(pk, std::abs(mono[(size_t)s]));
            peaks[b] = pk;
        }
        float mx = *std::max_element(peaks.begin(), peaks.end());
        if (mx > 0.f) for (auto& p : peaks) p /= mx;

        // Deliver results to message thread
        juce::MessageManager::callAsync([safeThis, takeIndex, key, peaks] {
            if (safeThis == nullptr) return;
            if (takeIndex < (int)safeThis->m_takes.size()) {
                auto& t = safeThis->m_takes.getReference(takeIndex);
                t.key   = juce::String(key);
                t.peaks = peaks;
                // Update sidecar with detected key so it persists across reloads
                if (!key.empty()) {
                    juce::File activeDir = safeThis->m_currentFolder.isEmpty()
                        ? juce::File(safeThis->m_savePath)
                        : juce::File(safeThis->m_savePath).getChildFile(safeThis->m_currentFolder);
                    juce::File sidecar = activeDir.getChildFile(t.name).withFileExtension("txt");
                    juce::String existing = sidecar.existsAsFile() ? sidecar.loadFileAsString() : "";
                    // Only prepend key if not already present
                    if (!existing.startsWithIgnoreCase("key:") && !existing.contains("\nkey:")) {
                        juce::String header;
                        if (t.bpm > 0.f) header += "bpm: " + juce::String((int)t.bpm) + "\n";
                        header += "key: " + juce::String(key) + "\n";
                        // Strip any existing bpm header to avoid duplicates
                        juce::String body = existing;
                        if (existing.startsWithIgnoreCase("bpm:"))
                            body = existing.fromFirstOccurrenceOf("\n", false, false).trimStart();
                        sidecar.replaceWithText(header + body);
                    }
                }
            }
            safeThis->repaint();
        });
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::scanTakesFromDisk()
{
    m_takesScrollY = 0;  // reset scroll on every scan
    // Scan the active folder (root or a subfolder)
    juce::File rootFolder(m_savePath);
    juce::File folder = m_currentFolder.isEmpty()
        ? rootFolder
        : rootFolder.getChildFile(m_currentFolder);
    if (!folder.isDirectory()) { m_takes.clear(); return; }

    // Collect existing live entries so we can merge them back
    juce::Array<Take> liveTakes;
    for (auto& t : m_takes)
        if (t.live) liveTakes.add(t);

    m_takes.clear();

    // Add subdirectories as special "folder" entries (only in root view)
    if (m_currentFolder.isEmpty()) {
        auto dirs = rootFolder.findChildFiles(juce::File::findDirectories, false);
        for (auto& d : dirs) {
            if (d.getFileName().startsWithChar('.')) continue;  // skip hidden
            Take ft;
            ft.name   = d.getFileName();
            ft.folder = "__DIR__";  // sentinel = directory entry
            int wavCount = d.findChildFiles(juce::File::findFiles, false, "*.wav").size();
            ft.duration = juce::String(wavCount) + " take" + (wavCount == 1 ? "" : "s");
            ft.size   = {};
            ft.live   = false;
            m_takes.add(ft);
        }
    }

    // Find all .wav files, sort newest first
    auto wavFiles = folder.findChildFiles(juce::File::findFiles, false, "*.wav");
    struct Sortable { juce::File f; juce::Time t; };
    juce::Array<Sortable> sorted;
    for (auto& f : wavFiles)
        sorted.add({f, f.getLastModificationTime()});
    struct SortByTime {
        static int compareElements(const Sortable& a, const Sortable& b)
        { return a.t > b.t ? -1 : (a.t < b.t ? 1 : 0); }
    };
    SortByTime sorter;
    sorted.sort(sorter, false);

    for (auto& s : sorted)
    {
        Take t;
        t.name = s.f.getFileName();
        t.live = false;

        // File size
        int64_t bytes = s.f.getSize();
        t.size = juce::String(bytes / (1024.f * 1024.f), 1) + " MB";

        // Duration: read from WAV header
        {
            juce::WavAudioFormat fmt;
            if (auto stream = s.f.createInputStream())
            {
                std::unique_ptr<juce::AudioFormatReader> reader(
                    fmt.createReaderFor(stream.release(), true));
                if (reader && reader->sampleRate > 0 && reader->lengthInSamples > 0)
                {
                    double secs = (double)reader->lengthInSamples / reader->sampleRate;
                    int mins = (int)(secs / 60.0), sec = (int)secs % 60;
                    t.duration = juce::String::formatted("%d:%02d", mins, sec);
                }
                else
                {
                    t.duration = "?:??";
                }
            }
        }

        // Transcript sidecar: same name with .txt extension
        juce::File sidecar = s.f.withFileExtension("txt");
        if (sidecar.existsAsFile()) {
            juce::String full = sidecar.loadFileAsString();
            // Parse sidecar format: bpm:, key: headers then blank line then lyrics
            juce::String body = full;
            for (auto& line : juce::StringArray::fromLines(full)) {
                if (line.startsWithIgnoreCase("bpm:")) {
                    t.bpm = line.fromFirstOccurrenceOf(":", false, false).trim().getFloatValue();
                    body = body.fromFirstOccurrenceOf(line, false, false);
                } else if (line.startsWithIgnoreCase("key:")) {
                    t.key = line.fromFirstOccurrenceOf(":", false, false).trim();
                    body = body.fromFirstOccurrenceOf(line, false, false);
                }
            }
            t.transcript = body.trim();
        }

        m_takes.add(t);
    }

    // Re-insert live entries at the front
    for (int i = liveTakes.size() - 1; i >= 0; --i)
        m_takes.insert(0, liveTakes.getReference(i));

    // Trigger post-record analysis for any takes that lack peaks (lazy load)
    // Limit to first 5 unanalysed takes to avoid hammering CPU on large folders
    int analysisCount = 0;
    for (int ti = 0; ti < (int)m_takes.size() && analysisCount < 5; ++ti) {
        auto& tk = m_takes.getReference(ti);
        if (!tk.live && tk.peaks.empty() && tk.folder != "__DIR__") {
            runPostRecordAnalysis(ti);
            ++analysisCount;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::renameFolderDialog(const juce::String& folderName)
{
    auto* alert = new juce::AlertWindow("Rename folder",
        "Enter a new name for "" + folderName + "":",
        juce::MessageBoxIconType::NoIcon);
    alert->addTextEditor("name", folderName, "New name:");
    alert->addButton("Rename", 1);
    alert->addButton("Cancel", 0);
    juce::Component::SafePointer<MicInputEditor> safeThis2(this);
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safeThis2, folderName, alert](int result) {
            if (result == 1 && safeThis2 != nullptr) {
                juce::String newName = alert->getTextEditorContents("name").trim();
                newName = newName.replaceCharacters("/:*?<>|", "________");
                if (newName.isNotEmpty() && newName != folderName) {
                    juce::File oldDir(juce::File(safeThis2->m_savePath).getChildFile(folderName));
                    juce::File newDir(juce::File(safeThis2->m_savePath).getChildFile(newName));
                    if (oldDir.isDirectory() && oldDir.moveFileTo(newDir)) {
                        safeThis2->m_currentFolder = newName;
                        safeThis2->scanTakesFromDisk();
                        safeThis2->resized();
                        safeThis2->repaint();
                    }
                }
            }
            delete alert;
        }), false);
}

void MicInputEditor::deleteFolderDialog(const juce::String& folderName)
{
    juce::File dir(juce::File(m_savePath).getChildFile(folderName));
    int wavCount = dir.findChildFiles(juce::File::findFiles, false, "*.wav").size();
    juce::String msg = wavCount > 0
        ? "Delete \"" + folderName + "\" and its " + juce::String(wavCount)
            + " takes? This cannot be undone."
        : "Delete empty folder \"" + folderName + "\"?";

    auto* alert = new juce::AlertWindow("Delete folder", msg,
                                         juce::MessageBoxIconType::WarningIcon);
    alert->addButton("Delete", 1);
    alert->addButton("Cancel", 0);
    juce::Component::SafePointer<MicInputEditor> safeThis3(this);
    alert->enterModalState(true, juce::ModalCallbackFunction::create(
        [safeThis3, folderName, alert](int result) {
            if (result == 1 && safeThis3 != nullptr) {
                juce::File dir2(juce::File(safeThis3->m_savePath).getChildFile(folderName));
                dir2.deleteRecursively();
                safeThis3->m_currentFolder = {};
                safeThis3->m_takesScrollY  = 0;
                safeThis3->scanTakesFromDisk();
                safeThis3->resized();
                safeThis3->repaint();
            }
            delete alert;
        }), false);
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::openLyrics(int index)
{
    if (index < 0 || index >= (int)m_takes.size()) return;
    auto& t = m_takes.getReference(index);
    if (t.live) return;

    m_lyricsIndex = index;
    m_lyricsOpen  = true;
    m_lyricsDirty = false;

    // Load text into editor
    m_lyricsEditor.setText(t.transcript, false);
    m_lyricsEditor.setVisible(true);
    m_lyricsEditor.grabKeyboardFocus();

    // Show / hide transcribe bar
    bool hasTx  = t.transcript.isNotEmpty();
    bool hasKey = m_proc.hasModel();
    m_lyricsTxBtn .setVisible(!hasTx && hasKey);
    m_lyricsTxBtn .setEnabled(!t.transcribing);

    // Hide Takes-tab widgets so they don't show through the lyrics panel
    m_openFolderBtn  .setVisible(false);
    m_changeFolderBtn.setVisible(false);
    m_autoSaveToggle .setVisible(false);
    m_newFolderBtn    .setVisible(false);
    m_folderUpBtn     .setVisible(false);
    m_renameFolderBtn .setVisible(false);
    m_deleteFolderBtn .setVisible(false);
    m_loopRecBtn      .setVisible(false);
    m_meter           .setVisible(false);

    m_lyricsBackBtn  .setVisible(true);
    m_lyricsCopyBtn  .setVisible(true);
    m_lyricsSaveBtn  .setVisible(true);
    m_lyricsDeleteBtn.setVisible(true);
    m_lyricsPlayBtn  .setVisible(true);

    // Load WAV into player on a background thread to avoid blocking the UI
    // Use SafePointer so the callAsync does nothing if editor was destroyed first
    juce::File activeDir = m_currentFolder.isEmpty()
        ? juce::File(m_savePath)
        : juce::File(m_savePath).getChildFile(m_currentFolder);
    juce::File wav(activeDir.getChildFile(t.name));
    m_player.stop();
    juce::String wavPath = wav.getFullPathName();
    // Use SafePointer only — do NOT capture raw 'this' in detached thread.
    // Access m_player via safeThis to avoid dangling pointer if editor closes.
    juce::Component::SafePointer<MicInputEditor> safeThis(this);
    std::thread([safeThis, wavPath] {
        if (safeThis == nullptr) return;
        safeThis->m_player.load(wavPath);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr) safeThis->repaint();
        });
    }).detach();

    resized(); repaint();
}

void MicInputEditor::closeLyrics()
{
    if (m_lyricsDirty) saveLyrics();   // auto-save on back
    m_player.stop();
    m_lyricsOpen  = false;
    m_lyricsIndex = -1;
    m_lyricsDirty = false;

    m_lyricsEditor   .setVisible(false);
    m_lyricsBackBtn  .setVisible(false);
    m_lyricsCopyBtn  .setVisible(false);
    m_lyricsSaveBtn  .setVisible(false);
    m_lyricsDeleteBtn.setVisible(false);
    m_lyricsPlayBtn  .setVisible(false);
    m_lyricsTxBtn    .setVisible(false);

    // Restore Takes-tab widgets based on current folder state
    m_openFolderBtn  .setVisible(true);
    m_changeFolderBtn.setVisible(true);
    m_autoSaveToggle .setVisible(true);
    bool inSub = m_currentFolder.isNotEmpty();
    m_newFolderBtn    .setVisible(!inSub);
    m_folderUpBtn     .setVisible( inSub);
    m_renameFolderBtn .setVisible( inSub);
    m_deleteFolderBtn .setVisible( inSub);
    m_loopRecBtn      .setVisible(true);
    m_meter           .setVisible(true);

    resized(); repaint();
}

void MicInputEditor::saveLyrics()
{
    if (m_lyricsIndex < 0 || m_lyricsIndex >= (int)m_takes.size()) return;
    auto& t = m_takes.getReference(m_lyricsIndex);
    juce::String text = m_lyricsEditor.getText();
    t.transcript  = text;
    m_lyricsDirty = false;
    // Prepend BPM/key metadata header so it survives round-trips through scanTakesFromDisk
    juce::String header;
    if (t.bpm > 0.f)        header += "bpm: " + juce::String((int)t.bpm) + "\n";
    if (t.key.isNotEmpty()) header += "key: " + t.key + "\n";
    if (header.isNotEmpty()) header += "\n";
    juce::File activeDir = m_currentFolder.isEmpty()
        ? juce::File(m_savePath)
        : juce::File(m_savePath).getChildFile(m_currentFolder);
    activeDir.getChildFile(t.name).withFileExtension("txt").replaceWithText(header + text);
    repaint();
}

void MicInputEditor::deleteTake(int index)
{
    if (index < 0 || index >= (int)m_takes.size()) return;
    auto& t = m_takes.getReference(index);
    juce::File activeDir = m_currentFolder.isEmpty()
        ? juce::File(m_savePath)
        : juce::File(m_savePath).getChildFile(m_currentFolder);
    juce::File wav(activeDir.getChildFile(t.name));
    juce::File txt = wav.withFileExtension("txt");
    m_lyricsDirty = false;   // prevent closeLyrics() calling saveLyrics() on file we're deleting
    m_player.stop();
    closeLyrics();
    wav.deleteFile();
    txt.deleteFile();
    scanTakesFromDisk();
}

void MicInputEditor::switchTab(Tab t)
{
    cancelRename();
    if (m_lyricsOpen) {
        // Force-close lyrics without saving when switching tabs
        m_player.stop();
        m_lyricsOpen = false; m_lyricsIndex = -1; m_lyricsDirty = false;
        m_lyricsEditor.setVisible(false);
        for (auto* b : { &m_lyricsBackBtn, &m_lyricsCopyBtn, &m_lyricsSaveBtn,
                         &m_lyricsDeleteBtn, &m_lyricsPlayBtn, &m_lyricsTxBtn })
            b->setVisible(false);
    }
    m_activeTab = t;
    bool rec  = (t == Tab::Record);
    bool tak  = (t == Tab::Takes);
    bool sett = (t == Tab::Settings);

    // Refresh file list from disk every time user opens Takes
    if (tak) scanTakesFromDisk();

    m_deviceCombo      .setVisible(rec);
    m_deviceRefreshBtn .setVisible(rec);
    m_meter            .setVisible(rec || tak);
    m_recBtn           .setVisible(rec);
    m_monitorBtn       .setVisible(rec);
    m_monitorVol       .setVisible(rec && m_proc.isMonitorEnabled());

    m_openFolderBtn    .setVisible(tak);
    m_changeFolderBtn  .setVisible(tak);
    m_autoSaveToggle   .setVisible(tak);
    // Folder nav: root shows "+ Folder"; subfolder shows Back/Rename/Delete
    bool inSubfolder = m_currentFolder.isNotEmpty();
    m_newFolderBtn    .setVisible(tak && !inSubfolder);
    m_folderUpBtn     .setVisible(tak &&  inSubfolder);
    m_renameFolderBtn .setVisible(tak &&  inSubfolder);
    m_deleteFolderBtn .setVisible(tak &&  inSubfolder);

    m_prebufSlider     .setVisible(sett);
    m_prebufValueLabel .setVisible(sett);
    for (auto* b : { &m_modelTiny, &m_modelBase, &m_modelSmall,
                     &m_modelMedium, &m_modelLarge, &m_modelDownloadBtn })
        b->setVisible(sett);
    // Cancel only visible when actively downloading
    m_modelCancelBtn .setVisible(sett && m_downloader.isRunning());
    m_modelUnloadBtn .setVisible(false);  // removed from UI
    m_langCombo      .setVisible(sett);
    m_noSpeechSlider .setVisible(sett);
    m_noSpeechLabel  .setVisible(sett);
    m_gainSlider     .setVisible(sett);
    m_gainLabel      .setVisible(sett);
    m_gateSlider     .setVisible(sett);
    m_gateLabel      .setVisible(sett);
    m_loopRecBtn     .setVisible(tak);
    m_midiNoteCombo  .setVisible(sett);

    if (sett)
        m_prebufValueLabel.setText(juce::String(m_proc.getPrebufMs(), 1) + " ms",
                                    juce::dontSendNotification);
        m_noSpeechLabel.setText(juce::String(m_proc.getNoSpeechThold(), 2),
                                 juce::dontSendNotification);
        m_gainLabel.setText(juce::String(m_proc.getInputGain(), 2) + "x",
                             juce::dontSendNotification);
        m_gateLabel.setText(juce::String(m_proc.getNoiseGateThreshold(), 2),
                             juce::dontSendNotification);
        // Sync loop rec button state
        bool loopOn = m_proc.getLoopRecEnabled();
        if (m_loopRecBtn.getToggleState() != loopOn)
            m_loopRecBtn.setToggleState(loopOn, juce::dontSendNotification);
        // Update download button label
        using MS2 = WhisperClient::ModelSize;
        MS2 curM = m_proc.getWhisperModel();
        bool alreadyHave = WhisperClient::modelExists(curM);
        m_modelDownloadBtn.setButtonText(
            m_downloader.isRunning() ? "Downloading..."
            : alreadyHave ? "Re-download " + WhisperClient::modelName(curM)
            : "Download " + WhisperClient::modelLabel(curM));
        // Keep Cancel visible state synced
        m_modelCancelBtn.setVisible(m_activeTab == Tab::Settings && m_downloader.isRunning());
    repaint(); resized();
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::timerCallback()
{
    m_capturing = m_proc.isCapturing.load();
    m_lastUnderrun = m_proc.underruns.load();

    // Finalise any async-stopped auto-save recording.
    // requestStop() was called from the audio thread; we complete the join
    // and file finalisation here on the message thread.
    if (m_pendingFinish && !m_proc.isRecording())
    {
        m_pendingFinish = false;
        m_recBtn.setEnabled(true);  // safe to record again after finalisation
        m_proc.finishRecorderAsync();
        scanTakesFromDisk();
    }
    // (Manual stops are handled synchronously in recBtn.onClick — no extra check needed here)

    // Repaint when player is active or transcription is in progress
    if (m_lyricsOpen && (m_player.isPlaying() || m_player.isLoaded()))
        repaint();
    // Lock nav tabs during transcription — setEnabled(false) prevents click AND visual select
    {
        bool txing = m_proc.isTranscribing();
        bool wasLocked = !m_navRecord.isEnabled();
        if (txing != wasLocked) {
            m_navRecord  .setEnabled(!txing);
            m_navSettings.setEnabled(!txing);
            if (txing) {
                // Force Takes tab active so user sees the progress bar
                if (m_activeTab != Tab::Takes)
                    switchTab(Tab::Takes);
                m_navTakes.setToggleState(true, juce::dontSendNotification);
            } else {
                // Re-enable: restore correct toggle state for current tab
                m_navRecord  .setToggleState(m_activeTab == Tab::Record,   juce::dontSendNotification);
                m_navTakes   .setToggleState(m_activeTab == Tab::Takes,    juce::dontSendNotification);
                m_navSettings.setToggleState(m_activeTab == Tab::Settings, juce::dontSendNotification);
            }
        }
        if (txing) repaint();  // drive progress bar animation
    }

    // ── Peak hold decay + clip latch ─────────────────────────────────────────
    {
        float procPeak = m_proc.peakHold.load();
        if (procPeak > m_peakHoldVal) {
            m_peakHoldVal = procPeak;
            m_proc.peakHold.store(0.f);  // reset after reading
        } else {
            m_peakHoldVal *= 0.97f;  // ~3s decay at 30Hz
        }
        if (m_proc.clipDetected.exchange(false)) {
            m_clipLatchMs = juce::Time::getMillisecondCounterHiRes();
        }
    }

    // ── MIDI record trigger ────────────────────────────────────────────────────
    if (m_proc.getMidiRecTrigger() && m_activeTab == Tab::Record) {
        bool wantsOn = !m_isRecording;
        m_recBtn.setToggleState(wantsOn, juce::sendNotification);  // triggers onClick
    }

    // ── Recording state sync — runs on every tick regardless of active tab ────
    // This ensures auto-save works even when user is on Takes or Settings tab.
    {
        bool procRec = m_proc.isRecording();
        if (procRec && !m_isRecording)
        {
            // Processor started recording (DAW arm + play) — reflect in editor
            m_isRecording = true;
            m_recBtn.setToggleState(true, juce::dontSendNotification);
            m_recBtn.setEnabled(false);  // prevent manual interference during auto-save
            bool hasLive = false;
            for (auto& t : m_takes) if (t.live) { hasLive = true; break; }
            if (!hasLive) {
                Take t;
                t.name      = m_proc.getCurrentRecordingName();
                if (t.name.isEmpty())
                    t.name = "take_" + juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S") + ".wav";
                t.duration  = "0:00";
                t.size      = "saving...";
                t.latencyMs = m_proc.measuredLatMs.load();
                t.bpm       = m_proc.sessionBpm.load();  // stamp DAW BPM
                t.live      = true;
                m_takes.insert(0, t);
            }
        }
        else if (!procRec && m_isRecording)
        {
            // Processor stopped recording (DAW stopped or manual stop handled it)
            // Queue finalisation regardless of how recording started
            m_isRecording  = false;
            m_recBtn.setToggleState(false, juce::dontSendNotification);
            m_recBtn.setEnabled(true);   // re-enable now that auto-save finished
            m_pendingFinish = true;
        }

        // Keep live take duration ticking on every timer callback
        if (m_isRecording && !m_takes.isEmpty() && m_takes[0].live) {
            double secs = m_proc.recordingSeconds();
            int m_ = (int)(secs / 60.0), s_ = (int)secs % 60;
            m_takes.getReference(0).duration = juce::String::formatted("%d:%02d", m_, s_);
        }

        // Meter update — always needed for both Record and Takes tabs
        if (m_activeTab == Tab::Record || m_activeTab == Tab::Takes)
            m_meter.setLevels(m_proc.levelL.load(), m_proc.levelR.load());
    }

    if (m_activeTab == Tab::Record)
    {
        bool monOn = m_proc.isMonitorEnabled();
        if (m_monitorBtn.getToggleState() != monOn) {
            m_monitorBtn.setToggleState(monOn, juce::dontSendNotification);
        }
        m_monitorBtn.setButtonText(monOn
            ? "Direct monitor  on  -  ~" + juce::String(m_proc.getMonitorLatencyMs(),0) + "ms"
            : "Direct monitor");
        m_monitorVol.setEnabled(monOn);
        m_monitorVol.setVisible(monOn);

    }
    else if (m_activeTab == Tab::Takes)
    {
        bool procSave = m_proc.getSaveEnabled();
        if (m_saveEnabled != procSave) {
            m_saveEnabled = procSave;
            m_autoSaveToggle.setToggleState(procSave, juce::dontSendNotification);
        }
    }
    else
    {
        m_prebufValueLabel.setText(juce::String(m_proc.getPrebufMs(), 1) + " ms",
                                    juce::dontSendNotification);
        m_noSpeechLabel.setText(juce::String(m_proc.getNoSpeechThold(), 2),
                                 juce::dontSendNotification);
        m_gainLabel.setText(juce::String(m_proc.getInputGain(), 2) + "x",
                             juce::dontSendNotification);
        m_gateLabel.setText(juce::String(m_proc.getNoiseGateThreshold(), 2),
                             juce::dontSendNotification);
        // Sync loop rec button state
        bool loopOn = m_proc.getLoopRecEnabled();
        if (m_loopRecBtn.getToggleState() != loopOn)
            m_loopRecBtn.setToggleState(loopOn, juce::dontSendNotification);
        // Update download button label
        using MS2 = WhisperClient::ModelSize;
        MS2 curM = m_proc.getWhisperModel();
        bool alreadyHave = WhisperClient::modelExists(curM);
        m_modelDownloadBtn.setButtonText(
            m_downloader.isRunning() ? "Downloading..."
            : alreadyHave ? "Re-download " + WhisperClient::modelName(curM)
            : "Download " + WhisperClient::modelLabel(curM));
        // Keep Cancel visible state synced
        m_modelCancelBtn.setVisible(m_activeTab == Tab::Settings && m_downloader.isRunning());
    }
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::mouseDown(const juce::MouseEvent& e)
{
    if (m_renameEditor.isVisible() &&
        !m_renameEditor.getBounds().contains(e.getPosition()))
        commitRename();

    // Reset drag-to-folder state from previous gesture
    m_isDraggingTake  = false;
    m_dragHoverFolder = -1;

    // ── Lyrics panel waveform scrub ───────────────────────────────────────────
    if (m_lyricsOpen)
    {
        // Scrub: click on waveform area seeks player
        const int wfX = PAD + 36;
        const int wfW = W - PAD * 2 - 36 - 50;
        const int wfY = BODY_Y + NAV + PAD + 5;
        const int wfH = 24;
        juce::Rectangle<int> wfRect(wfX, wfY, wfW, wfH);
        if (wfRect.contains(e.getPosition()))
        {
            float t2 = (float)(e.x - wfX) / (float)wfW;
            m_player.seek(t2);
            if (!m_player.isPlaying()) m_player.play();
            repaint();
        }

        // ── Word-click alignment: clicking a word label seeks to that word ───
        if (m_lyricsIndex >= 0 && m_lyricsIndex < (int)m_takes.size()
            && m_player.isLoaded())
        {
            const auto& curTake = m_takes.getReference(m_lyricsIndex);
            const float dur = (float)m_player.getDurationSecs();
            if (!curTake.words.empty() && dur > 0.f)
            {
                const int wordRowY = BODY_Y + NAV + PAD + 5 + 12;
                for (const auto& w : curTake.words)
                {
                    if (w.t1 <= 0.f) continue;
                    int wx = wfX + (int)(w.t0 / dur * (float)wfW);
                    juce::Rectangle<int> hit(wx, wordRowY, 40, 10);
                    if (hit.contains(e.getPosition()))
                    {
                        m_player.seek(w.t0 / dur);
                        if (!m_player.isPlaying()) m_player.play();
                        repaint();
                        break;
                    }
                }
            }
        }
        return;   // don't process Takes hit-tests while lyrics open
    }

    if (m_activeTab != Tab::Takes) return;

    const int listTop    = takesListStartY();
    const int listBottom = H - STA - 4;
    const int cw         = W - PAD * 2;
    const int rowH = 36, dirRowH = 30;

    // Hit-test using scroll-adjusted row positions
    // Note: Back/Rename/Delete folder buttons handled by JUCE button onClick — not hit-tested here
    int iy = listTop - m_takesScrollY;
    for (int i = 0; i < (int)m_takes.size(); ++i)
    {
        auto& t = m_takes.getReference(i);
        bool isDir = (t.folder == "__DIR__");
        const int rh = isDir ? dirRowH : rowH;

        // Skip rows outside visible area
        if (iy + rh < listTop) { iy += rh; continue; }
        if (iy > listBottom)   break;

        juce::Rectangle<int> row(PAD, iy, cw, rh);
        if (!row.contains(e.getPosition())) { iy += rh; continue; }

        if (isDir) {
            // Navigate into folder
            m_currentFolder = t.name;
            m_takesScrollY  = 0;
            scanTakesFromDisk();
            // Update button visibility: show Back/Rename/Delete, hide + Folder
            m_newFolderBtn   .setVisible(false);
            m_folderUpBtn    .setVisible(true);
            m_renameFolderBtn.setVisible(true);
            m_deleteFolderBtn.setVisible(true);
            resized(); repaint();
            return;
        }

        // Transcribe hit (small touch target on right)
        if (!t.live && t.transcript.isEmpty() && !t.transcribing && m_proc.hasModel())
        {
            juce::Rectangle<int> txHit(W - PAD - 100, iy + 20, 96, 11);
            if (txHit.contains(e.getPosition()))
            {
                t.transcribing = true; repaint();
                juce::File activeFolder = m_currentFolder.isEmpty()
                    ? juce::File(m_savePath)
                    : juce::File(m_savePath).getChildFile(m_currentFolder);
                juce::String wavPath = activeFolder.getChildFile(t.name).getFullPathName();
                m_proc.transcribe(wavPath, [this, i](juce::String text, juce::String err, std::vector<WhisperWord> words) {
                    juce::MessageManager::callAsync([this, i, text, err, words] {
                        if (i < (int)m_takes.size()) {
                            auto& t2 = m_takes.getReference(i);
                            t2.transcribing = false;
                            if (err.isEmpty()) {
                                t2.transcript = text;
                                t2.words      = words;
                                juce::File af = m_currentFolder.isEmpty()
                                    ? juce::File(m_savePath)
                                    : juce::File(m_savePath).getChildFile(m_currentFolder);
                                af.getChildFile(t2.name).withFileExtension("txt").replaceWithText(text);
                            } else {
                                t2.transcriptError = err;
                            }
                        }
                        repaint();
                    });
                });
                return;
            }
        }

        if (!t.live) {
            // Defer opening lyrics to mouseUp so user can drag without opening
            m_clickIndex = i;
            m_dragIndex  = i;  // also set dragIndex — if drag starts, clickIndex is cleared
        } else {
            m_dragIndex = i;
        }
        return;
    }
    m_clickIndex = -1;
    m_dragIndex  = -1;
}

void MicInputEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (m_activeTab != Tab::Takes || m_lyricsOpen) return;
    if (m_dragIndex < 0 || m_dragIndex >= (int)m_takes.size()) return;
    if (e.getDistanceFromDragStart() < 6) return;

    auto& t = m_takes.getReference(m_dragIndex);
    if (t.live || t.folder == "__DIR__") return;

    // Once threshold exceeded, cancel the pending lyrics-open
    m_clickIndex     = -1;
    m_isDraggingTake = true;

    // ── Check if cursor has left the plugin window ────────────────────────────
    // NativeFileDrag::perform() must be called while the mouse button is DOWN.
    // Detect leaving the window and trigger immediately.
    auto windowBounds = getLocalBounds();
    bool outsideWindow = !windowBounds.contains(e.getPosition());

    if (outsideWindow)
    {
        // Cursor left the plugin — initiate DAW drag now (blocks until drop)
        juce::File activeDir = m_currentFolder.isEmpty()
            ? juce::File(m_savePath)
            : juce::File(m_savePath).getChildFile(m_currentFolder);
        juce::File file = activeDir.getChildFile(t.name);
        if (file.existsAsFile())
        {
            // Reset drag state before blocking call so mouseUp finds nothing to do
            m_dragIndex       = -1;
            m_clickIndex      = -1;
            m_dragHoverFolder = -1;
            m_isDraggingTake  = false;
            repaint();

#if JUCE_WINDOWS
            NativeFileDrag::perform(file.getFullPathName());
#endif
        }
        return;
    }

    // ── Hit-test: is the cursor over a folder row? ────────────────────────────
    const int listTop    = takesListStartY();
    const int listBottom = H - STA - 4;
    const int cw         = W - PAD * 2;
    const int rowH = 36, dirRowH = 30;

    int newHover = -1;
    if (m_currentFolder.isEmpty())  // folders only exist at root view
    {
        int iy = listTop - m_takesScrollY;
        for (int i = 0; i < (int)m_takes.size(); ++i)
        {
            const auto& ti = m_takes.getReference(i);
            bool isDir = (ti.folder == "__DIR__");
            const int rh = isDir ? dirRowH : rowH;
            if (iy + rh >= listTop && iy <= listBottom)
            {
                if (isDir && i != m_dragIndex)
                {
                    juce::Rectangle<int> rowRect(PAD, iy, cw, rh);
                    if (rowRect.contains(e.getPosition()))
                    {
                        newHover = i;
                        break;
                    }
                }
            }
            iy += rh;
        }
    }

    if (newHover != m_dragHoverFolder)
    {
        m_dragHoverFolder = newHover;
        repaint();
    }
}

void MicInputEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (m_lyricsOpen) return;
    if (m_activeTab != Tab::Takes) return;

    const int listTop    = takesListStartY();
    const int listBottom = H - STA - 4;
    const int cw         = W - PAD * 2;
    const int rowH = 36, dirRowH = 30;

    int iy = listTop - m_takesScrollY;
    for (int i = 0; i < (int)m_takes.size(); ++i)
    {
        auto& t = m_takes.getReference(i);
        bool isDir = (t.folder == "__DIR__");
        const int rh = isDir ? dirRowH : rowH;

        if (iy + rh < listTop) { iy += rh; continue; }
        if (iy > listBottom)   break;

        if (t.live || isDir) { iy += rh; continue; }

        // Name hit area for rename
        juce::Rectangle<int> nameHit(PAD + 32, iy + 2, cw - 140, 14);
        if (nameHit.contains(e.getPosition())) {
            showRenameEditor(i, nameHit);
            return;
        }
        iy += rh;
    }
}

void MicInputEditor::mouseUp(const juce::MouseEvent& e)
{
    // ── Case 1: drop onto a folder row → move file ────────────────────────────
    if (m_isDraggingTake && m_dragHoverFolder >= 0
        && m_dragIndex >= 0 && m_dragIndex < (int)m_takes.size()
        && m_dragHoverFolder < (int)m_takes.size())
    {
        auto& take   = m_takes.getReference(m_dragIndex);
        auto& folder = m_takes.getReference(m_dragHoverFolder);

        if (!take.live && take.folder != "__DIR__" && folder.folder == "__DIR__")
        {
            // Move WAV + sidecar .txt into the target folder
            juce::File srcDir = juce::File(m_savePath);  // always root when folders are visible
            juce::File dstDir = srcDir.getChildFile(folder.name);
            dstDir.createDirectory();

            juce::File srcWav = srcDir.getChildFile(take.name);
            juce::File srcTxt = srcWav.withFileExtension("txt");
            juce::File dstWav = dstDir.getChildFile(take.name);

            if (srcWav.existsAsFile())
            {
                srcWav.moveFileTo(dstWav);
                if (srcTxt.existsAsFile())
                    srcTxt.moveFileTo(dstDir.getChildFile(take.name).withFileExtension("txt"));
            }
            m_takesScrollY = 0;
            scanTakesFromDisk();
            repaint();
        }
    }
    // ── Case 2: short click (no drag) → open lyrics ───────────────────────────
    else if (!m_isDraggingTake && m_activeTab == Tab::Takes && !m_lyricsOpen
             && m_clickIndex >= 0 && m_clickIndex < (int)m_takes.size())
    {
        auto& t = m_takes.getReference(m_clickIndex);
        if (!t.live && t.folder != "__DIR__")
            openLyrics(m_clickIndex);
    }
    // Case 3 (DAW drag) is handled in mouseDrag when cursor leaves window bounds

    // Reset all drag state
    m_clickIndex      = -1;
    m_dragIndex       = -1;
    m_dragHoverFolder = -1;
    m_isDraggingTake  = false;
    repaint();
}

void MicInputEditor::mouseWheelMove(const juce::MouseEvent& e,
                                     const juce::MouseWheelDetails& wheel)
{
    if (m_activeTab != Tab::Takes || m_lyricsOpen) return;

    const int listTop    = takesListStartY();
    const int listBottom = H - STA - 4;

    // Only scroll when mouse is over the list area
    if (e.y < listTop || e.y > listBottom) return;

    const int scrollStep = 40;
    m_takesScrollY -= (int)(wheel.deltaY * scrollStep);
    int listH    = listBottom - listTop;
    int maxScroll = std::max(0, m_takeListTotalH - listH);
    m_takesScrollY = std::clamp(m_takesScrollY, 0, maxScroll);
    repaint();
}

int MicInputEditor::takesListStartY() const
{
    int y = BODY_Y + PAD;
    y += 34;  // path row
    y += 30;  // auto-save row
    y += 18;  // meter
    y += 24;  // folder nav row (breadcrumb / new-folder)
    y += 16;  // section header
    return y;
}

void MicInputEditor::showRenameEditor(int index, juce::Rectangle<int> bounds)
{
    cancelRename();
    auto& t = m_takes.getReference(index);
    juce::String stem = t.name.upToLastOccurrenceOf(".wav", false, true);
    if (stem.isEmpty()) stem = t.name;
    m_renameIndex = index;
    m_renameEditor.setText(stem, false);
    m_renameEditor.setBounds(bounds.expanded(2, 2));
    m_renameEditor.setVisible(true);
    m_renameEditor.grabKeyboardFocus();
    m_renameEditor.selectAll();
}

void MicInputEditor::commitRename()
{
    if (m_renameIndex < 0 || m_renameIndex >= (int)m_takes.size()) { cancelRename(); return; }
    juce::String newStem = m_renameEditor.getText().trim();
    newStem = newStem.replaceCharacters("\\/:*?\"<>|", "_________");
    if (newStem.isEmpty()) { cancelRename(); return; }
    juce::String newName = newStem.endsWithIgnoreCase(".wav") ? newStem : newStem + ".wav";
    auto& t = m_takes.getReference(m_renameIndex);
    if (newName == t.name) { cancelRename(); return; }
    juce::File activeDir = m_currentFolder.isEmpty()
        ? juce::File(m_savePath)
        : juce::File(m_savePath).getChildFile(m_currentFolder);
    juce::File oldF(activeDir.getChildFile(t.name));
    juce::File newF(activeDir.getChildFile(newName));
    if (oldF.existsAsFile() && oldF.moveFileTo(newF)) t.name = newName;
    scanTakesFromDisk();  // rescan so list reflects new filename
    cancelRename();
}

void MicInputEditor::cancelRename()
{
    m_renameIndex = -1;
    m_renameEditor.setVisible(false);
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
void MicInputEditor::comboBoxChanged(juce::ComboBox* box)
{
    if (box != &m_deviceCombo) return;
    int id = m_deviceCombo.getSelectedId();
    m_proc.selectDevice(id <= 1 ? -1 : id - 2);
}

void MicInputEditor::sliderValueChanged(juce::Slider* s)
{
    if (s == &m_prebufSlider) {
        float ms = (float)s->getValue();
        m_proc.setPrebufMs(ms);
        m_prebufValueLabel.setText(juce::String(ms, 1) + " ms", juce::dontSendNotification);
        repaint();
    }
}

void MicInputEditor::populateDeviceCombo()
{
    m_deviceCombo.clear(juce::dontSendNotification);
    m_deviceCombo.addItem("System Default", 1);
    auto devs = m_proc.getAvailableDevices();
    for (int i = 0; i < (int)devs.size(); ++i)
        m_deviceCombo.addItem(juce::String(devs[i].name.c_str()), i + 2);
}

void MicInputEditor::drawRecordButton(juce::Graphics&, juce::Rectangle<int>) {}
void MicInputEditor::drawMeter(juce::Graphics&, juce::Rectangle<int>) {}
