#include "PluginProcessor.h"
#include "PluginEditor.h"

static const juce::StringArray kNoteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
static constexpr int kPpq = 960;  // ticks por negra en los .mid
static constexpr double kGridTicks[] = { 0.0, 960.0, 480.0, 240.0, 120.0, 320.0, 160.0 };

static juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

// ---------------------------------------------------------------- Parámetros
// Todo lo que aparece en la interfaz y se guarda con el proyecto de FL.
juce::AudioProcessorValueTreeState::ParameterLayout HumAudioProcessor::createLayout()
{
    juce::StringArray scaleNames;
    for (const auto& s : hum::kScales)
        scaleNames.add (u8 (s.name));

    const auto percent = juce::AudioParameterFloatAttributes().withStringFromValueFunction (
        [] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; });

    return {
        std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gate", 1 },
            "Puerta", juce::NormalisableRange<float> (-70.0f, -10.0f, 0.5f), -40.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                [] (float v, int) { return juce::String (v, 1) + " dB"; })),
        std::make_unique<juce::AudioParameterInt>    (juce::ParameterID { "octave", 1 }, "Octava", -2, 2, 0),
        // Detalle: 100 % lo capta todo (adornos, temblores); menos = solo la melodía principal.
        std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "detail", 1 }, "Detalle",
            juce::NormalisableRange<float> (0.0f, 1.0f), 0.8f, percent),
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "root", 1 }, u8 ("Tónica"), kNoteNames, 0),
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "scale", 1 }, "Escala", scaleNames, 0),
        std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "synth", 1 }, "Synth", true),
        std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "autotune", 1 }, u8 ("Afinación auto"), true),
        std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "synthVol", 1 }, "Volumen",
            juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent),

        // Nuevos
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "chords", 1 }, "Acordes",
            juce::StringArray { "Sin acordes", u8 ("Tríadas"), u8 ("Con séptima"), "Quintas", "Octavas" }, 0),
        std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "bend", 1 }, "Pitch bend", false),
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "bendRange", 1 }, "Rango bend",
            [] { juce::StringArray a; for (int i = 1; i <= 12; ++i) a.add (u8 ("±") + juce::String (i) + " st"); return a; }(), 1),
        std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "velFixed", 1 }, "Velocity fija", false),
        std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "voiceMix", 1 }, "Tu voz",
            juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent),
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "grid", 1 }, "Rejilla",
            juce::StringArray { "Sin cuantizar", "1/4", "1/8", "1/16", "1/32", "1/8 tresillo", "1/16 tresillo" }, 3),
        std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "strength", 1 }, "Fuerza",
            juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f, percent),
        std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "legato", 1 }, "Legato", false),
        std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "syncPlay", 1 }, "Grabar al dar Play", false),
    };
}

HumAudioProcessor::HumAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pGate      = apvts.getRawParameterValue ("gate");
    pOctave    = apvts.getRawParameterValue ("octave");
    pRoot      = apvts.getRawParameterValue ("root");
    pScale     = apvts.getRawParameterValue ("scale");
    pSynth     = apvts.getRawParameterValue ("synth");
    pSynthVol  = apvts.getRawParameterValue ("synthVol");
    pAutoTune  = apvts.getRawParameterValue ("autotune");
    pDetail    = apvts.getRawParameterValue ("detail");
    pChords    = apvts.getRawParameterValue ("chords");
    pBend      = apvts.getRawParameterValue ("bend");
    pBendRange = apvts.getRawParameterValue ("bendRange");
    pVelFixed  = apvts.getRawParameterValue ("velFixed");
    pVoiceMix  = apvts.getRawParameterValue ("voiceMix");
    pGrid      = apvts.getRawParameterValue ("grid");
    pStrength  = apvts.getRawParameterValue ("strength");
    pLegato    = apvts.getRawParameterValue ("legato");
    pSyncPlay  = apvts.getRawParameterValue ("syncPlay");
    for (auto& h : pcHistory) h = 0.0f;
    startTimerHz (20);  // vacía la cola de grabación aunque la ventana del plugin esté cerrada
}

HumAudioProcessor::~HumAudioProcessor() { stopTimer(); }

bool HumAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto ok = [] (const juce::AudioChannelSet& s)
    { return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo(); };
    return ok (in) && ok (out);
}

