#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "HumLookAndFeel.h"

// ---- Pitch roll: tu voz (línea) y las notas detectadas (bloques) sobre un mini piano roll.
//      Con FL en marcha dibuja los tiempos y compases.
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

// ---- Botón de grabar: Grabar / Esperando Play / Parar
class RecButton : public juce::Button
{
public:
    explicit RecButton (ui::HumLookAndFeel& l) : juce::Button ("rec"), lnf (l) {}
    enum Mode { Ready, Armed, Recording };
    Mode mode = Ready;
    int liveNotes = 0;
    float blink = 0.0f;  // 0..1, para el parpadeo del modo armado
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    ui::HumLookAndFeel& lnf;
};

// ---- Una toma en la lista: escuchar, ver, arrastrar al piano roll, borrar.
class TakeCard : public juce::Component
{
public:
    TakeCard (ui::HumLookAndFeel& l, HumAudioProcessor& p, int takeIndex);
    void refresh();  // vuelve a pedir los datos (p. ej. si cambia la cuantización)
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    int index;
    std::function<void()> onChanged;  // la lista se refresca (selección, borrado)

private:
    juce::Rectangle<float> playArea() const;
    juce::Rectangle<float> deleteArea() const;

    ui::HumLookAndFeel& lnf;
    HumAudioProcessor& proc;
    HumAudioProcessor::TakeInfo info;
    bool hover = false, overPlay = false, overDelete = false, dragging = false, dragStarted = false;
};

// ---- Lista de tomas (la más nueva arriba)
class TakesList : public juce::Component
{
public:
    TakesList (ui::HumLookAndFeel& l, HumAudioProcessor& p) : lnf (l), proc (p) {}
    void rebuild (int viewportWidth);
    void refreshAll();
    void repaintPlaying();
    void paint (juce::Graphics&) override;
    std::function<void()> onChanged;

private:
    ui::HumLookAndFeel& lnf;
    HumAudioProcessor& proc;
    std::vector<std::unique_ptr<TakeCard>> cards;
};

class HumAudioProcessorEditor : public juce::AudioProcessorEditor,
                                private juce::Timer,
                                private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit HumAudioProcessorEditor (HumAudioProcessor&);
    ~HumAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void parameterChanged (const juce::String&, float) override { exportSettingsChanged = true; }
    void setupKnob (juce::Slider&, juce::Label&, const juce::String& name, const juce::String& paramId, bool bipolar);
    void setupCombo (juce::ComboBox&, juce::Label&, const juce::String& name, const juce::String& paramId);
    void setupToggle (juce::ToggleButton&, const juce::String& text, const juce::String& paramId);
    void placeKnob (juce::Slider&, juce::Label&, juce::Rectangle<int> area);
    void startCalibration();
    void detectKey();
    void saveSelectedTake();
    void setParam (const juce::String& id, float value);

    HumAudioProcessor& proc;
    ui::HumLookAndFeel lnf;

    juce::Rectangle<int> screenArea, controlsArea, takesArea;
    std::vector<int> dividers;  // x de los separadores entre grupos de controles

    PitchRoll roll { lnf };
    Readout readout { lnf };
    RecButton recButton { lnf };
    juce::ToggleButton syncToggle;

    // Detección
    juce::Slider gateKnob, detailKnob, octaveKnob;
    juce::Label gateLabel, detailLabel, octaveLabel;
    juce::TextButton calibrateButton;

    // Tonalidad
    juce::ComboBox rootBox, scaleBox;
    juce::Label rootLabel, scaleLabel, keyResult, tuneLabel;
    juce::TextButton detectButton;
    juce::ToggleButton autoTuneToggle;

    // Armonía y expresión
    juce::ComboBox chordsBox, bendRangeBox;
    juce::Label chordsLabel, bendRangeLabel;
    juce::ToggleButton bendToggle, velFixedToggle;

    // Monitor
    juce::ToggleButton synthToggle;
    juce::Slider volKnob, voiceKnob;
    juce::Label volLabel, voiceLabel;

    // Tomas y exportación
    TakesList takesList { lnf, proc };
    juce::Viewport takesViewport;
    juce::ComboBox gridBox;
    juce::Label gridLabel, strengthLabel;
    juce::Slider strengthKnob;
    juce::ToggleButton legatoToggle;
    juce::TextButton saveButton;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::TooltipWindow tooltips { this, 700 };  // explica los controles al pasar el ratón

    // Las "attachments" enlazan cada control con su parámetro: mueves el control y cambia el
    // parámetro; FL automatiza el parámetro y se mueve el control.
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> sliderAtts;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>> comboAtts;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> buttonAtts;

    std::vector<HumAudioProcessor::PitchFrame> scratch = std::vector<HumAudioProcessor::PitchFrame> (2048);
    int seenTakesVersion = -1;
    std::atomic<bool> exportSettingsChanged { false };  // lo marcan los parámetros (puede ser desde el audio)

    // Calibración de la puerta: escucha el silencio de la sala 1.5 s
    bool calibrating = false;
    double calibrationStart = 0.0, calibrationMessageUntil = 0.0;
    std::vector<float> calibrationSamples;
    double keyMessageUntil = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumAudioProcessorEditor)
};
