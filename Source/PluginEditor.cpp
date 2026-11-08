#include "PluginEditor.h"

using namespace ui;

namespace
{
    juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

    const char* kNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    constexpr bool kBlack[] = { false, true, false, true, false, false, true, false, true, false, true, false };

    int pc (int n) { return ((n % 12) + 12) % 12; }
    juce::String noteName (int n) { return juce::String (kNames[pc (n)]) + juce::String (n / 12); }  // como FL: C5 = 60
    const char* kNamesEs[] = { "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" };

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

    // ---- Marcas de tiempo: tiempos y compases de FL si está sonando; si no, una por segundo
    const float px = grid.getWidth() / (float) (kFrames - 1);
    const auto xOf = [&] (int age) { return grid.getRight() - (float) age * px; };
    bool hasBeats = false;
    for (int age = 0; age < count; ++age)
    {
        const auto beat = at (age).beat;
        if (beat == 0) continue;
        hasBeats = true;
        g.setColour (beat == 2 ? col::faint : col::line.withAlpha (0.6f));
        g.fillRect (xOf (age), grid.getY(), beat == 2 ? 1.5f : 1.0f, grid.getHeight());
    }
    if (! hasBeats)
    {
        g.setColour (col::line.withAlpha (0.5f));
        for (int age = (int) (total % 125); age < kFrames; age += 125)
            g.fillRect (xOf (age), grid.getY(), 1.0f, grid.getHeight());
    }

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
    const auto r = getLocalBounds().toFloat().reduced (0.75f);
    const float rad = r.getHeight() * 0.5f;
    if (mode == Recording)
    {
        g.setColour (col::rec);
        g.fillRoundedRectangle (r, rad);
    }
    else
    {
        g.setColour (over ? col::raised.brighter (0.06f) : col::raised);
        g.fillRoundedRectangle (r, rad);
        g.setColour (mode == Armed ? col::rec.withAlpha (0.4f + 0.6f * blink) : col::line);
        g.drawRoundedRectangle (r, rad, mode == Armed ? 1.5f : 1.0f);
    }

    const float cy = r.getCentreY(), ix = r.getX() + 18.0f;
    if (mode == Recording)
    {
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (ix - 4.5f, cy - 4.5f, 9.0f, 9.0f, 1.5f);
    }
    else
    {
        g.setColour (col::rec.withAlpha (mode == Armed ? 0.35f + 0.65f * blink : 1.0f));
        g.fillEllipse (ix - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    }

    juce::String label = "Grabar";
    if (mode == Armed) label = "Esperando Play";
    if (mode == Recording) label = "Parar  " + juce::String (liveNotes);
    g.setColour (mode == Recording ? juce::Colours::white : col::text);
    g.setFont (lnf.strong (15.0f));
    g.drawText (label, r.withTrimmedLeft (32.0f).withTrimmedRight (10.0f), juce::Justification::centredLeft);
}

// ================================================================ TakeCard
TakeCard::TakeCard (HumLookAndFeel& l, HumAudioProcessor& p, int takeIndex)
    : index (takeIndex), lnf (l), proc (p)
{
    refresh();
}

void TakeCard::refresh()
{
    info = proc.getTakeInfo (index);
    repaint();
}

juce::Rectangle<float> TakeCard::playArea() const   { return { 12.0f, (float) getHeight() * 0.5f - 16.0f, 32.0f, 32.0f }; }
juce::Rectangle<float> TakeCard::deleteArea() const { return { (float) getWidth() - 28.0f, 8.0f, 20.0f, 20.0f }; }

void TakeCard::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool selected = proc.selectedTake == index;
    const bool playing = proc.getPreviewTake() == index;

    g.setColour (hover ? col::raised.brighter (0.04f) : col::raised);
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (selected ? col::note : (hover ? col::faint : col::line));
    g.drawRoundedRectangle (r, 7.0f, selected ? 1.5f : 1.0f);

