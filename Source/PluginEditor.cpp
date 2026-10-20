#include "PluginEditor.h"

using namespace ui;

namespace
{
    juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

    const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    constexpr bool kBlack[] = { false, true, false, true, false, false, true, false, true, false, true, false };

    int pc (int n) { return ((n % 12) + 12) % 12; }
    juce::String noteName (int n) { return juce::String (kNames[pc (n)]) + juce::String (n / 12); }  // como FL: C5 = 60

    float textWidth (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }
}

// ================================================================ PitchRoll
void PitchRoll::append (const HumAudioProcessor::PitchFrame* f, int n)
{
    for (int i = 0; i < n; ++i)
    {
        frames[(size_t) write] = f[i];
        write = (write + 1) % kFrames;
        count = juce::jmin (count + 1, kFrames);
        anyVoice = anyVoice || f[i].voiced;
        ++total;
    }
}

void PitchRoll::tick()
{
    // La vista sigue a la nota que suena (o a tu voz si aún no hay nota), con suavidad.
    for (int age = 0; age < juce::jmin (count, 60); ++age)
    {
        const auto& f = at (age);
        if (f.note >= 0) { target = (float) f.note; break; }
        if (f.voiced)    { target = f.midi; break; }
    }
    centre += (target - centre) * 0.12f;
}