// FL llama a esto antes de empezar (y cada vez que cambias la frecuencia de muestreo).
// Aquí, y no en processBlock, es donde se reserva memoria.
void HumAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sr = sampleRate;
    detector.prepare (sampleRate);
    synth.prepare (sampleRate);
    chordCount = 0;
    lastBend = 8192;
    currentNote = -1;
}

hum::ChordType HumAudioProcessor::chordType() const
{
    return (hum::ChordType) juce::jlimit (0, 4, (int) pChords->load());
}

// ---------------------------------------------------------------- Audio
void HumAudioProcessor::readHostPosition()
{
    blockPlaying = blockHasPpq = false;
    double quartersPerBar = 4.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm()) blockBpm = juce::jlimit (20.0, 999.0, *bpm);
            blockPlaying = pos->getIsPlaying();
            if (auto ts = pos->getTimeSignature())
                if (ts->numerator > 0 && ts->denominator > 0)
                    quartersPerBar = ts->numerator * 4.0 / ts->denominator;
            if (auto ppq = pos->getPpqPosition()) { blockPpq = *ppq; blockHasPpq = true; }
            if (auto bar = pos->getPpqPositionOfLastBarStart()) blockBarPpq = *bar;
            else blockBarPpq = std::floor (blockPpq / quartersPerBar) * quartersPerBar;
        }
    blockTimeSig = juce::jmax (1, (int) std::lround (quartersPerBar));
    hostBpm = blockBpm;
    hostPlaying = blockPlaying;
}

void HumAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();
    readHostPosition();

    // Parámetros: se leen una vez por bloque.
    auto& tracker = detector.tracker;
    detector.gateDb  = pGate->load();
    tracker.octave   = (int) pOctave->load();
    tracker.root     = (int) pRoot->load();
    tracker.autoTune = pAutoTune->load() > 0.5f;
    tracker.setDetail (pDetail->load());
    const auto& scale = hum::kScales[juce::jlimit (0, hum::kNumScales - 1, (int) pScale->load())];
    tracker.steps     = scale.steps;
    tracker.stepCount = scale.count;
    const bool  synthOn  = pSynth->load() > 0.5f;
    const float synthVol = pSynthVol->load();
    const float voiceMix = pVoiceMix->load();

    handleRecordState();

    // Reproducción de tomas: arrancar/parar a petición de la UI
    if (playState.load() == PlayStart)     { playPos = 0; playIndex = 0; synth.allOff (1); playState = Playing; }
    else if (playState.load() == PlayStop) { synth.allOff (1); playState = PlayStopped; }

    // Si apagas el pitch bend, la rueda vuelve al centro
    const bool bendOn = pBend->load() > 0.5f;
    if (bendWasOn && ! bendOn) sendBend (0.0f, 0, midi);
    bendWasOn = bendOn;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int i = 0; i < numSamples; ++i)
    {
        // 1. Mezclamos los canales de entrada a mono.
        float s = 0.0f;
        for (int ch = 0; ch < numIn; ++ch)
            s += buffer.getSample (ch, i);
        if (numIn > 0)
            s /= (float) numIn;

        // 2. Al detector (filtra, analiza cada 8 ms y decide notas).
        hum::Detector::Event events[2];
        const int n = detector.push (s, events);
        for (int k = 0; k < n; ++k)
            emit (events[k], i, midi);
        if (detector.frameCount != lastFrameCount)
        {
            lastFrameCount = detector.frameCount;
            pushPitchFrame (i, midi);
        }

        // 3. Reproducción de una toma (solo por el synth interno, no sale como MIDI)
        if (playState.load() == Playing)
            processPreview();

        // 4. Salida: synth interno + (si quieres) tu propia voz para oírte
        const float out = synth.next (synthOn ? synthVol : 0.0f, juce::jmax (0.3f, synthVol)) + s * voiceMix;
        for (int ch = 0; ch < numOut; ++ch)
            buffer.setSample (ch, i, out);
    }

    if (playState.load() == Playing)
        previewProgress = (float) playPos / (float) juce::jmax ((juce::int64) 1, playLength);

    currentNote  = tracker.current;
    inputLevelDb = detector.levelDb;
    tuneCents    = tracker.tuneOffset() * 100.0f;

    sampleClock += numSamples;
    if (recState.load() == Recording)
        recordedLength = sampleClock - recordStart;
}

