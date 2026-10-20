#include "PluginEditor.h"

namespace
{
    const juce::Colour kBg      { 0xff121316 };
    const juce::Colour kPanel   { 0xff1c1e23 };
    const juce::Colour kText    { 0xffe8e8ea };
    const juce::Colour kDim     { 0xff7d8088 };
    const juce::Colour kAccent  { 0xffffb547 };
    const juce::Colour kRec     { 0xffff4d5e };

    juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

    juce::String noteName (int n)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[n % 12]) + juce::String (n / 12);  // como FL: Do central (60) = C5
    }
}

// ---------------------------------------------------------------- LevelMeter
void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (r, 3.0f);

    const auto toX = [&] (float db) { return r.getX() + r.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f); };
    const bool open = levelDb >= gateDb;

    g.setColour (open ? kAccent : kDim);
    g.fillRoundedRectangle (r.withRight (toX (levelDb)), 3.0f);

    g.setColour (kText);
    g.fillRect (juce::Rectangle<float> (toX (gateDb) - 1.0f, r.getY() - 2.0f, 2.0f, r.getHeight() + 4.0f));
}

// ---------------------------------------------------------------- MidiDragArea
void MidiDragArea::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (active ? kAccent.withAlpha (0.12f) : kPanel);
    g.fillRoundedRectangle (r, 6.0f);

    juce::Path border;
    border.addRoundedRectangle (r, 6.0f);
    const float dashes[] = { 5.0f, 4.0f };
    juce::PathStrokeType (1.2f).createDashedStroke (border, border, dashes, 2);
    g.setColour (active ? kAccent : kDim);
    g.fillPath (border);

    g.setFont (juce::FontOptions (14.0f));
    g.drawFittedText (text, getLocalBounds().reduced (8), juce::Justification::centred, 2);
}

void MidiDragArea::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || ! active || e.getDistanceFromDragStart() < 4 || ! createFile)
        return;

    const auto file = createFile();
    if (! file.existsAsFile())
        return;

    // Arrastre "externo": el sistema operativo lleva el archivo fuera del plugin y FL lo
    // recibe igual que si lo arrastrases desde el explorador de Windows.
    dragging = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                [this] { dragging = false; });
}

// ---------------------------------------------------------------- Editor
HumAudioProcessorEditor::HumAudioProcessorEditor (HumAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    lnf.setColour (juce::ResizableWindow::backgroundColourId, kBg);
    lnf.setColour (juce::Slider::thumbColourId, kAccent);
    lnf.setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.6f));
    lnf.setColour (juce::Slider::backgroundColourId, kPanel);
    lnf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::ComboBox::backgroundColourId, kPanel);
    lnf.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::ToggleButton::tickColourId, kAccent);
    lnf.setColour (juce::TextButton::buttonColourId, kPanel);
    setLookAndFeel (&lnf);

    titleLabel.setText ("hum2midi", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (titleLabel);

    noteLabel.setFont (juce::FontOptions (72.0f, juce::Font::bold));
    noteLabel.setJustificationType (juce::Justification::centred);
    noteLabel.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (noteLabel);
    addAndMakeVisible (meter);

    setupSlider (gateSlider,   gateLabel,   "Puerta");
    setupSlider (octaveSlider, octaveLabel, "Octava");
    setupSlider (volSlider,    volLabel,    "Volumen");
    setupCombo  (rootBox,  rootLabel,  u8 ("Tónica"), "root");
    setupCombo  (scaleBox, scaleLabel, "Escala", "scale");

    gateAtt   = std::make_unique<SliderAtt> (proc.apvts, "gate", gateSlider);
    octaveAtt = std::make_unique<SliderAtt> (proc.apvts, "octave", octaveSlider);
    volAtt    = std::make_unique<SliderAtt> (proc.apvts, "synthVol", volSlider);
    rootAtt   = std::make_unique<ComboAtt>  (proc.apvts, "root", rootBox);
    scaleAtt  = std::make_unique<ComboAtt>  (proc.apvts, "scale", scaleBox);

    synthToggle.setButtonText (u8 ("Oír synth"));
    synthAtt = std::make_unique<ButtonAtt> (proc.apvts, "synth", synthToggle);
    addAndMakeVisible (synthToggle);

    quantizeToggle.setButtonText ("Cuantizar a 1/16");
    quantizeToggle.setToggleState (true, juce::dontSendNotification);
    addAndMakeVisible (quantizeToggle);

    recButton.onClick = [this]
    {
        if (proc.isRecording()) proc.stopRecording();
        else                    proc.startRecording();
    };
    addAndMakeVisible (recButton);

    dragArea.createFile = [this] { return proc.writeTakeToMidiFile (quantizeToggle.getToggleState()); };
    addAndMakeVisible (dragArea);

    setSize (460, 400);
    timerCallback();
    startTimerHz (30);
}