void PitchRoll::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    juce::Path clip;
    clip.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), 7.0f, 7.0f,
                              false, true, false, true);
    g.reduceClipRegion (clip);

    auto keys = bounds.removeFromLeft (40.0f);
    const auto grid = bounds;
    const float rowH = grid.getHeight() / kSpan;
    const float top = centre + kSpan * 0.5f;
    const auto yOf = [&] (float m) { return grid.getY() + (top - m) * rowH; };
    const int lo = (int) std::floor (centre - kSpan * 0.5f) - 1;
    const int hi = (int) std::ceil (centre + kSpan * 0.5f) + 1;
    const int current = count > 0 ? at (0).note : -1;

    // ---- Filas del piano roll
    for (int n = lo; n <= hi; ++n)
    {
        const float y0 = yOf ((float) n + 0.5f), y1 = yOf ((float) n - 0.5f);
        g.setColour (kBlack[pc (n)] ? juce::Colour (0xff181b22) : col::panel);
        g.fillRect (grid.getX(), y0, grid.getWidth(), y1 - y0);
        if (pc (n) == 0)      { g.setColour (col::line);                    g.fillRect (grid.getX(), y1 - 1.0f, grid.getWidth(), 1.0f); }
        else if (pc (n) == 5) { g.setColour (col::line.withAlpha (0.45f)); g.fillRect (grid.getX(), y1 - 1.0f, grid.getWidth(), 1.0f); }
    }

    // ---- Marcas de tiempo (una por segundo) que se desplazan con la historia
    const float px = grid.getWidth() / (float) (kFrames - 1);
    const auto xOf = [&] (int age) { return grid.getRight() - (float) age * px; };
    g.setColour (col::line.withAlpha (0.5f));
    for (int age = (int) (total % 125); age < kFrames; age += 125)
        g.fillRect (xOf (age), grid.getY(), 1.0f, grid.getHeight());

    // ---- Teclado
    const juce::Colour ivory (0xffb8bfcb), ebony (0xff252a33);
    for (int n = lo; n <= hi; ++n)
    {
        const float y0 = yOf ((float) n + 0.5f), y1 = yOf ((float) n - 0.5f);
        const bool active = n == current;
        if (! kBlack[pc (n)])
        {
            g.setColour (active ? col::note : ivory);
            g.fillRect (keys.getX(), y0, keys.getWidth(), y1 - y0);
            if (pc (n) == 0 || pc (n) == 5)
            {
                g.setColour (juce::Colour (0xff7d8594));
                g.fillRect (keys.getX(), y1 - 1.0f, keys.getWidth(), 1.0f);
            }
            if (pc (n) == 0 && rowH >= 11.0f)
            {
                g.setColour (col::bg.withAlpha (0.75f));
                g.setFont (lnf.strong (10.5f));
                g.drawText (noteName (n), juce::Rectangle<float> (keys.getX(), y0, keys.getWidth() - 4.0f, y1 - y0),
                            juce::Justification::centredRight);
            }
        }
        else
        {
            g.setColour (ivory);
            g.fillRect (keys.getX(), y0, keys.getWidth(), y1 - y0);
            g.setColour (active ? col::noteEdge : ebony);
            g.fillRoundedRectangle (keys.getX() - 3.0f, y0 + 0.5f, keys.getWidth() * 0.62f + 3.0f, y1 - y0 - 1.0f, 2.0f);
        }
    }
    g.setColour (col::line);
    g.fillRect (keys.getRight(), grid.getY(), 1.0f, grid.getHeight());

    // ---- Tramos grabados: línea roja abajo
    for (int age = 0; age < count;)
    {
        if (! at (age).recording) { ++age; continue; }
        int end = age;
        while (end + 1 < count && at (end + 1).recording) ++end;
        g.setColour (col::rec);
        g.fillRect (xOf (end), grid.getBottom() - 3.0f, xOf (age) - xOf (end) + 1.0f, 3.0f);
        age = end + 1;
    }

    // ---- Notas detectadas (bloques)
    for (int age = 0; age < count;)
    {
        const int note = at (age).note;
        if (note < 0) { ++age; continue; }
        int end = age;
        while (end + 1 < count && at (end + 1).note == note) ++end;

        const float x0 = xOf (end), x1 = xOf (age);
        const juce::Rectangle<float> r (x0, yOf ((float) note + 0.5f) + 1.5f, juce::jmax (3.0f, x1 - x0), rowH - 3.0f);
        g.setColour (col::note);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (col::noteEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        age = end + 1;
    }

    // ---- Tu voz (línea continua; se corta en los silencios)
    juce::Path trace;
    bool pen = false;
    for (int age = count - 1; age >= 0; --age)
    {
        const auto& f = at (age);
        if (! f.voiced) { pen = false; continue; }
        const float x = xOf (age), y = juce::jlimit (grid.getY(), grid.getBottom(), yOf (f.midi));
        if (pen) trace.lineTo (x, y);
        else     { trace.startNewSubPath (x, y); pen = true; }
    }
    g.setColour (col::trace.withAlpha (0.12f));
    g.strokePath (trace, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (col::trace.withAlpha (0.92f));
    g.strokePath (trace, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (count > 0 && at (0).voiced)
    {
        const float y = juce::jlimit (grid.getY(), grid.getBottom(), yOf (at (0).midi));
        g.setColour (col::trace);
        g.fillEllipse (grid.getRight() - 4.0f, y - 3.5f, 7.0f, 7.0f);
    }

    if (! anyVoice)
    {
        g.setColour (col::dim);
        g.setFont (lnf.font (15.0f));
        g.drawText (u8 ("Tararea o canta y tus notas aparecerán aquí"), grid, juce::Justification::centred);
    }
}

// ================================================================ Readout
void Readout::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (20.0f, 16.0f);

    // ---- Nota: letra grande, sostenido y octava pequeños a la derecha
    auto noteArea = r.removeFromTop (96.0f);
    const auto big = lnf.readout (92.0f), small = lnf.readout (34.0f);
    if (note >= 0)
    {
        const juce::String name = kNames[pc (note)];
        const juce::String letter = name.substring (0, 1);
        const float lw = textWidth (big, letter);
        g.setColour (col::text);
        g.setFont (big);
        g.drawText (letter, noteArea.withWidth (lw + 2.0f), juce::Justification::centredLeft);
        const float sx = noteArea.getX() + lw + 4.0f;
        g.setFont (small);
        if (name.length() > 1)
            g.drawText ("#", juce::Rectangle<float> (sx, noteArea.getY() + 12.0f, 40.0f, 34.0f), juce::Justification::topLeft);
        g.setColour (col::dim);
        g.drawText (juce::String (note / 12), juce::Rectangle<float> (sx, noteArea.getBottom() - 46.0f, 40.0f, 34.0f),
                    juce::Justification::bottomLeft);
    }
    else
    {
        g.setColour (col::faint);
        g.setFont (big);
        g.drawText (u8 ("–"), noteArea, juce::Justification::centredLeft);
    }

    // ---- Afinador: ±50 cents respecto a la nota detectada
    r.removeFromTop (10.0f);
    auto tuner = r.removeFromTop (40.0f);
    const float ty = tuner.getY() + 8.0f;
    g.setColour (col::line);
    g.fillRect (tuner.getX(), ty - 0.5f, tuner.getWidth(), 1.0f);
    for (int c = -50; c <= 50; c += 25)
    {
        const float x = juce::jmap ((float) c, -50.0f, 50.0f, tuner.getX(), tuner.getRight());
        g.setColour (c == 0 ? col::dim : col::faint);
        const float th = c == 0 ? 7.0f : 4.0f;
        g.fillRect (x - 0.5f, ty - th, 1.0f, th * 2.0f);
    }
    if (hasCents)
    {
        const float c = juce::jlimit (-50.0f, 50.0f, cents);
        const float x = juce::jmap (c, -50.0f, 50.0f, tuner.getX(), tuner.getRight());
        g.setColour (std::abs (c) < 10.0f ? col::note : col::text);
        g.fillRoundedRectangle (x - 1.5f, ty - 8.0f, 3.0f, 16.0f, 1.5f);

        const int ci = juce::roundToInt (cents);
        const juce::String txt = (ci > 0 ? "+" : (ci < 0 ? u8 ("−") : "")) + juce::String (std::abs (ci)) + u8 (" ¢");
        g.setColour (col::dim);
        g.setFont (lnf.font (13.5f));
        g.drawText (txt, tuner.withTop (ty + 10.0f), juce::Justification::centredLeft);
    }

    // ---- Entrada: barra segmentada; la marca es la puerta (por debajo no se analiza)
    auto meter = r.removeFromBottom (30.0f);
    g.setColour (col::dim);
    g.setFont (lnf.font (13.0f));
    g.drawText ("Entrada", meter.removeFromTop (16.0f), juce::Justification::centredLeft);
    const auto bar = meter.withHeight (6.0f).withY (meter.getY() + 4.0f);
    constexpr int segs = 26;
    constexpr float minDb = -80.0f;
    const float sw = (bar.getWidth() - (segs - 1) * 2.0f) / segs;
    for (int i = 0; i < segs; ++i)
    {
        const float segDb = minDb + (i + 0.5f) * (-minDb / segs);
        const bool lit = levelDb >= segDb;
        g.setColour (! lit ? col::line : (segDb >= gateDb ? col::text : col::faint));
        g.fillRoundedRectangle (bar.getX() + i * (sw + 2.0f), bar.getY(), sw, bar.getHeight(), 1.0f);
    }
    const float gx = juce::jmap (gateDb, minDb, 0.0f, bar.getX(), bar.getRight());
    g.setColour (col::note);
    g.fillRect (gx - 1.0f, bar.getY() - 4.0f, 2.0f, bar.getHeight() + 8.0f);
}

// ================================================================ RecButton
void RecButton::paintButton (juce::Graphics& g, bool over, bool)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const float rad = r.getHeight() * 0.5f;
    if (recording)
    {
        g.setColour (col::rec);
        g.fillRoundedRectangle (r, rad);
    }
    else
    {
        g.setColour (over ? col::raised.brighter (0.06f) : col::raised);
        g.fillRoundedRectangle (r, rad);
        g.setColour (col::line);
        g.drawRoundedRectangle (r, rad, 1.0f);
    }

    const float cy = r.getCentreY(), ix = r.getX() + 18.0f;
    if (recording)
    {
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (ix - 4.5f, cy - 4.5f, 9.0f, 9.0f, 1.5f);
    }
    else
    {
        g.setColour (col::rec);
        g.fillEllipse (ix - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    }
    g.setColour (recording ? juce::Colours::white : col::text);
    g.setFont (lnf.strong (15.0f));
    g.drawText (recording ? "Parar" : "Grabar", r.withTrimmedLeft (32.0f).withTrimmedRight (12.0f), juce::Justification::centredLeft);
}

// ================================================================ TakeChip
void TakeChip::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (col::raised);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (ready() && isMouseOver() ? col::faint : col::line);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    auto inner = getLocalBounds().toFloat().reduced (12.0f, 0.0f);
    g.setFont (lnf.font (14.5f));
    if (recording)
    {
        g.setColour (col::rec);
        g.fillEllipse (inner.getX(), inner.getCentreY() - 3.5f, 7.0f, 7.0f);
        g.setColour (col::text);
        g.drawText (juce::String (notes) + (notes == 1 ? " nota grabada" : " notas grabadas"),
                    inner.withTrimmedLeft (16.0f), juce::Justification::centredLeft);
    }
    else if (ready())
    {
        g.setColour (col::dim);  // asa de arrastre: 2 columnas x 3 puntos
        for (int cx = 0; cx < 2; ++cx)
            for (int cy = -1; cy <= 1; ++cy)
                g.fillEllipse (inner.getX() + cx * 5.0f, inner.getCentreY() + cy * 5.0f - 1.25f, 2.5f, 2.5f);
        g.setColour (col::text);
        g.drawText ("Arrastrar toma", inner.withTrimmedLeft (18.0f), juce::Justification::centredLeft);
        g.setColour (col::dim);
        g.drawText (juce::String (notes) + (notes == 1 ? " nota" : " notas"), inner, juce::Justification::centredRight);
    }
    else
    {
        g.setColour (col::faint);
        g.drawText ("Graba una toma para arrastrarla", inner, juce::Justification::centredLeft);
    }
}