    // Escuchar / parar (con anillo de progreso mientras suena)
    const auto pa = playArea();
    g.setColour (overPlay ? col::line.brighter (0.25f) : col::line);
    g.fillEllipse (pa);
    if (playing)
    {
        juce::Path ring;
        ring.addCentredArc (pa.getCentreX(), pa.getCentreY(), 15.0f, 15.0f, 0.0f, 0.0f,
                            juce::MathConstants<float>::twoPi * juce::jlimit (0.0f, 1.0f, proc.previewProgress.load()), true);
        g.setColour (col::note);
        g.strokePath (ring, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (col::text);
        g.fillRoundedRectangle (pa.withSizeKeepingCentre (9.0f, 9.0f), 1.5f);
    }
    else
    {
        juce::Path tri;
        const auto c = pa.getCentre();
        tri.addTriangle (c.x - 3.5f, c.y - 6.0f, c.x - 3.5f, c.y + 6.0f, c.x + 6.5f, c.y);
        g.setColour (col::text);
        g.fillPath (tri);
    }

    // Nombre y datos
    const float tx = 56.0f;
    g.setColour (col::text);
    g.setFont (lnf.strong (15.0f));
    g.drawText (info.name, juce::Rectangle<float> (tx, 9.0f, (float) getWidth() - tx - 34.0f, 20.0f), juce::Justification::centredLeft);
    const int secs = juce::roundToInt (info.seconds);
    juce::String meta = juce::String (info.notes) + (info.notes == 1 ? " nota, " : " notas, ")
                      + juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2);
    if (info.bar > 0) meta << u8 (", compás ") << info.bar;
    g.setColour (col::dim);
    g.setFont (lnf.font (13.0f));
    g.drawText (meta, juce::Rectangle<float> (tx, 28.0f, (float) getWidth() - tx - 12.0f, 18.0f), juce::Justification::centredLeft);

    // Miniatura: la melodía como en el piano roll
    const auto thumb = juce::Rectangle<float> (tx, 50.0f, (float) getWidth() - tx - 12.0f, (float) getHeight() - 60.0f);
    g.setColour (col::panel);
    g.fillRoundedRectangle (thumb, 3.0f);
    if (! info.preview.empty())
    {
        double t0 = info.preview.front().start, t1 = t0;
        int lo = 127, hi = 0;
        for (const auto& n : info.preview) { t1 = std::max (t1, n.end); lo = std::min (lo, n.note); hi = std::max (hi, n.note); }
        lo -= 1; hi += 1;
        const float rowH = thumb.getHeight() / (float) (hi - lo + 1);
        const auto inner = thumb.reduced (4.0f, 0.0f);
        const auto xOf = [&] (double t) { return inner.getX() + (float) ((t - t0) / std::max (1.0, t1 - t0)) * inner.getWidth(); };
        g.setColour (selected ? col::note : col::note.withAlpha (0.75f));
        for (const auto& n : info.preview)
        {
            const float y = thumb.getBottom() - (float) (n.note - lo + 1) * rowH;
            const float h = juce::jlimit (3.0f, 6.0f, rowH - 1.0f);
            g.fillRoundedRectangle (xOf (n.start), y + (rowH - h) * 0.5f, std::max (3.0f, xOf (n.end) - xOf (n.start) - 1.5f), h, 1.5f);
        }
        if (playing)
        {
            g.setColour (col::trace);
            g.fillRect (inner.getX() + inner.getWidth() * proc.previewProgress.load(), thumb.getY(), 1.5f, thumb.getHeight());
        }
    }

    // Borrar (solo al pasar el ratón)
    if (hover)
    {
        const auto da = deleteArea().reduced (5.0f);
        g.setColour (overDelete ? col::rec : col::dim);
        g.drawLine (da.getX(), da.getY(), da.getRight(), da.getBottom(), 1.6f);
        g.drawLine (da.getRight(), da.getY(), da.getX(), da.getBottom(), 1.6f);
    }
}

void TakeCard::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.position;
    const bool np = playArea().contains (p), nd = deleteArea().contains (p);
    if (! hover || np != overPlay || nd != overDelete)
    {
        hover = true; overPlay = np; overDelete = nd;
        setMouseCursor (np || nd ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::DraggingHandCursor);
        repaint();
    }
}

void TakeCard::mouseExit (const juce::MouseEvent&) { hover = overPlay = overDelete = false; repaint(); }
void TakeCard::mouseDown (const juce::MouseEvent&) { dragStarted = false; }

void TakeCard::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || dragStarted || overPlay || overDelete || e.getDistanceFromDragStart() < 5)
        return;
    const auto file = proc.writeTakeFile (index);
    if (! file.existsAsFile())
        return;

    // Arrastre "externo": el sistema operativo lleva el archivo fuera del plugin y FL lo
    // recibe igual que si lo arrastrases desde el explorador de Windows.
    dragging = dragStarted = true;
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                [this] { dragging = false; });
}

