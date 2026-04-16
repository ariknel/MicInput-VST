cmake_minimum_required(VERSION 3.22)
project(MicInputVST VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ── whisper.cpp — local on-device STT ────────────────────────────────────────
FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG        v1.7.4
    GIT_SHALLOW    TRUE
)
set(WHISPER_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER    OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)

juce_add_plugin(MicInputVST
    COMPANY_NAME                "OpenMic"
    PLUGIN_MANUFACTURER_CODE    Omic
    PLUGIN_CODE                 Mi01
    FORMATS                     VST3
    PRODUCT_NAME                "MicInput"
    IS_SYNTH                    TRUE        # FALSE = effect/generator, recordable on Audio Track
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             "Instrument|Generator"
    COPY_PLUGIN_AFTER_BUILD     TRUE
)

juce_generate_juce_header(MicInputVST)

target_sources(MicInputVST PRIVATE
    src/audio/WasapiCapture.cpp
    src/plugin/PluginProcessor.cpp
    src/gui/LookAndFeel.cpp
    src/gui/PluginEditor.cpp
    src/gui/components/LevelMeter.cpp
    src/gui/components/ModeSelector.cpp
)

target_include_directories(MicInputVST PRIVATE
    src
    ${whisper_SOURCE_DIR}
    ${whisper_SOURCE_DIR}/include
)

target_compile_definitions(MicInputVST PUBLIC
    NOMINMAX
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(MicInputVST PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_plugin_client
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    mmdevapi avrt ole32 oleaut32 winmm shlwapi shell32 winhttp
    whisper
PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