void TakeChip::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || ! ready() || e.getDistanceFromDragStart() < 4 || ! createFile)
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

// ================================================================ Editor
HumAudioProcessorEditor::HumAudioProcessorEditor (HumAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (roll);
    addAndMakeVisible (readout);

    recButton.onClick = [this]
    {
        if (proc.isRecording()) proc.stopRecording();
        else                    proc.startRecording();
    };
    addAndMakeVisible (recButton);

    takeChip.createFile = [this] { return proc.writeTakeToMidiFile (quantizeToggle.getToggleState()); };
    addAndMakeVisible (takeChip);

    quantizeToggle.setButtonText ("Cuantizar 1/16");
    quantizeToggle.setToggleState (true, juce::dontSendNotification);
    addAndMakeVisible (quantizeToggle);

    setupKnob (gateKnob, gateLabel, "Puerta", "gate", false);
    setupKnob (octaveKnob, octaveLabel, "Octava", "octave", true);
    setupKnob (volKnob, volLabel, "Volumen", "synthVol", false);
    setupCombo (rootBox, rootLabel, u8 ("Tónica"), "root");
    setupCombo (scaleBox, scaleLabel, "Escala", "scale");

    autoTuneToggle.setButtonText (u8 ("Afinación auto"));
    autoTuneAtt = std::make_unique<ButtonAtt> (proc.apvts, "autotune", autoTuneToggle);
    addAndMakeVisible (autoTuneToggle);
    tuneLabel.setColour (juce::Label::textColourId, col::dim);
    tuneLabel.setFont (lnf.font (14.0f));
    addAndMakeVisible (tuneLabel);

    synthToggle.setButtonText (u8 ("Oír synth"));
    synthAtt = std::make_unique<ButtonAtt> (proc.apvts, "synth", synthToggle);
    addAndMakeVisible (synthToggle);

    setSize (800, 464);
    timerCallback();
    startTimerHz (60);
}

