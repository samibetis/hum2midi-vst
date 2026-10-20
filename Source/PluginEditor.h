#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Medidor de entrada con la línea de la puerta: si la barra no pasa la línea, no detecta.
class LevelMeter : public juce::Component
{
public:
    float levelDb = -100.0f, gateDb = -40.0f;
    void paint (juce::Graphics&) override;
};

// Zona de la que arrastras la toma grabada (.mid) directamente a FL Studio.
class MidiDragArea : public juce::Component
{
public:
    std::function<juce::File()> createFile;
    juce::String text;
    bool active = false;
    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

private:
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
    void timerCallback() override;  // refresca nota, medidor y grabación ~30 veces/s
    void setupSlider (juce::Slider&, juce::Label&, const juce::String& name);
    void setupCombo (juce::ComboBox&, juce::Label&, const juce::String& name, const juce::String& paramId);

    HumAudioProcessor& proc;
    juce::LookAndFeel_V4 lnf { juce::LookAndFeel_V4::getMidnightColourScheme() };

    juce::Label noteLabel, titleLabel;
    LevelMeter meter;
    juce::Slider gateSlider, octaveSlider, volSlider;
    juce::Label gateLabel, octaveLabel, volLabel, rootLabel, scaleLabel;
    juce::ComboBox rootBox, scaleBox;
    juce::ToggleButton synthToggle { "Oir synth" }, quantizeToggle, autoTuneToggle;
    juce::Label tuneLabel;
    juce::TextButton recButton;
    MidiDragArea dragArea;

    // Las "attachments" enlazan cada control con su parámetro: mueves el slider y cambia
    // el parámetro; FL automatiza el parámetro y se mueve el slider.
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SliderAtt> gateAtt, octaveAtt, volAtt;
    std::unique_ptr<ComboAtt>  rootAtt, scaleAtt;
    std::unique_ptr<ButtonAtt> synthAtt, autoTuneAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumAudioProcessorEditor)
};