void HumAudioProcessor::processPreview()
{
    while (playIndex < playCount && playData[(size_t) playIndex].pos <= playPos)
    {
        const auto& e = playData[(size_t) playIndex++];
        if (e.on) synth.noteOn (e.note, 1, 0.8f);
        else      synth.noteOff (e.note, 1);
    }
    if (++playPos >= playLength)
    {
        synth.allOff (1);
        previewProgress = 1.0f;
        playState = PlayStopped;
    }
}

void HumAudioProcessor::emit (const hum::Detector::Event& e, int sampleOffset, juce::MidiBuffer& midi)
{
    const int vel = pVelFixed->load() > 0.5f ? 100 : e.velocity;
    if (e.on)
    {
        if (pBend->load() > 0.5f) sendBend (0.0f, sampleOffset, midi);  // cada nota empieza centrada
        midi.addEvent (juce::MidiMessage::noteOn (1, e.note, (juce::uint8) vel), sampleOffset);
        synth.noteOn (e.note, 0, (float) vel / 127.0f);

        // Acordes debajo de la melodía, ajustados a la escala elegida
        const auto& scale = hum::kScales[juce::jlimit (0, hum::kNumScales - 1, (int) pScale->load())];
        chordCount = hum::chordTones (e.note, chordType(), (int) pRoot->load(), scale.steps, scale.count, chordNotes);
        const auto chordVel = (juce::uint8) juce::jmax (1, vel * 85 / 100);
        for (int k = 0; k < chordCount; ++k)
            if (chordNotes[k] >= 0 && chordNotes[k] < 128)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, chordNotes[k], chordVel), sampleOffset);
                synth.noteOn (chordNotes[k], 0, (float) chordVel / 127.0f);
            }
    }
    else
    {
        midi.addEvent (juce::MidiMessage::noteOff (1, e.note), sampleOffset);
        synth.noteOff (e.note, 0);
        for (int k = 0; k < chordCount; ++k)
            if (chordNotes[k] >= 0 && chordNotes[k] < 128)
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, chordNotes[k]), sampleOffset);
                synth.noteOff (chordNotes[k], 0);
            }
        chordCount = 0;
    }

    if (recState.load() == Recording)
    {
        // El MIDI en vivo sale cuando la nota se confirma (~40-60 ms tarde), pero el detector
        // nos dice cuándo ocurrió de verdad (samplesAgo): la toma cae donde la cantaste.
        // En la toma solo va la melodía; los acordes se añaden al exportar (así puedes cambiarlos).
        const juce::int64 pos = sampleClock + sampleOffset - recordStart - e.samplesAgo;
        pushRecEvent ({ e.on, e.note, vel, juce::jmax ((juce::int64) 0, pos), e.replace });
    }
}

void HumAudioProcessor::sendBend (float semitones, int sampleOffset, juce::MidiBuffer& midi)
{
    const float range = pBendRange->load() + 1.0f;  // la opción 0 es ±1 st
    const int value = juce::jlimit (0, 16383, 8192 + juce::roundToInt (semitones / range * 8191.0f));
    if (value == lastBend || (std::abs (value - lastBend) < 24 && value != 8192))
        return;  // cambios minúsculos no merecen un mensaje
    midi.addEvent (juce::MidiMessage::pitchWheel (1, value), sampleOffset);
    lastBend = value;
    synth.setBend ((float) (value - 8192) / 8191.0f * range);
}

void HumAudioProcessor::pushPitchFrame (int sampleOffset, juce::MidiBuffer& midi)
{
    const auto& tr = detector.tracker;
    const float display = detector.lastMidi + 12.0f * (float) tr.octave - tr.tuneOffset();

    // Marcas de tiempo/compás para el pitch roll (solo con FL en marcha)
    juce::uint8 beat = 0;
    if (blockPlaying && blockHasPpq)
    {
        const double ppqNow = blockPpq + (double) sampleOffset / sr * blockBpm / 60.0;
        const double beatIndex = std::floor (ppqNow);
        if (std::abs (beatIndex - lastBeatIndex) > 0.5)
        {
            const double rel = beatIndex - blockBarPpq;
            beat = std::abs (rel - blockTimeSig * std::round (rel / blockTimeSig)) < 0.01 ? 2 : 1;
            lastBeatIndex = beatIndex;
        }
    }
    else lastBeatIndex = -1.0e9;

    const PitchFrame f { display, (short) tr.current, detector.lastVoiced, recState.load() == Recording, beat };
    const auto scope = pitchFifo.write (1);
    if (scope.blockSize1 > 0)      pitchData[(size_t) scope.startIndex1] = f;
    else if (scope.blockSize2 > 0) pitchData[(size_t) scope.startIndex2] = f;

    // Pitch bend: la rueda sigue a tu voz respecto a la nota que suena
    if (pBend->load() > 0.5f && tr.current >= 0 && detector.lastVoiced)
    {
        const float range = pBendRange->load() + 1.0f;
        sendBend (juce::jlimit (-range, range, display - (float) tr.current), sampleOffset, midi);
    }

    // Histograma de notas (para sugerir la tonalidad): se olvida a la mitad cada ~45 s
    constexpr float decay = 0.99988f;
    for (int i = 0; i < 12; ++i)
        pcHistory[(size_t) i] = pcHistory[(size_t) i].load() * decay;
    if (tr.current >= 0)
    {
        auto& h = pcHistory[(size_t) (tr.current % 12)];
        h = h.load() + 0.008f;
    }
}

