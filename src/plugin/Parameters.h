#pragma once
#include <JuceHeader.h>
#include <string_view>

namespace MicInput::Params
{
    // ── Parameter IDs ─────────────────────────────────────────────────────
    constexpr std::string_view MODE      = "mode";    // 0=Shared 1=Exclusive
    constexpr std::string_view GAIN      = "gain";    // 0.0–2.0 input gain (Capture dial)
    constexpr std::string_view GATE      = "gate";    // 0.0–1.0 noise gate threshold
    constexpr std::string_view LOOP_REC  = "looprec"; // 0/1 loop record mode

    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        AudioProcessorValueTreeState::ParameterLayout layout;

        // NOTE: MODE is NOT in APVTS — it is stored in plugin state XML directly
        // so Bitwig/DAW never sees it as an automatable parameter.

        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID{Params::GAIN.data(), 1}, "Input Gain",
            NormalisableRange<float>(0.f, 2.f, 0.01f), 1.f));

        layout.add(std::make_unique<AudioParameterFloat>(
            ParameterID{Params::GATE.data(), 1}, "Noise Gate",
            NormalisableRange<float>(0.f, 1.f, 0.01f), 0.f));

        layout.add(std::make_unique<AudioParameterBool>(
            ParameterID{Params::LOOP_REC.data(), 1}, "Loop Record", false));

        return layout;
    }

} // namespace MicInput::Params
