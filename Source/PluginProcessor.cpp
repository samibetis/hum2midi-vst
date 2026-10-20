#include "PluginProcessor.h"
#include "PluginEditor.h"

static const juce::StringArray kNoteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// ---------------------------------------------------------------- Parámetros
// Todo lo que aparece en la interfaz y se guarda con el proyecto de FL.
juce::AudioProcessorValueTreeState::ParameterLayout HumAudioProcessor::createLayout()
{
    juce::StringArray scaleNames;
    for (const auto& s : hum::kScales)
        scaleNames.add (juce::String::fromUTF8 (s.name));

    return {
        std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gate", 1 },
            "Puerta", juce::NormalisableRange<float> (-70.0f, -10.0f, 0.5f), -40.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                [] (float v, int) { return juce::String (v, 1) + " dB"; })),
        std::make_unique<juce::AudioParameterInt>    (juce::ParameterID { "octave", 1 }, "Octava", -2, 2, 0),
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "root", 1 }, juce::String::fromUTF8 ("Tónica"), kNoteNames, 0),
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "scale", 1 }, "Escala", scaleNames, 0),
        std::make_unique<juce::AudioParameterBool>   (juce::ParameterID { "synth", 1 }, "Synth", true),
        std::make_unique<juce::AudioParameterFloat>  (juce::ParameterID { "synthVol", 1 }, "Volumen",
            juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                [] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; })),
    };
}

HumAudioProcessor::HumAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pGate     = apvts.getRawParameterValue ("gate");
    pOctave   = apvts.getRawParameterValue ("octave");
    pRoot     = apvts.getRawParameterValue ("root");
    pScale    = apvts.getRawParameterValue ("scale");
    pSynth    = apvts.getRawParameterValue ("synth");
    pSynthVol = apvts.getRawParameterValue ("synthVol");
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
    const int win = sampleRate > 50000.0 ? 4096 : 2048;  // ~46 ms de ventana sea cual sea el sample rate
    hopSize = win / 2;                                     // analizamos cada ~23 ms
    window.assign ((size_t) win, 0.0f);
    hopBuffer.assign ((size_t) hopSize, 0.0f);
    hopFill = 0;
    hopSumSq = 0.0;
    yin.prepare (sampleRate, win);
    synth.prepare (sampleRate);
    tracker.reset();
    currentNote = -1;
}

// ---------------------------------------------------------------- Audio
void HumAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();

    // Tempo del proyecto: lo usamos al exportar para que el MIDI cuadre con tu BPM.
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
                hostBpm = *bpm;

    // Parámetros: se leen una vez por bloque.
    gateLinear = juce::Decibels::decibelsToGain (pGate->load());
    tracker.octave = (int) pOctave->load();
    tracker.root   = (int) pRoot->load();
    const auto& scale = hum::kScales[juce::jlimit (0, hum::kNumScales - 1, (int) pScale->load())];
    tracker.steps     = scale.steps;
    tracker.stepCount = scale.count;
    const bool  synthOn  = pSynth->load() > 0.5f;
    const float synthVol = pSynthVol->load();

    handleRecordState();

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

        // 2. Llenamos el bloque de análisis; cuando está lleno, analizamos.
        hopBuffer[(size_t) hopFill++] = s;
        hopSumSq += (double) s * s;
        if (hopFill == hopSize)
            analyseHop (i, midi);

        // 3. La salida es el synth interno (o silencio). Tu voz no pasa: solo oyes la nota detectada.
        const float synthSample = synth.next();
        const float out = synthOn ? synthSample * synthVol : 0.0f;
        for (int ch = 0; ch < numOut; ++ch)
            buffer.setSample (ch, i, out);
    }

    sampleClock += numSamples;
    if (recState.load() == Recording)
        recordedLength = sampleClock - recordStart;
}

void HumAudioProcessor::analyseHop (int sampleOffset, juce::MidiBuffer& midi)
{
    const float rms = (float) std::sqrt (hopSumSq / hopSize);
    hopSumSq = 0.0;
    hopFill = 0;

    // Desplazamos la ventana y metemos el bloque nuevo al final.
    std::copy (window.begin() + hopSize, window.end(), window.begin());
    std::copy (hopBuffer.begin(), hopBuffer.end(), window.end() - hopSize);

    inputLevelDb = juce::Decibels::gainToDecibels (rms, -100.0f);

    // Puerta de volumen -> YIN -> nota MIDI (con decimales).
    bool voiced = false;
    float midiNote = 0.0f;
    if (rms >= gateLinear)
    {
        const float hz = yin.detect (window.data());
        if (hz > 0.0f) { midiNote = hum::hzToMidi (hz); voiced = true; }
    }

    // Segmentación -> eventos.
    hum::NoteEvent events[2];
    const int n = tracker.update (midiNote, voiced, hum::rmsToVelocity (rms, gateLinear), events);
    for (int k = 0; k < n; ++k)
        emit (events[k], sampleOffset, midi);

    currentNote = tracker.current;
}

void HumAudioProcessor::emit (const hum::NoteEvent& e, int sampleOffset, juce::MidiBuffer& midi)
{
    if (e.on)
    {
        midi.addEvent (juce::MidiMessage::noteOn (1, e.note, (juce::uint8) e.velocity), sampleOffset);
        synth.noteOn (e.note);
    }
    else
    {
        midi.addEvent (juce::MidiMessage::noteOff (1, e.note), sampleOffset);
        synth.noteOff();
    }

    if (recState.load() == Recording)
    {
        // La nota se confirma `stable` bloques después de empezar a cantarla (y se corta
        // `release` bloques después de parar), y la ventana de YIN mira un bloque hacia atrás.
        // Restamos ese retraso para que la toma caiga donde la cantaste, no donde se detectó.
        const juce::int64 latency = (juce::int64) ((e.on ? tracker.stable : tracker.release) + 1) * hopSize;
        const juce::int64 pos = sampleClock + sampleOffset - recordStart - latency;
        pushRecEvent ({ e.on, e.note, e.velocity, juce::jmax ((juce::int64) 0, pos) });
    }
}