// ---------------------------------------------------------------- Grabación
// La UI pide arrancar/parar; el hilo de audio lo ejecuta al principio del siguiente bloque
// para que el instante exacto salga de su propio reloj de muestras y de la posición de FL.
void HumAudioProcessor::handleRecordState()
{
    int state = recState.load();
    const auto begin = [this] (bool synced)
    {
        recSynced = synced;
        recordStart = sampleClock;
        recordedLength = 0;
        double offset = 0.0;
        int bar = 0;
        if (blockPlaying && blockHasPpq)
        {
            // La toma empieza en el inicio del compás: así, al arrastrarla a ese compás en el
            // piano roll, cada nota cae donde la cantaste y la rejilla coincide con la de FL.
            offset = juce::jmax (0.0, blockPpq - blockBarPpq);
            bar = (int) std::lround (blockBarPpq / blockTimeSig) + 1;
        }
        recOffsetQuarters = offset;
        recBar = bar;
        recBpm = blockBpm;
        if (detector.tracker.current >= 0)  // ya estabas cantando una nota al darle a grabar
            pushRecEvent ({ true, detector.tracker.current, 100, 0, false });
    };

    if (state == StartRequested && recState.compare_exchange_strong (state, Recording))
        begin (false);
    else if (state == Armed && blockPlaying && recState.compare_exchange_strong (state, Recording))
        begin (true);
    else if ((state == StopRequested || (state == Recording && recSynced && ! blockPlaying))
             && recState.compare_exchange_strong (state, Idle))
    {
        recordedLength = sampleClock - recordStart;
        if (detector.tracker.current >= 0)
            pushRecEvent ({ false, detector.tracker.current, 0, recordedLength.load(), false });
        ++recStops;  // la UI convierte lo grabado en una toma
    }
}

void HumAudioProcessor::pushRecEvent (const RecEvent& e)
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 > 0)      fifoData[(size_t) scope.startIndex1] = e;
    else if (scope.blockSize2 > 0) fifoData[(size_t) scope.startIndex2] = e;
    // si la cola estuviese llena, el evento se pierde (2048 huecos y 20 vaciados/s: no pasa)
}

int HumAudioProcessor::popPitchFrames (PitchFrame* dest, int maxFrames)
{
    const int num = juce::jmin (maxFrames, pitchFifo.getNumReady());
    int k = 0;
    pitchFifo.read (num).forEach ([&] (int index) { dest[k++] = pitchData[(size_t) index]; });
    return num;
}

void HumAudioProcessor::drainFifo()
{
    const int ready = fifo.getNumReady();
    if (ready == 0)
        return;
    fifo.read (ready).forEach ([this] (int index)
    {
        const auto& e = fifoData[(size_t) index];
        current.events.push_back (e);
        if (e.on && ! e.replace) ++liveNoteCount;
    });
}