void TakeCard::mouseUp (const juce::MouseEvent& e)
{
    if (dragStarted) return;
    if (playArea().contains (e.position))
        proc.playTake (index);
    else if (deleteArea().contains (e.position) && hover)
        proc.deleteTake (index);
    else
        proc.selectedTake = index;
    if (onChanged) onChanged();
}

// ================================================================ TakesList
void TakesList::rebuild (int viewportWidth)
{
    cards.clear();
    const int n = proc.getNumTakes();
    int y = 0;
    for (int i = n - 1; i >= 0; --i)  // la más nueva arriba
    {
        auto card = std::make_unique<TakeCard> (lnf, proc, i);
        card->onChanged = [this] { if (onChanged) onChanged(); };
        card->setBounds (0, y, viewportWidth, 92);
        addAndMakeVisible (*card);
        cards.push_back (std::move (card));
        y += 100;
    }
    setSize (viewportWidth, juce::jmax (y, getParentHeight()));
    repaint();
}

void TakesList::refreshAll()     { for (auto& c : cards) c->refresh(); }
void TakesList::repaintPlaying() { for (auto& c : cards) if (proc.getPreviewTake() == c->index) c->repaint(); }

void TakesList::paint (juce::Graphics& g)
{
    if (! cards.empty()) return;
    g.setColour (col::dim);
    g.setFont (lnf.font (14.5f));
    g.drawFittedText (u8 ("Pulsa Grabar y canta.\nCada toma aparece aquí: escúchala\ny arrástrala al piano roll de FL."),
                      getLocalBounds().reduced (12), juce::Justification::centred, 4);
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
        if (proc.isRecording() || proc.isArmed()) proc.stopRecording();
        else                                      proc.startRecording();
    };
    addAndMakeVisible (recButton);
    setupToggle (syncToggle, "Grabar al dar Play", "syncPlay");
    syncToggle.setTooltip (u8 ("La grabación empieza al dar Play en FL y para al pararlo. La toma queda alineada al compás."));

    // Detección
    setupKnob (gateKnob, gateLabel, "Puerta", "gate", false);
    setupKnob (detailKnob, detailLabel, "Detalle", "detail", false);
    detailKnob.setTooltip (u8 ("Al 100 % capta adornos y temblores; más bajo se queda con la melodía principal"));
    setupKnob (octaveKnob, octaveLabel, "Octava", "octave", true);
    calibrateButton.setButtonText ("Calibrar puerta");
    calibrateButton.setTooltip (u8 ("Quédate en silencio 1,5 s: la puerta se ajusta justo por encima del ruido de tu sala"));
    calibrateButton.onClick = [this] { startCalibration(); };
    addAndMakeVisible (calibrateButton);

    // Tonalidad
    setupCombo (rootBox, rootLabel, u8 ("Tónica"), "root");
    setupCombo (scaleBox, scaleLabel, "Escala", "scale");
    detectButton.setButtonText ("Detectar tonalidad");
    detectButton.setTooltip (u8 ("Mira qué notas has cantado (la toma seleccionada, o lo último que has cantado) y elige la tonalidad que mejor encaja"));
    detectButton.onClick = [this] { detectKey(); };
    addAndMakeVisible (detectButton);
    keyResult.setFont (lnf.font (13.5f));
    keyResult.setColour (juce::Label::textColourId, col::dim);
    keyResult.setBorderSize ({});
    addAndMakeVisible (keyResult);
    setupToggle (autoTuneToggle, u8 ("Afinación auto"), "autotune");
    autoTuneToggle.setTooltip (u8 ("Aprende si cantas un poco alto o bajo y lo compensa"));
    tuneLabel.setColour (juce::Label::textColourId, col::dim);
    tuneLabel.setFont (lnf.font (13.5f));
    addAndMakeVisible (tuneLabel);

    // Armonía y expresión
    setupCombo (chordsBox, chordsLabel, "Acordes", "chords");
    chordsBox.setTooltip (u8 ("Añade acordes de la escala debajo de cada nota, en vivo y al exportar"));
    setupToggle (bendToggle, "Pitch bend", "bend");
    bendToggle.setTooltip (u8 ("La rueda de pitch bend sigue tu voz: deslizamientos y vibrato llegan al instrumento"));
    setupCombo (bendRangeBox, bendRangeLabel, "rango del bend", "bendRange");
    bendRangeBox.setTooltip (u8 ("Pon el mismo rango que tenga el instrumento de FL (casi siempre ±2)"));
    setupToggle (velFixedToggle, "Velocity fija", "velFixed");
    velFixedToggle.setTooltip (u8 ("Todas las notas con la misma fuerza en vez de según tu volumen"));

    // Monitor
    setupToggle (synthToggle, u8 ("Oír synth"), "synth");
    setupKnob (volKnob, volLabel, "Volumen", "synthVol", false);
    setupKnob (voiceKnob, voiceLabel, "Tu voz", "voiceMix", false);
    voiceKnob.setTooltip (u8 ("Mezcla tu voz con el synth para oírte mientras cantas (usa auriculares)"));

    // Tomas y exportación
    takesList.onChanged = [this] { seenTakesVersion = -1; };
    takesViewport.setViewedComponent (&takesList, false);
    takesViewport.setScrollBarsShown (true, false);
    takesViewport.setScrollBarThickness (8);
    addAndMakeVisible (takesViewport);
    setupCombo (gridBox, gridLabel, "Rejilla", "grid");
    gridBox.setTooltip (u8 ("A qué figura se ajustan las notas al arrastrar o guardar"));
    setupKnob (strengthKnob, strengthLabel, "Fuerza", "strength", false);
    strengthKnob.setTooltip (u8 ("100 %: notas exactas en la rejilla. Menos: se acercan sin perder tu toque humano"));
    setupToggle (legatoToggle, "Legato", "legato");
    legatoToggle.setTooltip (u8 ("Cada nota dura hasta la siguiente (sin huecos pequeños)"));
    saveButton.setButtonText ("Guardar .mid");
    saveButton.onClick = [this] { saveSelectedTake(); };
    addAndMakeVisible (saveButton);

    for (const auto* id : { "grid", "strength", "legato", "detail" })
        proc.apvts.addParameterListener (id, this);

    setSize (1240, 576);
    timerCallback();
    startTimerHz (60);
}