HumAudioProcessorEditor::~HumAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void HumAudioProcessorEditor::setupSlider (juce::Slider& s, juce::Label& l, const juce::String& name)
{
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    s.setColour (juce::Slider::textBoxBackgroundColourId, kPanel);
    addAndMakeVisible (s);
    l.setText (name, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (l);
}

void HumAudioProcessorEditor::setupCombo (juce::ComboBox& c, juce::Label& l, const juce::String& name, const juce::String& paramId)
{
    // Los items tienen que existir ANTES de crear la attachment.
    if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (paramId)))
        c.addItemList (param->choices, 1);
    addAndMakeVisible (c);
    l.setText (name, juce::dontSendNotification);
    l.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (l);
}

void HumAudioProcessorEditor::timerCallback()
{
    const int note = proc.currentNote.load();
    noteLabel.setText (note >= 0 ? noteName (note) : u8 ("—"), juce::dontSendNotification);
    noteLabel.setColour (juce::Label::textColourId, note >= 0 ? kAccent : kDim);

    meter.levelDb = proc.inputLevelDb.load();
    meter.gateDb  = (float) gateSlider.getValue();
    meter.repaint();

    const bool recording = proc.isRecording();
    const int count = proc.getRecordedNoteCount();
    recButton.setButtonText (recording ? u8 ("■  Parar") : u8 ("●  Grabar"));
    recButton.setColour (juce::TextButton::buttonColourId, recording ? kRec : kPanel);

    juce::String text;
    if (recording)       text = u8 ("Grabando… ") + juce::String (count) + " notas";
    else if (count > 0)  text = u8 ("Arrastra el MIDI al piano roll  ⇢\n") + juce::String (count) + " notas";
    else                 text = u8 ("Graba algo y arrástralo desde aquí");
    dragArea.active = ! recording && count > 0;
    if (text != dragArea.text) { dragArea.text = text; dragArea.repaint(); }
}

void HumAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
}

void HumAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (16);

    titleLabel.setBounds (r.removeFromTop (20));
    noteLabel.setBounds (r.removeFromTop (84));
    meter.setBounds (r.removeFromTop (8));
    r.removeFromTop (16);

    const auto row = [&] (juce::Label& l, juce::Component& c)
    {
        auto line = r.removeFromTop (26);
        l.setBounds (line.removeFromLeft (72));
        c.setBounds (line);
        r.removeFromTop (6);
    };
    row (gateLabel, gateSlider);
    row (octaveLabel, octaveSlider);

    auto combos = r.removeFromTop (26);
    rootLabel.setBounds (combos.removeFromLeft (72));
    rootBox.setBounds (combos.removeFromLeft (70));
    combos.removeFromLeft (16);
    scaleLabel.setBounds (combos.removeFromLeft (56));
    scaleBox.setBounds (combos);
    r.removeFromTop (6);

    auto synthRow = r.removeFromTop (26);
    synthToggle.setBounds (synthRow.removeFromLeft (110));
    volLabel.setBounds (synthRow.removeFromLeft (64));
    volSlider.setBounds (synthRow);
    r.removeFromTop (14);

    auto bottom = r;
    auto left = bottom.removeFromLeft (140);
    recButton.setBounds (left.removeFromTop (36));
    left.removeFromTop (6);
    quantizeToggle.setBounds (left.removeFromTop (24));
    bottom.removeFromLeft (12);
    dragArea.setBounds (bottom);
}