void HumAudioProcessor::timerCallback()
{
    drainFifo();
    if (recStops.load() != seenStops)
    {
        seenStops = recStops.load();
        finishTake();
    }

    // Reproducción: ¿terminó?, ¿hay que arrancar otra?
    if (previewTake >= 0 && pendingPreview < 0 && playState.load() == PlayStopped)
        previewTake = -1;
    if (pendingPreview >= 0 && playState.load() == PlayStopped)
    {
        const juce::ScopedLock sl (takesLock);
        if (juce::isPositiveAndBelow (pendingPreview, (int) takes.size()))
        {
            const auto& t = takes[(size_t) pendingPreview];
            const auto notes = renderTake (t, true);
            const double samplesPerTick = 60.0 / t.bpm / kPpq * sr;
            const double first = notes.empty() ? 0.0 : notes.front().start;
            const juce::int64 lead = (juce::int64) (0.12 * sr);
            std::vector<PlayEvent> ev;
            for (const auto& n : notes)
            {
                ev.push_back ({ lead + (juce::int64) ((n.start - first) * samplesPerTick), (short) n.note, true });
                ev.push_back ({ lead + (juce::int64) ((n.end - first) * samplesPerTick) - 1, (short) n.note, false });
            }
            std::stable_sort (ev.begin(), ev.end(), [] (const PlayEvent& a, const PlayEvent& b)
                              { return a.pos < b.pos || (a.pos == b.pos && ! a.on && b.on); });
            playCount = juce::jmin ((int) ev.size(), (int) playData.size());
            std::copy (ev.begin(), ev.begin() + playCount, playData.begin());
            playLength = (playCount > 0 ? playData[(size_t) playCount - 1].pos : 0) + (juce::int64) (0.3 * sr);
            previewProgress = 0.0f;
            previewTake = pendingPreview;
            playState = PlayStart;
        }
        pendingPreview = -1;
    }
}

void HumAudioProcessor::startRecording()
{
    stopPreview();
    drainFifo();
    current = {};
    liveNoteCount = 0;
    seenStops = recStops.load();
    recState = pSyncPlay->load() > 0.5f ? Armed : StartRequested;
}

void HumAudioProcessor::stopRecording()
{
    int state = recState.load();
    if (state == Armed) recState.compare_exchange_strong (state, Idle);  // nunca llegó a grabar
    else if (state == Recording || state == StartRequested) recState = StopRequested;
}

bool HumAudioProcessor::isRecording() const
{
    const int state = recState.load();
    return state == StartRequested || state == Recording || state == StopRequested;
}

bool HumAudioProcessor::isArmed() const { return recState.load() == Armed; }

void HumAudioProcessor::finishTake()
{
    drainFifo();
    current.length = recordedLength.load();
    current.sampleRate = sr;
    current.bpm = recBpm.load();
    current.offsetQuarters = recOffsetQuarters.load();
    current.bar = recBar.load();

    const bool hasNotes = std::any_of (current.events.begin(), current.events.end(), [] (const RecEvent& e) { return e.on; });
    if (hasNotes)
    {
        const juce::ScopedLock sl (takesLock);
        current.name = "Toma " + juce::String (++takeCounter);
        takes.push_back (std::move (current));
        if ((int) takes.size() > kMaxTakes)
            takes.erase (takes.begin());
        selectedTake = (int) takes.size() - 1;
        ++takesVersion;
    }
    current = {};
    liveNoteCount = 0;
}

// ---------------------------------------------------------------- Tomas
int HumAudioProcessor::getNumTakes() const
{
    const juce::ScopedLock sl (takesLock);
    return (int) takes.size();
}

HumAudioProcessor::TakeInfo HumAudioProcessor::getTakeInfo (int index) const
{
    const juce::ScopedLock sl (takesLock);
    TakeInfo info;
    if (! juce::isPositiveAndBelow (index, (int) takes.size()))
        return info;
    const auto& t = takes[(size_t) index];
    info.name = t.name;
    info.seconds = (double) t.length / t.sampleRate;
    info.bar = t.bar;
    info.preview = renderTake (t, false);
    info.notes = (int) info.preview.size();
    return info;
}

void HumAudioProcessor::deleteTake (int index)
{
    const juce::ScopedLock sl (takesLock);
    if (! juce::isPositiveAndBelow (index, (int) takes.size()))
        return;
    if (previewTake == index) stopPreview();
    takes.erase (takes.begin() + index);
    if (previewTake > index) --previewTake;
    selectedTake = juce::jmin (selectedTake >= index ? selectedTake - 1 : selectedTake, (int) takes.size() - 1);
    if (selectedTake < 0 && ! takes.empty()) selectedTake = 0;
    ++takesVersion;
}

