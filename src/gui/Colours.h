#pragma once
#include <JuceHeader.h>

namespace MicInput::Colours
{
    // ── Core surfaces ─────────────────────────────────────────────────────────
    inline const juce::Colour BG        { 0xff0e1117 };  // deep navy-black body
    inline const juce::Colour BG2       { 0xff13161e };  // slightly lighter body variant
    inline const juce::Colour CHROME    { 0xff090c12 };  // header / statusbar
    inline const juce::Colour CARD      { 0xff181c26 };  // card surface
    inline const juce::Colour CARD2     { 0xff111520 };  // deeper card / rec zone

    // ── Borders ───────────────────────────────────────────────────────────────
    inline const juce::Colour BORDER    { 0x18ffffff };  // subtle
    inline const juce::Colour BORDER2   { 0x28ffffff };  // emphasis

    // ── Text ─────────────────────────────────────────────────────────────────
    inline const juce::Colour TEXT      { 0xfff0f2f8 };  // primary
    inline const juce::Colour DIM       { 0x99f0f2f8 };  // secondary
    inline const juce::Colour HINT      { 0x4df0f2f8 };  // muted

    // ── Accent — electric blue ────────────────────────────────────────────────
    inline const juce::Colour ACCENT    { 0xff4f8ef7 };  // primary blue
    inline const juce::Colour ACCENT_LT { 0xff7fb3ff };  // lighter blue
    inline const juce::Colour ACCENT_DK { 0xff2563d4 };  // darker blue for glow base

    // ── Semantic ─────────────────────────────────────────────────────────────
    inline const juce::Colour GREEN     { 0xff22c55e };
    inline const juce::Colour GREEN_LT  { 0xff4ade80 };
    inline const juce::Colour AMBER     { 0xfff59e0b };
    inline const juce::Colour AMBER_LT  { 0xfffbbf24 };
    inline const juce::Colour RED       { 0xffef4444 };
    inline const juce::Colour RED_LT    { 0xfff87171 };

    // ── Meter ─────────────────────────────────────────────────────────────────
    inline const juce::Colour M_LOW     { 0xff22c55e };
    inline const juce::Colour M_MID     { 0xfff59e0b };
    inline const juce::Colour M_PEAK    { 0xffef4444 };

    inline juce::Colour forLatency(float ms)
    {
        if (ms < 15.f) return GREEN;
        if (ms < 25.f) return AMBER;
        return RED;
    }
    inline const char* qualityLabel(float ms)
    {
        if (ms < 10.f) return "Imperceptible";
        if (ms < 15.f) return "Excellent";
        if (ms < 20.f) return "Very Good";
        if (ms < 30.f) return "Acceptable";
        if (ms < 50.f) return "Noticeable";
        return "High";
    }
}
