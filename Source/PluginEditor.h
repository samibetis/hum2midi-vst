#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "HumLookAndFeel.h"

// ---- Pitch roll: tu voz (línea) y las notas detectadas (bloques) sobre un mini piano roll.
class PitchRoll : public juce::Component
{
public:
    explicit PitchRoll (ui::HumLookAndFeel& l) : lnf (l) { frames.resize (kFrames); }

    void append (const HumAudioProcessor::PitchFrame* f, int n);
    void tick();  // centra la vista en lo que cantas; llamar ~60 veces/s
    void paint (juce::Graphics&) override;

private:
    static constexpr int kFrames = 600;    // ~4.8 s de historia
    static constexpr float kSpan = 16.0f;  // semitonos visibles

    const HumAudioProcessor::PitchFrame& at (int age) const { return frames[(size_t) ((write - 1 - age + 2 * kFrames) % kFrames)]; }

    ui::HumLookAndFeel& lnf;
    std::vector<HumAudioProcessor::PitchFrame> frames;
    int write = 0, count = 0;
    juce::int64 total = 0;  // frames recibidos desde que se abrió (para las marcas de tiempo)
    float centre = 60.0f, target = 60.0f;
    bool anyVoice = false;
};

// ---- Lectura: nota grande, afinador en cents y nivel de entrada con la puerta.
class Readout : public juce::Component
{
public:
    explicit Readout (ui::HumLookAndFeel& l) : lnf (l) {}
    int note = -1;
    float cents = 0.0f;
    bool hasCents = false;
    float levelDb = -100.0f, gateDb = -40.0f;
    void paint (juce::Graphics&) override;

private:
    ui::HumLookAndFeel& lnf;
};

// ---- Botón de grabar
class RecButton : public juce::Button
{
public:
    explicit RecButton (ui::HumLookAndFeel& l) : juce::Button ("rec"), lnf (l) {}
    bool recording = false;
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    ui::HumLookAndFeel& lnf;
};

// ---- Ficha de la toma: se arrastra directamente al piano roll de FL.
class TakeChip : public juce::Component
{
public:
    explicit TakeChip (ui::HumLookAndFeel& l) : lnf (l) {}
    std::function<juce::File()> createFile;
    bool recording = false;
    int notes = 0;
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }
    bool ready() const { return ! recording && notes > 0; }

private:
    ui::HumLookAndFeel& lnf;
    bool dragging = false;
};

class HumAudioProcessorEditor : public juce::AudioProcessorEditor,
                                private juce::Timer
{
public:
    explicit HumAudioProcessorEditor (HumAudioProcessor&);
    ~HumAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setupKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& paramId, bool bipolar);
    void setupCombo (juce::ComboBox&, juce::Label&, const juce::String& name, const juce::String& paramId);
    void placeKnob (juce::Slider&, juce::Label&, juce::Rectangle<int> area);

    HumAudioProcessor& proc;
    ui::HumLookAndFeel lnf;

    juce::Rectangle<int> screenArea, controlsArea;
    std::vector<int> dividers;  // x de los separadores entre grupos de controles

    PitchRoll roll { lnf };
    Readout readout { lnf };
    RecButton recButton { lnf };
    TakeChip takeChip { lnf };
    juce::ToggleButton quantizeToggle, autoTuneToggle, synthToggle;
    juce::Label tuneLabel;

    juce::Slider gateKnob, octaveKnob, volKnob;
    juce::Label gateLabel, octaveLabel, volLabel, rootLabel, scaleLabel;
    juce::ComboBox rootBox, scaleBox;

    // Las "attachments" enlazan cada control con su parámetro: mueves el control y cambia el
    // parámetro; FL automatiza el parámetro y se mueve el control.
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAtt> gateAtt, octaveAtt, volAtt;
    std::unique_ptr<ComboAtt>  rootAtt, scaleAtt;
    std::unique_ptr<ButtonAtt> synthAtt, autoTuneAtt;

    std::vector<HumAudioProcessor::PitchFrame> scratch = std::vector<HumAudioProcessor::PitchFrame> (2048);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumAudioProcessorEditor)
};