// Convierte una toma en notas listas para MIDI, con los ajustes actuales de detalle,
// cuantización, legato y (opcionalmente) acordes.
std::vector<hum::TakeNote> HumAudioProcessor::renderTake (const Take& t, bool withChords) const
{
    const double offsetTicks = t.offsetQuarters * kPpq;
    const auto toTicks = [&] (juce::int64 samples) { return offsetTicks + (double) samples / t.sampleRate * t.bpm / 60.0 * kPpq; };

    // 1. Emparejamos note on / note off
    std::vector<hum::TakeNote> notes;
    int openNote = -1, openVel = 0;
    double openStart = 0.0;
    for (const auto& e : t.events)
    {
        const double tick = toTicks (e.samplePos);
        if (e.on && e.replace && openNote < 0 && ! notes.empty())
        {
            // El detector corrigió la nota anterior (scoop, paso de glissando...): se borra
            // y la buena ocupa su lugar desde su inicio.
            openStart = notes.back().start;
            openVel   = notes.back().velocity;
            openNote  = e.note;
            notes.pop_back();
        }
        else if (e.on)
        {
            if (openNote >= 0) notes.push_back ({ openNote, openVel, openStart, tick });
            openNote = e.note; openVel = e.velocity; openStart = tick;
        }
        else if (e.note == openNote)
        {
            notes.push_back ({ openNote, openVel, openStart, tick });
            openNote = -1;
        }
    }
    if (openNote >= 0)  // nota que seguía sonando al parar
        notes.push_back ({ openNote, openVel, openStart, toTicks (t.length) });

    // 2. Menos detalle = las notas muy cortas (adornos, temblores) se funden con su vecina.
    const double detail = pDetail->load();
    if (detail < 0.999)
    {
        const auto msToTicks = [&] (double ms) { return ms * 0.001 * t.bpm / 60.0 * kPpq; };
        hum::mergeShortNotes (notes, msToTicks (40.0 + 110.0 * (1.0 - detail)), msToTicks (15.0));
    }

    // 3. Cuantización (con fuerza) y legato. La rejilla cuenta desde el inicio del compás.
    const double grid = kGridTicks[juce::jlimit (0, 6, (int) pGrid->load())];
    hum::quantizeNotes (notes, grid, pStrength->load(), pLegato->load() > 0.5f, kPpq);
    hum::makeMonophonic (notes);

    // 4. Acordes
    if (withChords && chordType() != hum::NoChord)
    {
        const auto& scale = hum::kScales[juce::jlimit (0, hum::kNumScales - 1, (int) pScale->load())];
        std::vector<hum::TakeNote> all;
        for (const auto& n : notes)
        {
            all.push_back (n);
            int tones[4];
            const int c = hum::chordTones (n.note, chordType(), (int) pRoot->load(), scale.steps, scale.count, tones);
            for (int k = 0; k < c; ++k)
                if (tones[k] >= 0 && tones[k] < 128)
                    all.push_back ({ tones[k], juce::jmax (1, n.velocity * 85 / 100), n.start, n.end });
        }
        notes.swap (all);
    }
    return notes;
}

juce::File HumAudioProcessor::writeMidi (const std::vector<hum::TakeNote>& notes, double bpm, const juce::String& name)
{
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::round (60'000'000.0 / bpm)), 0.0);
    for (const auto& n : notes)
    {
        seq.addEvent (juce::MidiMessage::noteOn (1, n.note, (juce::uint8) juce::jlimit (1, 127, n.velocity)), n.start);
        seq.addEvent (juce::MidiMessage::noteOff (1, n.note), n.end);
    }
    seq.updateMatchedPairs();

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote (kPpq);
    midiFile.addTrack (seq);

    // Nombre único: arrastrar dos veces seguidas no pisa un archivo que FL aún está leyendo.
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hum2midi");
    dir.createDirectory();
    auto file = dir.getChildFile ("hum2midi " + name + " " + juce::String (++fileCounter) + ".mid");
    file.deleteFile();
    juce::FileOutputStream os (file);
    if (! os.openedOk() || ! midiFile.writeTo (os))
        return {};
    os.flush();
    return file;
}

juce::File HumAudioProcessor::writeTakeFile (int index)
{
    std::vector<hum::TakeNote> notes;
    double bpm = 120.0;
    juce::String name;
    {
        const juce::ScopedLock sl (takesLock);
        if (! juce::isPositiveAndBelow (index, (int) takes.size()))
            return {};
        const auto& t = takes[(size_t) index];
        notes = renderTake (t, true);
        bpm = t.bpm;
        name = t.name;
    }
    if (notes.empty())
        return {};
    return writeMidi (notes, bpm, name);
}

bool HumAudioProcessor::saveTakeTo (int index, const juce::File& destination)
{
    const auto file = writeTakeFile (index);
    return file.existsAsFile() && file.copyFileTo (destination);
}