// ---------------------------------------------------------------- Grabación
// La UI pide arrancar/parar; el hilo de audio lo ejecuta al principio del siguiente bloque
// para que el instante exacto (recordStart) salga de su propio reloj de muestras.
void HumAudioProcessor::handleRecordState()
{
    int state = recState.load();
    if (state == StartRequested && recState.compare_exchange_strong (state, Recording))
    {
        recordStart = sampleClock;
        recordedLength = 0;
        if (tracker.current >= 0)  // ya estabas cantando una nota al darle a grabar
            pushRecEvent ({ true, tracker.current, 100, 0 });
    }
    else if (state == StopRequested && recState.compare_exchange_strong (state, Idle))
    {
        recordedLength = sampleClock - recordStart;
        if (tracker.current >= 0)
            pushRecEvent ({ false, tracker.current, 0, recordedLength.load() });
    }
}

void HumAudioProcessor::pushRecEvent (const RecEvent& e)
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 > 0)      fifoData[(size_t) scope.startIndex1] = e;
    else if (scope.blockSize2 > 0) fifoData[(size_t) scope.startIndex2] = e;
    // si la cola estuviese llena, el evento se pierde (con 1024 huecos y 20 vaciados/s no pasa)
}

void HumAudioProcessor::drainFifo()
{
    const int ready = fifo.getNumReady();
    if (ready == 0)
        return;
    const auto scope = fifo.read (ready);
    scope.forEach ([this] (int index)
    {
        const auto& e = fifoData[(size_t) index];
        take.push_back (e);
        if (e.on) ++recordedNoteCount;
    });
}

void HumAudioProcessor::startRecording()
{
    drainFifo();
    take.clear();
    recordedNoteCount = 0;
    recState = StartRequested;
}

void HumAudioProcessor::stopRecording()
{
    int state = recState.load();
    if (state == Recording || state == StartRequested)
        recState = StopRequested;
}

bool HumAudioProcessor::isRecording() const
{
    const int state = recState.load();
    return state == StartRequested || state == Recording;
}

// Convierte la toma en un archivo .mid temporal listo para arrastrar a FL.
juce::File HumAudioProcessor::writeTakeToMidiFile (bool quantize)
{
    drainFifo();
    if (take.empty())
        return {};

    // 1. Emparejamos note on / note off en notas con inicio y fin (en ticks MIDI).
    constexpr int ppq = 960;  // ticks por negra
    const double bpm = hostBpm.load();
    const auto toTicks = [&] (juce::int64 samples) { return (double) samples / sr * bpm / 60.0 * ppq; };

    struct Note { int note, velocity; double start, end; };
    std::vector<Note> notes;
    int openNote = -1, openVel = 0;
    double openStart = 0.0;

    for (const auto& e : take)
    {
        const double t = toTicks (e.samplePos);
        if (e.on)
        {
            if (openNote >= 0) notes.push_back ({ openNote, openVel, openStart, t });
            openNote = e.note; openVel = e.velocity; openStart = t;
        }
        else if (e.note == openNote)
        {
            notes.push_back ({ openNote, openVel, openStart, t });
            openNote = -1;
        }
    }
    if (openNote >= 0)  // nota que seguía sonando al parar
        notes.push_back ({ openNote, openVel, openStart, toTicks (recordedLength.load()) });

    // 2. Cuantización opcional a semicorcheas (1/16 = un cuarto de negra).
    if (quantize)
    {
        constexpr double q = ppq / 4.0;
        for (auto& n : notes)
        {
            n.start = std::round (n.start / q) * q;
            n.end   = std::max (n.start + q, std::round (n.end / q) * q);
        }
    }

    // 3. Monofónico: ninguna nota puede pisar a la siguiente. Si dos caen en el mismo
    //    sitio al cuantizar, nos quedamos con la segunda.
    std::vector<Note> clean;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        auto n = notes[i];
        if (i + 1 < notes.size())
        {
            if (notes[i + 1].start <= n.start) continue;
            n.end = std::min (n.end, notes[i + 1].start);
        }
        if (n.end > n.start) clean.push_back (n);
    }

    // 4. Escribimos el archivo MIDI.
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::round (60'000'000.0 / bpm)), 0.0);
    for (const auto& n : clean)
    {
        seq.addEvent (juce::MidiMessage::noteOn (1, n.note, (juce::uint8) n.velocity), n.start);
        seq.addEvent (juce::MidiMessage::noteOff (1, n.note), n.end);
    }
    seq.updateMatchedPairs();

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote (ppq);
    midiFile.addTrack (seq);

    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("Tarareo_" + juce::Time::getCurrentTime().formatted ("%H%M%S") + ".mid");
    file.deleteFile();
    juce::FileOutputStream os (file);
    if (! os.openedOk() || ! midiFile.writeTo (os))
        return {};
    os.flush();
    return file;
}

// ---------------------------------------------------------------- Estado / editor
void HumAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void HumAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* HumAudioProcessor::createEditor() { return new HumAudioProcessorEditor (*this); }

// Punto de entrada que FL Studio busca dentro del .vst3
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HumAudioProcessor(); }