HumAudioProcessorEditor::~HumAudioProcessorEditor()
{
    stopTimer();
    for (const auto* id : { "grid", "strength", "legato", "detail" })
        proc.apvts.removeParameterListener (id, this);
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

    sliderAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, paramId, s));
    if (auto* param = proc.apvts.getParameter (paramId))  // doble clic = valor por defecto
        s.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));

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
    comboAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, paramId, c));

    l.setText (name, juce::dontSendNotification);
    l.setFont (lnf.font (13.5f));
    l.setColour (juce::Label::textColourId, col::dim);
    l.setBorderSize ({});
    addAndMakeVisible (l);
}

void HumAudioProcessorEditor::setupToggle (juce::ToggleButton& b, const juce::String& text, const juce::String& paramId)
{
    b.setButtonText (text);
    addAndMakeVisible (b);
    buttonAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, paramId, b));
}

void HumAudioProcessorEditor::setParam (const juce::String& id, float value)
{
    if (auto* param = proc.apvts.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

void HumAudioProcessorEditor::startCalibration()
{
    calibrating = true;
    calibrationSamples.clear();
    calibrationStart = juce::Time::getMillisecondCounterHiRes() * 0.001;
}

void HumAudioProcessorEditor::detectKey()
{
    const int numTakes = proc.getNumTakes();
    const int index = numTakes > 0 ? proc.selectedTake : -1;
    const auto key = proc.detectKey (index);
    keyMessageUntil = juce::Time::getMillisecondCounterHiRes() * 0.001 + 12.0;
    if (! key.valid)
    {
        keyResult.setText (u8 ("Canta algunas notas más y vuelve a probar"), juce::dontSendNotification);
        return;
    }
    setParam ("root", (float) key.root);
    setParam ("scale", key.minor ? 2.0f : 1.0f);  // índices de Mayor / Menor en kScales
    juce::String text = juce::String (kNamesEs[key.root]) + (key.minor ? " menor" : " mayor")
                      + " (" + kNames[key.root] + (key.minor ? "m" : "") + ")";
    if (key.margin < 0.05f) text << ", dudosa";
    keyResult.setText (text, juce::dontSendNotification);
}

void HumAudioProcessorEditor::saveSelectedTake()
{
    const int index = proc.selectedTake;
    const auto info = proc.getTakeInfo (index);
    if (info.notes == 0) return;
    chooser = std::make_unique<juce::FileChooser> ("Guardar toma como MIDI",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("hum2midi " + info.name + ".mid"), "*.mid");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, index] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f != juce::File()) proc.saveTakeTo (index, f.withFileExtension ("mid"));
                          });
}

