# MicInput VST3
Webpage https://ariknel.github.io/MicInput-VST/**
A microphone capture plugin for Windows DAWs. Record takes directly inside the plugin — with automatic save, folder management, Whisper AI transcription, pitch detection, and waveform previews.

Built for rappers, producers, and vocalists who want a dedicated recording workflow inside Bitwig, Ableton, FL Studio, or Reaper.

---

## Download
👉**[Webpage Download]https://ariknel.github.io/MicInput-VST/**
👉 **[Download MicInput-Installer.exe](https://github.com/ariknel/micinput-vst/releases/latest)**

---

## Installation

1. Download `MicInput-Installer.exe` from the link above
2. Double-click it and follow the wizard
3. Default install path: `C:\Program Files\Common Files\VST3\`
4. Open your DAW → rescan VST3 plugins → add **MicInput** to an Instrument Track

---

## Features
<img width="655" height="628" alt="afbeelding" src="https://github.com/user-attachments/assets/c981ff4c-664c-470d-9f5f-f637ae994924" />

**Recording**
- Captures microphone audio via WASAPI at native sample rate
- Manual record button or auto-save when DAW starts recording
- Loop record mode — each loop pass saved as a separate take automatically
- MIDI trigger — assign any MIDI note to start/stop recording

**Takes Management**
- Scrollable take list with waveform thumbnail previews
- BPM stamp (reads from DAW transport when recording starts)
- Pitch / key detection on each take (YIN algorithm, runs after recording)
- Drag takes into folders to organise sessions
- Drag takes directly to DAW timeline
- Inline rename, colour-coded live indicator
<img width="665" height="644" alt="afbeelding" src="https://github.com/user-attachments/assets/2a2ee92b-7452-48e5-a6e9-0369371d9f00" />

**Folders**
- Create, rename, delete folders inside the plugin
- Drag and drop takes between folders
<img width="1207" height="681" alt="afbeelding" src="https://github.com/user-attachments/assets/47ca3f23-4fc8-4073-8511-2088032704c9" />

**Transcription (Whisper AI)**
- Local transcription — no internet required, no API key
- Five model sizes: Tiny (fast) to Large (accurate)
- Word-level timestamps with alignment view in lyrics editor
- 18 language options with auto-detect
- Progress bar during transcription

**Signal Chain**
- Input gain (0–2×) — automatable from DAW, maps to Capture dial
- Noise gate with attack/release smoothing
- Clip indicator with peak hold
- Direct monitor with adjustable volume

**DAW Integration**
- Reads BPM and transport state from playhead
- Auto-save mirrors DAW record arm + play
- Compatible: Bitwig Studio, Ableton Live, FL Studio, Reaper

---

## Requirements

- Windows 10 or 11 (64-bit)
- Any VST3-compatible DAW
- Microphone (USB or audio interface)

---

## Usage

### Basic recording
1. Load MicInput on an **Instrument Track** (not Audio Track)
2. Select your microphone from the dropdown
3. Press the record button — takes appear in the Takes tab

### Auto-save with DAW
1. Enable **Auto-save every recording** toggle in the Takes tab
2. Arm your DAW track and press Play+Record in your DAW
3. MicInput records automatically alongside your DAW session

### Transcription
1. Go to Settings → choose a Whisper model → click Download
2. On any take, click **Transcribe**
3. Edit the transcript in the lyrics editor — word timestamps shown on waveform

### Folders
- Click **+ Folder** to create a folder
- Drag takes onto folders to move them
- Enter a folder with a single click, exit with **< Back**

---

## Building from Source

Requires: Visual Studio 2022 Build Tools, CMake, Git, Ninja

```bat
git clone https://github.com/YOUR_USERNAME/micinput-vst.git
cd micinput-vst
build.bat
```

First build downloads JUCE (~120 MB) and whisper.cpp (~30 MB) automatically.

To build the installer (requires [Inno Setup 5](https://jrsoftware.org/isdl.php)):

```bat
make_installer.bat
```

Output: `dist\MicInput-Installer.exe`

---

## License

MIT License — see [LICENSE](LICENSE) for details.

Whisper model weights are provided by OpenAI under the MIT License.