HumAudioProcessorEditor::~HumAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void HumAudioProcessorEditor::setupKnob (juce::Slider& s, juce::Label& l, const juce::String& name,
                                         const juce::String& paramId, bool bipolar)
{
    addAndMakeVisible (s);  // primero dentro del editor, para que la caja de valor use nuestro LookAndFeel
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 20);
    s.getProperties().set ("bipolar", bipolar);

    auto att = std::make_unique<SliderAtt> (proc.apvts, paramId, s);
    if (auto* param = proc.apvts.getParameter (paramId))  // doble clic = valor por defecto
        s.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
    if (paramId == "gate") gateAtt = std::move (att);
    else if (paramId == "octave") octaveAtt = std::move (att);
    else volAtt = std::move (att);

    l.setText (name, juce::dontSendNotification);
    l.setFont (lnf.font (13.5f));
    l.setColour (juce::Label::textColourId, col::dim);
    l.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (l);
}

void HumAudioProcessorEditor::setupCombo (juce::ComboBox& c, juce::Label& l, const juce::String& name, const juce::String& paramId)
{
    // Los items tienen que existir ANTES de crear la attachment.
    if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (paramId)))
        c.addItemList (param->choices, 1);
    addAndMakeVisible (c);
    if (paramId == "root") rootAtt = std::make_unique<ComboAtt> (proc.apvts, paramId, c);
    else                   scaleAtt = std::make_unique<ComboAtt> (proc.apvts, paramId, c);

    l.setText (name, juce::dontSendNotification);
    l.setFont (lnf.font (13.5f));
    l.setColour (juce::Label::textColourId, col::dim);
    l.setBorderSize ({});
    addAndMakeVisible (l);
}