void HumAudioProcessorEditor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

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
        tuneText = std::abs (cents) < 5 ? u8 ("afinado") : juce::String (std::abs (cents)) + u8 (" ¢ ") + (cents < 0 ? "bajo" : "alto");
    tuneLabel.setText (tuneText, juce::dontSendNotification);
    if (keyMessageUntil > 0.0 && now > keyMessageUntil) { keyResult.setText ({}, juce::dontSendNotification); keyMessageUntil = 0.0; }

    // Calibración de la puerta
    if (calibrating)
    {
        calibrationSamples.push_back (proc.inputLevelDb.load());
        const double elapsed = now - calibrationStart;
        if (elapsed >= 1.5 && ! calibrationSamples.empty())
        {
            calibrating = false;
            std::sort (calibrationSamples.begin(), calibrationSamples.end());
            const float noise = calibrationSamples[(size_t) ((double) calibrationSamples.size() * 0.9)];
            const float gate = juce::jlimit (-70.0f, -15.0f, noise + 8.0f);  // 8 dB por encima del ruido
            setParam ("gate", gate);
            calibrateButton.setButtonText ("Puerta en " + juce::String (juce::roundToInt (gate)) + " dB");
            calibrationMessageUntil = now + 2.5;
        }
        else
            calibrateButton.setButtonText (u8 ("Silencio… ") + juce::String (juce::jmax (0.0, 1.5 - elapsed), 1) + " s");
    }
    else if (calibrationMessageUntil > 0.0 && now > calibrationMessageUntil)
    {
        calibrateButton.setButtonText ("Calibrar puerta");
        calibrationMessageUntil = 0.0;
    }

    // Grabación
    const auto mode = proc.isRecording() ? RecButton::Recording : (proc.isArmed() ? RecButton::Armed : RecButton::Ready);
    const float blink = 0.5f + 0.5f * std::sin ((float) now * 6.0f);
    if (recButton.mode != mode || recButton.liveNotes != proc.getLiveNoteCount() || mode == RecButton::Armed)
    {
        recButton.mode = mode;
        recButton.liveNotes = proc.getLiveNoteCount();
        recButton.blink = blink;
        recButton.repaint();
    }

    // Tomas
    if (proc.takesVersion.load() != seenTakesVersion)
    {
        seenTakesVersion = proc.takesVersion.load();
        takesList.rebuild (takesViewport.getWidth() - takesViewport.getScrollBarThickness() - 4);
        repaint (takesArea);
    }
    else if (exportSettingsChanged)
        takesList.refreshAll();
    exportSettingsChanged = false;
    takesList.repaintPlaying();
    saveButton.setEnabled (proc.getNumTakes() > 0);
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
        g.fillRect ((float) x, (float) controlsArea.getY() + 12.0f, 1.0f, (float) controlsArea.getHeight() - 24.0f);

    // ---- Panel de tomas
    const auto t = takesArea.toFloat();
    g.setColour (col::panel);
    g.fillRoundedRectangle (t, 8.0f);
    g.setColour (col::line);
    g.drawRoundedRectangle (t.reduced (0.5f), 8.0f, 1.0f);
    g.setColour (col::text);
    g.setFont (lnf.strong (16.0f));
    g.drawText ("Tomas", juce::Rectangle<float> (t.getX() + 16.0f, t.getY() + 10.0f, 120.0f, 24.0f), juce::Justification::centredLeft);
    const int numTakes = proc.getNumTakes();
    if (numTakes > 0)
    {
        g.setColour (col::dim);
        g.setFont (lnf.font (13.5f));
        g.drawText (juce::String (numTakes) + " de 12", juce::Rectangle<float> (t.getRight() - 136.0f, t.getY() + 10.0f, 120.0f, 24.0f),
                    juce::Justification::centredRight);
    }
    const float exportTop = t.getBottom() - 186.0f;
    g.setColour (col::line);
    g.fillRect (t.getX() + 1.0f, exportTop, t.getWidth() - 2.0f, 1.0f);
    g.setColour (col::dim);
    g.setFont (lnf.font (13.5f));
    g.drawText ("Al arrastrar o guardar", juce::Rectangle<float> (t.getX() + 16.0f, exportTop + 10.0f, 200.0f, 18.0f),
                juce::Justification::centredLeft);
}

