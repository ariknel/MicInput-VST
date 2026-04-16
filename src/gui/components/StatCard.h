#pragma once
#include <JuceHeader.h>
#include "gui/Colours.h"

// One rounded card displaying a single metric:
//   LABEL (small caps, dimmed)
//   VALUE (large, coloured by latency)
//   SUB   (small, dimmed — mode/device info)
class StatCard : public juce::Component
{
public:
    explicit StatCard(const char* label) : m_label(label) {}

    void setMs(float ms)
    {
        m_value  = juce::String(ms, 1) + "ms";
        m_colour = MicInput::Colours::forLatency(ms);
        m_sub    = "";
    }

    void setMsWithSub(float ms, const juce::String& sub)
    {
        setMs(ms);
        m_sub = sub;
    }

    void setMsAndSamples(float ms, int samples)
    {
        m_value  = juce::String(ms, 1) + "ms";
        m_colour = MicInput::Colours::TEXT;   // block size is neutral
        m_sub    = juce::String(samples) + " samples";
    }

    void setOutputType(bool isUsb, bool isBt, const juce::String& name)
    {
        if (isBt) {
            m_value  = "150ms";
            m_colour = MicInput::Colours::RED;
            m_sub    = "Bluetooth - use wired!";
        } else if (isUsb) {
            m_value  = "+1.5ms";
            m_colour = MicInput::Colours::TEXT;
            m_sub    = "USB";
        } else {
            m_value  = "+0.5ms";
            m_colour = MicInput::Colours::GREEN;
            m_sub    = "Analog 3.5mm";
        }
        // Truncate long device names
        juce::String n = name.substring(0, 18);
        if (n.length() < name.length()) n += "...";
        m_sub += " - " + n;
    }

    void setLatencyTotal(float ms)
    {
        m_value  = juce::String(ms, 1) + "ms";
        m_colour = MicInput::Colours::forLatency(ms);
        m_sub    = MicInput::Colours::qualityLabel(ms);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace MicInput::Colours;
        auto b = getLocalBounds().toFloat().reduced(2.0f);

        g.setColour(CARD);
        g.fillRoundedRectangle(b, 8.0f);
        g.setColour(BORDER);
        g.drawRoundedRectangle(b, 8.0f, 1.0f);

        const int pad = 8;
        const int h   = getHeight();

        // Label
        g.setColour(DIM);
        g.setFont(juce::Font(juce::FontOptions(10.0f)).withExtraKerningFactor(0.05f));
        g.drawText(m_label.toUpperCase(),
                   pad, pad, getWidth() - pad*2, 14,
                   juce::Justification::centred);

        // Value
        g.setColour(m_colour);
        g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
        g.drawText(m_value,
                   pad, h/2 - 16, getWidth() - pad*2, 28,
                   juce::Justification::centred);

        // Sub
        if (m_sub.isNotEmpty())
        {
            g.setColour(DIM);
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.drawText(m_sub,
                       pad, h - pad - 14, getWidth() - pad*2, 14,
                       juce::Justification::centred, true);
        }
    }

private:
    juce::String m_label, m_value, m_sub;
    juce::Colour m_colour = MicInput::Colours::TEXT;
};