void HumAudioProcessorEditor::timerCallback()
{
    // Pitch roll: lo que el hilo de audio ha analizado desde la última vez.
    const int n = proc.popPitchFrames (scratch.data(), (int) scratch.size());
    roll.append (scratch.data(), n);
    roll.tick();
    roll.repaint();

    if (n > 0)
    {
        const auto& f = scratch[(size_t) n - 1];
        readout.note = f.note;
        const bool has = f.voiced && f.note >= 0;
        const float c = has ? (f.midi - (float) f.note) * 100.0f : 0.0f;
        readout.cents = readout.hasCents && has ? readout.cents + (c - readout.cents) * 0.35f : c;
        readout.hasCents = has;
    }
    readout.levelDb = proc.inputLevelDb.load();
    readout.gateDb  = (float) gateKnob.getValue();
    readout.repaint();

    // Afinación automática: cuánto te compensa.
    const int cents = juce::roundToInt (proc.tuneCents.load());
    juce::String tuneText;
    if (autoTuneToggle.getToggleState())
        tuneText = std::abs (cents) < 5 ? u8 ("cantas afinado")
                                        : u8 ("cantas ") + juce::String (std::abs (cents)) + u8 (" ¢ ") + (cents < 0 ? "bajo" : "alto");
    tuneLabel.setText (tuneText, juce::dontSendNotification);

    // Grabación
    const bool recording = proc.isRecording();
    const int notes = proc.getRecordedNoteCount();
    if (recButton.recording != recording) { recButton.recording = recording; recButton.repaint(); }
    if (takeChip.recording != recording || takeChip.notes != notes)
    {
        takeChip.recording = recording;
        takeChip.notes = notes;
        takeChip.setMouseCursor (takeChip.ready() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        takeChip.repaint();
    }
}

void HumAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    // ---- Marca: una onda que se convierte en nota
    const float mx = 20.0f, my = 28.0f;
    juce::Path wave;  // la onda se va calmando hasta quedarse en la altura de la nota
    for (int i = 0; i <= 32; ++i)
    {
        const float t = (float) i / 32.0f;
        const float x = mx + t * 14.0f, y = my + 5.0f * (1.0f - t) * std::sin (t * 13.0f);
        if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
    }
    g.setColour (col::trace);
    g.strokePath (wave, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (col::note);
    g.fillRoundedRectangle (mx + 15.0f, my - 3.0f, 10.0f, 6.0f, 2.0f);

    g.setColour (col::text);
    g.setFont (lnf.strong (20.0f));
    g.drawText ("hum2midi", juce::Rectangle<float> (mx + 30.0f, 12.0f, 140.0f, 32.0f), juce::Justification::centredLeft);

    // ---- Pantalla
    const auto s = screenArea.toFloat();
    g.setColour (col::panel);
    g.fillRoundedRectangle (s, 8.0f);
    g.setColour (col::line);
    g.drawRoundedRectangle (s.reduced (0.5f), 8.0f, 1.0f);
    g.fillRect ((float) readout.getRight(), s.getY() + 1.0f, 1.0f, s.getHeight() - 2.0f);

    // ---- Separadores entre grupos de controles
    for (int x : dividers)
        g.fillRect ((float) x, (float) controlsArea.getY() + 14.0f, 1.0f, (float) controlsArea.getHeight() - 28.0f);
}

void HumAudioProcessorEditor::placeKnob (juce::Slider& s, juce::Label& l, juce::Rectangle<int> area)
{
    l.setBounds (area.getX(), area.getY() + 10, area.getWidth(), 18);
    s.setBounds (area.getX(), area.getY() + 30, area.getWidth(), 86);
}

void HumAudioProcessorEditor::resized()
{
    const int W = getWidth();

    // Cabecera
    recButton.setBounds (W - 20 - 108, 12, 108, 32);
    takeChip.setBounds (recButton.getX() - 10 - 230, 12, 230, 32);
    quantizeToggle.setBounds (takeChip.getX() - 20 - 146, 12, 146, 32);

    // Pantalla: lectura | pitch roll
    screenArea = { 16, 56, W - 32, 256 };
    auto s = screenArea.reduced (1);
    readout.setBounds (s.removeFromLeft (184));
    s.removeFromLeft (1);
    roll.setBounds (s);

    // Controles: detección | tonalidad | monitor
    controlsArea = { 16, 322, W - 32, getHeight() - 322 - 8 };
    dividers.clear();
    auto c = controlsArea;

    auto detect = c.removeFromLeft (190);
    placeKnob (gateKnob, gateLabel, detect.removeFromLeft (95));
    placeKnob (octaveKnob, octaveLabel, detect);
    dividers.push_back (c.getX() + 16);
    c.removeFromLeft (32);

    auto key = c.removeFromLeft (300);
    rootLabel.setBounds (key.getX(), key.getY() + 10, 92, 18);
    rootBox.setBounds (key.getX(), key.getY() + 32, 92, 34);
    scaleLabel.setBounds (key.getX() + 104, key.getY() + 10, 180, 18);
    scaleBox.setBounds (key.getX() + 104, key.getY() + 32, 180, 34);
    autoTuneToggle.setBounds (key.getX(), key.getY() + 84, 146, 28);
    tuneLabel.setBounds (key.getX() + 146, key.getY() + 84, 150, 28);
    dividers.push_back (c.getX() + 16);
    c.removeFromLeft (32);

    synthToggle.setBounds (c.getX(), c.getY() + 58, 112, 28);
    placeKnob (volKnob, volLabel, c.withTrimmedLeft (116));
}
