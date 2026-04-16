#include "gui/LookAndFeel.h"

MicInputLookAndFeel::MicInputLookAndFeel()
{
    using namespace MicInput::Colours;

    setColour(juce::ComboBox::backgroundColourId,              CARD);
    setColour(juce::ComboBox::textColourId,                    TEXT);
    setColour(juce::ComboBox::outlineColourId,                 BORDER2);
    setColour(juce::ComboBox::arrowColourId,                   HINT);
    setColour(juce::PopupMenu::backgroundColourId,             juce::Colour(0xff0f1219));
    setColour(juce::PopupMenu::textColourId,                   TEXT);
    setColour(juce::PopupMenu::highlightedBackgroundColourId,  ACCENT.withAlpha(0.18f));
    setColour(juce::PopupMenu::highlightedTextColourId,        ACCENT_LT);
    setColour(juce::TextButton::buttonColourId,                CARD);
    setColour(juce::TextButton::buttonOnColourId,              ACCENT.withAlpha(0.18f));
    setColour(juce::TextButton::textColourOnId,                ACCENT_LT);
    setColour(juce::TextButton::textColourOffId,               DIM);
    setColour(juce::Label::textColourId,                       TEXT);
    setColour(juce::Label::backgroundColourId,                 juce::Colour(0x00000000));
    setColour(juce::TextEditor::backgroundColourId,            juce::Colour(0xff131720));
    setColour(juce::TextEditor::textColourId,                  TEXT);
    setColour(juce::TextEditor::outlineColourId,               BORDER2);
    setColour(juce::TextEditor::focusedOutlineColourId,        ACCENT);
    setColour(juce::Slider::thumbColourId,                     juce::Colours::white);
    setColour(juce::Slider::trackColourId,                     ACCENT);
    setColour(juce::Slider::backgroundColourId,                juce::Colour(0x30ffffff));
    setColour(juce::ScrollBar::thumbColourId,                  BORDER2);
}

void MicInputLookAndFeel::drawComboBox(juce::Graphics& g, int w, int h,
                                        bool, int, int, int, int,
                                        juce::ComboBox& box)
{
    using namespace MicInput::Colours;
    auto b = juce::Rectangle<float>(0.f, 0.f, (float)w, (float)h);

    // Gradient fill
    juce::ColourGradient cg(juce::Colour(0xff1a1e2b), 0.f, 0.f,
                             juce::Colour(0xff13172000), 0.f, (float)h, false);
    // solid fill instead
    g.setColour(juce::Colour(0xff161a25));
    g.fillRoundedRectangle(b, 6.f);
    bool focused = box.hasKeyboardFocus(false);
    g.setColour(focused ? ACCENT.withAlpha(0.7f) : BORDER2);
    g.drawRoundedRectangle(b.reduced(0.5f), 6.f, focused ? 1.f : 0.5f);

    // Chevron
    float arrowX = w - 14.f, arrowY = h * 0.5f;
    juce::Path arrow;
    arrow.addTriangle(arrowX - 4.f, arrowY - 2.f,
                      arrowX + 4.f, arrowY - 2.f,
                      arrowX,       arrowY + 3.f);
    g.setColour(HINT);
    g.fillPath(arrow);
}

void MicInputLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                                juce::Button& btn,
                                                const juce::Colour&,
                                                bool highlighted, bool down)
{
    using namespace MicInput::Colours;
    auto b = btn.getLocalBounds().toFloat();

    // Painted-manually buttons — skip LookAndFeel background entirely
    if (btn.getName() == "REC_BTN" || btn.getName() == "ICON_BTN") return;

    if (btn.getToggleState())
    {
        // Active state: blue glow fill
        juce::ColourGradient ag(ACCENT.withAlpha(0.25f), b.getCentreX(), b.getY(),
                                 ACCENT.withAlpha(0.10f), b.getCentreX(), b.getBottom(), false);
        g.setGradientFill(ag);
        g.fillRoundedRectangle(b, 6.f);
        g.setColour(ACCENT.withAlpha(0.6f));
        g.drawRoundedRectangle(b.reduced(0.5f), 6.f, 1.f);
    }
    else
    {
        juce::Colour fill = down       ? juce::Colour(0x1affffff)
                          : highlighted ? juce::Colour(0x12ffffff)
                                        : juce::Colour(0x08ffffff);
        g.setColour(fill);
        g.fillRoundedRectangle(b, 6.f);
        g.setColour(down ? BORDER2 : juce::Colour(0x18ffffff));
        g.drawRoundedRectangle(b.reduced(0.5f), 6.f, 0.5f);
    }
}

void MicInputLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& btn,
                                          bool highlighted, bool)
{
    using namespace MicInput::Colours;
    auto text = btn.getButtonText();

    if (btn.getName() == "REC_BTN" || btn.getName() == "ICON_BTN") return;

    // Refresh icon ("R" button)
    if (text == "R")
    {
        auto b = btn.getLocalBounds().toFloat().reduced(7.f);
        g.setColour(highlighted ? ACCENT_LT : HINT);
        juce::Path arc;
        arc.addArc(b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                   0.4f, 0.4f + juce::MathConstants<float>::twoPi * 0.80f, true);
        g.strokePath(arc, juce::PathStrokeType(1.4f,
            juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        float cx = b.getCentreX(), ty = b.getY() + 1.f;
        juce::Path head;
        head.addTriangle(cx - 3.f, ty + 4.f, cx + 3.f, ty + 4.f, cx, ty);
        g.fillPath(head);
        return;
    }

    juce::Colour col = btn.getToggleState() ? ACCENT_LT
                     : highlighted          ? TEXT
                     : btn.isEnabled()      ? DIM
                                            : HINT;
    g.setColour(col);
    g.setFont(juce::Font(juce::FontOptions(11.f)));
    g.drawText(text, btn.getLocalBounds(), juce::Justification::centred, true);
}

void MicInputLookAndFeel::drawLinearSlider(juce::Graphics& g,
                                            int x, int y, int w, int h,
                                            float sliderPos,
                                            float, float,
                                            juce::Slider::SliderStyle style,
                                            juce::Slider&)
{
    using namespace MicInput::Colours;
    if (style != juce::Slider::LinearHorizontal) return;

    const float trackH = 4.f;
    const float thumbR = 6.f;
    const float trackY = y + h * 0.5f - trackH * 0.5f;
    const float trackX = (float)x + thumbR;
    const float trackW = (float)w - thumbR * 2.f;

    // Track background
    g.setColour(juce::Colour(0x25ffffff));
    g.fillRoundedRectangle(trackX, trackY, trackW, trackH, 2.f);

    // Filled portion with gradient
    float filled = sliderPos - trackX;
    if (filled > 0.f) {
        juce::ColourGradient tg(ACCENT_LT, trackX, trackY,
                                 ACCENT, trackX + filled, trackY, false);
        g.setGradientFill(tg);
        g.fillRoundedRectangle(trackX, trackY, filled, trackH, 2.f);
    }

    // Thumb — white circle with blue ring
    const float tx  = sliderPos - thumbR;
    const float ty2 = y + h * 0.5f - thumbR;
    g.setColour(juce::Colour(0xff222840));
    g.fillEllipse(tx, ty2, thumbR * 2.f, thumbR * 2.f);
    g.setColour(juce::Colours::white);
    g.fillEllipse(tx + 2.f, ty2 + 2.f, thumbR * 2.f - 4.f, thumbR * 2.f - 4.f);
    g.setColour(ACCENT.withAlpha(0.6f));
    g.drawEllipse(tx, ty2, thumbR * 2.f, thumbR * 2.f, 1.f);
}
