#pragma once
#include <JuceHeader.h>
#include "gui/Colours.h"
#include <cmath>
#include <algorithm>

// Single wide bar matching the mockup — green->amber->red gradient, dB label left
class LevelMeter : public juce::Component
{
public:
    void setLevels(float rmsL, float rmsR)
    {
        float rms = std::max(rmsL, rmsR);
        m_rms = rms;
        if (rms >= m_peak) { m_peak = rms; m_peakHold = 60; }
        else if (m_peakHold > 0) --m_peakHold;
        else m_peak = std::max(0.0f, m_peak - 0.008f);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        using namespace MicInput::Colours;
        const int w = getWidth();
        const int h = getHeight();

        // Track background
        g.setColour(juce::Colour(0x1affffff));
        g.fillRoundedRectangle(0.f, 0.f, (float)w, (float)h, (float)h * 0.5f);
        g.setColour(juce::Colour(0x12ffffff));
        g.drawRoundedRectangle(0.5f, 0.5f, w - 1.f, h - 1.f, (float)h * 0.5f, 0.8f);

        // Level fill with green->amber->red gradient
        if (m_rms > 0.001f)
        {
            float fillW = m_rms * (float)w;
            juce::ColourGradient grad(M_LOW, 0.f, 0.f, M_PEAK, (float)w, 0.f, false);
            grad.addColour(0.65, M_MID);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(0.f, 0.f, fillW, (float)h, (float)h * 0.5f);
        }

        // Peak marker
        if (m_peak > 0.01f)
        {
            int px = (int)(m_peak * (float)w);
            g.setColour(juce::Colour(0xaa1a3060));
            g.fillRect(px - 1, 1, 2, h - 2);
        }
    }

private:
    float m_rms   = 0.f;
    float m_peak  = 0.f;
    int   m_peakHold = 0;
};
