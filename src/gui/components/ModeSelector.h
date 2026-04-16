#pragma once
#include <JuceHeader.h>
#include "gui/Colours.h"
#include "audio/DeviceProber.h"
#include <functional>

// Shows Shared / Exclusive mode toggle + recommendation panel.
// All state is set externally via setters. Component just displays.
class ModeSelector : public juce::Component,
                     private juce::Button::Listener
{
public:
    std::function<void(int)> onModeChanged;

    ModeSelector()
    {
        m_sharedBtn.setButtonText("Shared");
        m_sharedBtn.setClickingTogglesState(true);
        m_sharedBtn.setRadioGroupId(1);
        m_sharedBtn.setToggleState(true, juce::dontSendNotification);
        m_sharedBtn.addListener(this);
        addAndMakeVisible(m_sharedBtn);

        m_excBtn.setButtonText("Exclusive");
        m_excBtn.setClickingTogglesState(true);
        m_excBtn.setRadioGroupId(1);
        m_excBtn.addListener(this);
        addAndMakeVisible(m_excBtn);
    }

    ~ModeSelector() override
    {
        m_sharedBtn.removeListener(this);
        m_excBtn.removeListener(this);
    }

    void setDeviceProfile(const DeviceProfile& dp,
                          const OutputProfile& op,
                          float blockMs)
    {
        m_dp      = dp;
        m_op      = op;
        m_blockMs = blockMs;
        repaint();
    }

    void setCurrentMode(int mode)
    {
        if (mode == 0) m_sharedBtn.setToggleState(true, juce::dontSendNotification);
        else           m_excBtn.setToggleState(true, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto row = getLocalBounds().removeFromTop(30);
        m_sharedBtn.setBounds(row.removeFromLeft(getWidth() / 2).reduced(2));
        m_excBtn.setBounds(row.reduced(2));
    }

    void paint(juce::Graphics& g) override
    {
        using namespace MicInput::Colours;

        // Info panel below the buttons
        auto panel = getLocalBounds().withTrimmedTop(32).reduced(2);
        g.setColour(CARD);
        g.fillRoundedRectangle(panel.toFloat(), 6.0f);
        g.setColour(BORDER);
        g.drawRoundedRectangle(panel.toFloat(), 6.0f, 1.0f);

        if (m_sharedBtn.getToggleState())
            drawSharedPanel(g, panel);
        else
            drawExclusivePanel(g, panel);
    }

private:
    void buttonClicked(juce::Button* btn) override
    {
        int mode = (btn == &m_excBtn) ? 1 : 0;
        if (onModeChanged) onModeChanged(mode);
        repaint();
    }

    void drawSharedPanel(juce::Graphics& g, juce::Rectangle<int> b)
    {
        using namespace MicInput::Colours;
        const float capMs  = m_dp.bestSharedPeriodMs();
        const float total  = 2.0f + capMs + m_blockMs * 0.5f + m_op.latencyMs;

        int y = b.getY() + 8;
        const int x = b.getX() + 8;
        const int w = b.getWidth() - 16;

        g.setFont(juce::Font(juce::FontOptions(11.0f)));

        // Latency summary line
        g.setColour(forLatency(total));
        g.drawText("Capture: " + juce::String(capMs, 1) + "ms   "
                   "Total monitoring: ~" + juce::String(total, 1) + "ms",
                   x, y, w, 16, juce::Justification::centredLeft);
        y += 18;

        if (!m_dp.supportsIAC3SmallBuf)
        {
            // Amber warning — generic driver
            drawBadge(g, AMBER, "AMBER", x, y, w);
            y += 18;
            g.setColour(DIM);
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.drawText("Driver limited to 10ms. Exclusive = " +
                        juce::String(2.0f + m_dp.exclusiveMinPeriodMs + m_blockMs*0.5f + m_op.latencyMs, 1) + "ms",
                       x, y, w, 14, juce::Justification::centredLeft);
        }
        else
        {
            drawBadge(g, GREEN, "Low-latency mode active (IAudioClient3)", x, y, w);
        }
    }

    void drawExclusivePanel(juce::Graphics& g, juce::Rectangle<int> b)
    {
        using namespace MicInput::Colours;

        if (!m_dp.supportsExclusive)
        {
            drawBadge(g, RED, "Exclusive not supported by this device", b.getX()+8, b.getY()+8, b.getWidth()-16);
            return;
        }

        const float total = 2.0f + m_dp.exclusiveMinPeriodMs + m_blockMs*0.5f + m_op.latencyMs;
        int y = b.getY() + 8;
        const int x = b.getX() + 8;
        const int w = b.getWidth() - 16;

        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.setColour(GREEN);
        g.drawText("Capture: " + juce::String(m_dp.exclusiveMinPeriodMs, 1) + "ms   "
                   "Total monitoring: ~" + juce::String(total, 1) + "ms",
                   x, y, w, 16, juce::Justification::centredLeft);
        y += 18;

        if (!m_op.activeAppNames.empty())
        {
            drawBadge(g, RED, "Apps that will lose audio:", x, y, w);
            y += 18;
            g.setColour(RED.withAlpha(0.8f));
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            for (auto& name : m_op.activeAppNames)
            {
                g.drawText("  - " + juce::String(name.c_str()), x, y, w, 13,
                            juce::Justification::centredLeft);
                y += 13;
            }
        }
        else
        {
            drawBadge(g, AMBER, "System audio exclusive to this plugin", x, y, w);
        }
    }

    void drawBadge(juce::Graphics& g, juce::Colour col,
                   const juce::String& text,
                   int x, int y, int w)
    {
        g.setColour(col.withAlpha(0.15f));
        g.fillRoundedRectangle((float)x, (float)y, (float)w, 16.0f, 3.0f);
        g.setColour(col);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(text, x + 6, y, w - 8, 16, juce::Justification::centredLeft);
    }

    juce::TextButton m_sharedBtn, m_excBtn;
    DeviceProfile    m_dp;
    OutputProfile    m_op;
    float            m_blockMs = 2.7f;
};
