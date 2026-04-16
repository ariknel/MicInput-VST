#pragma once
#include <JuceHeader.h>
#include "gui/Colours.h"

class MicInputLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MicInputLookAndFeel();

    void drawComboBox(juce::Graphics&, int w, int h, bool isDown,
                      int, int, int, int, juce::ComboBox&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&,
                              const juce::Colour&,
                              bool highlighted, bool down) override;

    void drawButtonText(juce::Graphics&, juce::TextButton&,
                        bool highlighted, bool down) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h,
                          float sliderPos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;

    juce::Font getComboBoxFont(juce::ComboBox&) override
        { return juce::Font(juce::FontOptions(11.f)); }
    juce::Font getLabelFont(juce::Label&) override
        { return juce::Font(juce::FontOptions(11.f)); }
    juce::Font getPopupMenuFont() override
        { return juce::Font(juce::FontOptions(11.f)); }
};