void HumAudioProcessorEditor::placeKnob (juce::Slider& s, juce::Label& l, juce::Rectangle<int> area)
{
    l.setBounds (area.getX(), area.getY() + 10, area.getWidth(), 18);
    s.setBounds (area.getX(), area.getY() + 30, area.getWidth(), 86);
}

void HumAudioProcessorEditor::resized()
{
    const int W = getWidth(), H = getHeight();

    // Cabecera
    recButton.setBounds (16 + 896 - 160, 11, 160, 34);
    syncToggle.setBounds (recButton.getX() - 20 - 190, 11, 190, 34);

    // Pantalla: lectura | pitch roll
    screenArea = { 16, 58, 896, 268 };
    auto s = screenArea.reduced (1);
    readout.setBounds (s.removeFromLeft (184));
    s.removeFromLeft (1);
    roll.setBounds (s);

    // Controles: detección | tonalidad | armonía | monitor
    controlsArea = { 16, 340, 896, H - 340 - 14 };
    dividers.clear();
    const int x = controlsArea.getX(), y = controlsArea.getY();

    // Detección
    placeKnob (gateKnob, gateLabel, { x, y, 90, 120 });
    placeKnob (detailKnob, detailLabel, { x + 90, y, 90, 120 });
    placeKnob (octaveKnob, octaveLabel, { x + 180, y, 90, 120 });
    calibrateButton.setBounds (x + 10, y + 146, 250, 34);
    dividers.push_back (x + 279);

    // Tonalidad
    const int x2 = x + 288;
    rootLabel.setBounds (x2, y + 10, 80, 18);
    rootBox.setBounds (x2, y + 32, 80, 34);
    scaleLabel.setBounds (x2 + 92, y + 10, 160, 18);
    scaleBox.setBounds (x2 + 92, y + 32, 160, 34);
    detectButton.setBounds (x2, y + 78, 252, 34);
    keyResult.setBounds (x2, y + 116, 252, 20);
    autoTuneToggle.setBounds (x2, y + 146, 146, 34);
    tuneLabel.setBounds (x2 + 146, y + 146, 106, 34);
    dividers.push_back (x2 + 261);

    // Armonía y expresión
    const int x3 = x2 + 270;
    chordsLabel.setBounds (x3, y + 10, 170, 18);
    chordsBox.setBounds (x3, y + 32, 170, 34);
    bendToggle.setBounds (x3, y + 78, 170, 34);
    bendRangeBox.setBounds (x3, y + 114, 84, 30);
    bendRangeLabel.setBounds (x3 + 92, y + 114, 80, 30);
    velFixedToggle.setBounds (x3, y + 150, 170, 34);
    dividers.push_back (x3 + 179);

    // Monitor
    const int x4 = x3 + 188, w4 = controlsArea.getRight() - x4;
    synthToggle.setBounds (x4, y + 6, w4, 34);
    placeKnob (volKnob, volLabel, { x4, y + 46, w4 / 2, 120 });
    placeKnob (voiceKnob, voiceLabel, { x4 + w4 / 2, y + 46, w4 - w4 / 2, 120 });

    // Panel de tomas
    takesArea = { 928, 58, W - 928 - 16, H - 58 - 14 };
    const auto t = takesArea;
    const int exportTop = t.getBottom() - 186;
    takesViewport.setBounds (t.getX() + 10, t.getY() + 44, t.getWidth() - 20, exportTop - (t.getY() + 44) - 10);
    gridLabel.setBounds (t.getX() + 16, exportTop + 38, 150, 18);
    gridBox.setBounds (t.getX() + 16, exportTop + 60, 160, 34);
    placeKnob (strengthKnob, strengthLabel, { t.getRight() - 106, exportTop + 22, 90, 120 });
    legatoToggle.setBounds (t.getX() + 16, exportTop + 102, 160, 30);
    saveButton.setBounds (t.getX() + 16, exportTop + 140, t.getWidth() - 32, 34);
    seenTakesVersion = -1;  // recolocar las tarjetas con el nuevo ancho
}