void HumAudioProcessor::playTake (int index)
{
    if (index == previewTake && playState.load() != PlayStopped)
    {
        stopPreview();
        return;
    }
    pendingPreview = index;
    if (playState.load() != PlayStopped)
        playState = PlayStop;  // primero suelta la que suena; el timer arranca la nueva
}

void HumAudioProcessor::stopPreview()
{
    pendingPreview = -1;
    if (playState.load() != PlayStopped)
        playState = PlayStop;
    previewTake = -1;
}

hum::KeyResult HumAudioProcessor::detectKey (int index) const
{
    float weight[12] {};
    {
        const juce::ScopedLock sl (takesLock);
        if (juce::isPositiveAndBelow (index, (int) takes.size()))
        {
            for (const auto& n : renderTake (takes[(size_t) index], false))
                weight[n.note % 12] += (float) (n.end - n.start);
        }
        else
            for (int i = 0; i < 12; ++i) weight[i] = pcHistory[(size_t) i].load();
    }
    return hum::detectKey (weight);
}

// ---------------------------------------------------------------- Estado / editor
// Parámetros + tomas en el mismo bloque: al guardar el proyecto de FL se guardan tus tomas.
void HumAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto xml = apvts.copyState().createXml();
    if (! xml) return;

    auto* takesXml = xml->createNewChildElement ("TAKES");
    const juce::ScopedLock sl (takesLock);
    takesXml->setAttribute ("counter", takeCounter);
    for (const auto& t : takes)
    {
        auto* tx = takesXml->createNewChildElement ("TAKE");
        tx->setAttribute ("name", t.name);
        tx->setAttribute ("length", juce::String (t.length));
        tx->setAttribute ("sr", t.sampleRate);
        tx->setAttribute ("bpm", t.bpm);
        tx->setAttribute ("offset", t.offsetQuarters);
        tx->setAttribute ("bar", t.bar);
        juce::MemoryOutputStream mo;
        for (const auto& e : t.events)
        {
            mo.writeByte ((char) ((e.on ? 1 : 0) | (e.replace ? 2 : 0)));
            mo.writeByte ((char) e.note);
            mo.writeByte ((char) e.velocity);
            mo.writeInt64 (e.samplePos);
        }
        tx->setAttribute ("events", mo.getMemoryBlock().toBase64Encoding());
    }
    copyXmlToBinary (*xml, destData);
}

void HumAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (! xml || ! xml->hasTagName (apvts.state.getType()))
        return;

    std::vector<Take> loaded;
    int counter = 0;
    if (auto* takesXml = xml->getChildByName ("TAKES"))
    {
        counter = takesXml->getIntAttribute ("counter");
        for (auto* tx : takesXml->getChildIterator())
        {
            Take t;
            t.name = tx->getStringAttribute ("name");
            t.length = tx->getStringAttribute ("length").getLargeIntValue();
            t.sampleRate = tx->getDoubleAttribute ("sr", 44100.0);
            t.bpm = tx->getDoubleAttribute ("bpm", 120.0);
            t.offsetQuarters = tx->getDoubleAttribute ("offset");
            t.bar = tx->getIntAttribute ("bar");
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (tx->getStringAttribute ("events")))
            {
                juce::MemoryInputStream mi (mb, false);
                while (mi.getNumBytesRemaining() >= 11)
                {
                    RecEvent e {};
                    const int flags = (juce::uint8) mi.readByte();
                    e.on = (flags & 1) != 0;
                    e.replace = (flags & 2) != 0;
                    e.note = (juce::uint8) mi.readByte();
                    e.velocity = (juce::uint8) mi.readByte();
                    e.samplePos = mi.readInt64();
                    t.events.push_back (e);
                }
            }
            if (! t.events.empty()) loaded.push_back (std::move (t));
        }
        xml->removeChildElement (takesXml, true);
    }
    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    const juce::ScopedLock sl (takesLock);
    stopPreview();
    takes = std::move (loaded);
    takeCounter = juce::jmax (counter, (int) takes.size());
    selectedTake = (int) takes.size() - 1;
    ++takesVersion;
}

juce::AudioProcessorEditor* HumAudioProcessor::createEditor() { return new HumAudioProcessorEditor (*this); }

// Punto de entrada que FL Studio busca dentro del .vst3
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HumAudioProcessor(); }
