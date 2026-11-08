#pragma once
// Identidad visual de hum2midi: grafito frío, notas en verde menta (el color de las notas del
// piano roll de FL, que es donde acaban) y la voz en azul hielo. Tipografía Barlow incrustada.

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

namespace ui
{
namespace col
{
    const juce::Colour bg       { 0xff171a21 };  // ventana
    const juce::Colour panel    { 0xff1c2028 };  // pantalla (pitch roll + lectura)
    const juce::Colour raised   { 0xff252a34 };  // combos, botones
    const juce::Colour line     { 0xff2e3440 };  // bordes y divisores
    const juce::Colour text     { 0xffe8ebf1 };
    const juce::Colour dim      { 0xff8c94a5 };
    const juce::Colour faint    { 0xff545c6c };
    const juce::Colour note     { 0xff6fd6a0 };  // notas detectadas
    const juce::Colour noteEdge { 0xff2f8f62 };
    const juce::Colour trace    { 0xffcfe3ff };  // tu voz
    const juce::Colour rec      { 0xffff5a5f };
}

class HumLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HumLookAndFeel()
    {
        medium   = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowMedium_ttf, BinaryData::BarlowMedium_ttfSize);
        semibold = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowSemiBold_ttf, BinaryData::BarlowSemiBold_ttfSize);
        display  = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowSemiCondensedSemiBold_ttf,
                                                            BinaryData::BarlowSemiCondensedSemiBold_ttfSize);
        setDefaultSansSerifTypeface (medium);

        setColour (juce::ResizableWindow::backgroundColourId, col::bg);
        setColour (juce::Label::textColourId, col::text);
        setColour (juce::Slider::textBoxTextColourId, col::text);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, col::line);
        setColour (juce::TextEditor::textColourId, col::text);
        setColour (juce::TextEditor::highlightColourId, col::line);
        setColour (juce::CaretComponent::caretColourId, col::text);
        setColour (juce::ComboBox::textColourId, col::text);
        setColour (juce::PopupMenu::backgroundColourId, col::raised);
        setColour (juce::PopupMenu::textColourId, col::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, col::line);
        setColour (juce::PopupMenu::highlightedTextColourId, col::text);
        setColour (juce::TooltipWindow::backgroundColourId, col::raised);
        setColour (juce::TooltipWindow::textColourId, col::text);
        setColour (juce::TooltipWindow::outlineColourId, col::line);
        setColour (juce::TextButton::textColourOffId, col::text);
        setColour (juce::TextButton::textColourOnId, col::text);
        setColour (juce::ScrollBar::thumbColourId, col::faint);
        setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    }

    juce::Font font (float h) const    { return juce::Font (juce::FontOptions().withTypeface (medium).withHeight (h)); }
    juce::Font strong (float h) const  { return juce::Font (juce::FontOptions().withTypeface (semibold).withHeight (h)); }
    juce::Font readout (float h) const { return juce::Font (juce::FontOptions().withTypeface (display).withHeight (h)); }

    // ---- Knob: arco fino + cuerpo mate con un puntero. "bipolar" = el arco sale del centro.
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        const auto area = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        const float r = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 2.0f;
        const auto c = area.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const bool bipolar = s.getProperties()["bipolar"];
        const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

        juce::Path track, value;
        track.addCentredArc (c.x, c.y, r, r, 0.0f, startAngle, endAngle, true);
        value.addCentredArc (c.x, c.y, r, r, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        const juce::PathStrokeType stroke (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour (col::line);
        g.strokePath (track, stroke);
        g.setColour (col::text.withAlpha (s.isEnabled() ? 0.9f : 0.3f));
        g.strokePath (value, stroke);

        const float br = r - 6.0f;
        const auto body = juce::Rectangle<float> (br * 2.0f, br * 2.0f).withCentre (c);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2d333e), c.x, c.y - br,
                                                 juce::Colour (0xff1e222a), c.x, c.y + br, false));
        g.fillEllipse (body);
        g.setColour (juce::Colour (0xff3a404d));
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        const auto tip  = c.getPointOnCircumference (br - 4.0f, angle);
        const auto tail = c.getPointOnCircumference (br * 0.35f, angle);
        g.setColour (col::text);
        g.drawLine ({ tail, tip }, 2.0f);
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setColour (juce::Label::textColourId, col::text);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setFont (font (13.5f));
        l->setJustificationType (juce::Justification::centred);
        return l;
    }

    // ---- Interruptor tipo píldora
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        const auto bounds = b.getLocalBounds().toFloat();
        const bool on = b.getToggleState();
        const auto pill = juce::Rectangle<float> (bounds.getX() + 1.0f, bounds.getCentreY() - 8.0f, 30.0f, 16.0f);

        g.setColour (on ? col::note : (over ? col::line.brighter (0.15f) : col::line));
        g.fillRoundedRectangle (pill, 8.0f);
        const float d = 12.0f;
        const float kx = on ? pill.getRight() - d - 2.0f : pill.getX() + 2.0f;
        g.setColour (on ? col::bg : col::dim);
        g.fillEllipse (kx, pill.getCentreY() - d * 0.5f, d, d);

        g.setColour (on ? col::text : col::dim);
        g.setFont (font (14.5f));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (40), juce::Justification::centredLeft, 1);
    }

    // ---- Desplegables planos
    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (col::raised);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (box.isMouseOver (true) ? col::faint : col::line);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);

        juce::Path chevron;
        const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.0f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (col::dim);
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // ---- Botones de texto planos
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (down ? col::line : (over ? col::raised.brighter (0.06f) : col::raised));
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (over ? col::faint : col::line);
        g.drawRoundedRectangle (r, 6.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int) override { return strong (14.5f); }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour (b.isEnabled() ? col::text : col::faint);
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (8, 0), juce::Justification::centred, 1);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (15.0f); }
    juce::Font getPopupMenuFont() override               { return font (15.0f); }
    juce::Font getLabelFont (juce::Label&) override      { return font (13.5f); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (4, 0, box.getWidth() - 26, box.getHeight());
        label.setFont (getComboBoxFont (box));
    }

private:
    juce::Typeface::Ptr medium, semibold, display;
};

} // namespace ui
