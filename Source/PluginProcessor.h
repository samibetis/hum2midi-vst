#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "HumDsp.h"

// El "motor" del plugin. FL Studio llama a processBlock() cientos de veces por segundo con
// trozos de audio del micro; aquí detectamos notas, sacamos MIDI (con acordes y pitch bend si
// están activos), tocamos el synth interno y, si estás grabando, apuntamos las notas en una
// toma que luego puedes escuchar, guardar o arrastrar al piano roll.
class HumAudioProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    HumAudioProcessor();
    ~HumAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // Guarda parámetros Y tomas dentro del proyecto de FL.
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // ---- Estado para la interfaz (lo escribe el hilo de audio, lo lee la UI)
    std::atomic<int>   currentNote  { -1 };
    std::atomic<float> inputLevelDb { -100.0f };
    std::atomic<float> tuneCents { 0.0f };       // compensación de la afinación automática
    std::atomic<bool>  hostPlaying { false };
    std::atomic<double> hostBpm { 120.0 };

    // Un frame de análisis (cada 8 ms) para dibujar el pitch roll.
    // beat: 0 = nada, 1 = empieza un tiempo, 2 = empieza un compás (solo con FL en marcha).
    struct PitchFrame { float midi; short note; bool voiced; bool recording; juce::uint8 beat; };
    int popPitchFrames (PitchFrame* dest, int maxFrames);  // solo desde el hilo de la UI

    // ---- Grabación (llamar desde el hilo de la UI)
    void startRecording();   // con "Grabar al dar Play" queda armada hasta que FL arranque
    void stopRecording();
    bool isRecording() const;
    bool isArmed() const;
    int  getLiveNoteCount() const { return liveNoteCount; }

    // ---- Tomas (hilo de la UI)
    struct TakeInfo
    {
        juce::String name;
        int notes = 0;
        double seconds = 0.0;
        int bar = 0;                         // compás de FL donde empezó (0 = sin sincronizar)
        std::vector<hum::TakeNote> preview;  // melodía ya procesada, en ticks (960 por negra)
    };
    int  getNumTakes() const;
    TakeInfo getTakeInfo (int index) const;
    void deleteTake (int index);
    std::atomic<int> takesVersion { 0 };     // sube cada vez que cambia la lista de tomas
    int selectedTake = -1;

    juce::File writeTakeFile (int index);    // .mid temporal con los ajustes actuales
    bool saveTakeTo (int index, const juce::File& destination);

    // Escuchar una toma con el synth interno
    void playTake (int index);
    void stopPreview();
    int  getPreviewTake() const { return previewTake; }
    std::atomic<float> previewProgress { 0.0f };

    // Tonalidad a partir de una toma (index >= 0) o de lo último que has cantado (-1)
    hum::KeyResult detectKey (int index) const;

private:
    struct RecEvent { bool on; int note; int velocity; juce::int64 samplePos; bool replace; };
    struct Take
    {
        std::vector<RecEvent> events;
        juce::int64 length = 0;   // muestras
        double sampleRate = 44100.0, bpm = 120.0;
        double offsetQuarters = 0.0;  // desde el inicio del compás hasta que empezó a grabar
        int bar = 0;
        juce::String name;
    };
    struct PlayEvent { juce::int64 pos; short note; bool on; };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void readHostPosition();
    void handleRecordState();
    void emit (const hum::Detector::Event& e, int sampleOffset, juce::MidiBuffer& midi);
    void pushRecEvent (const RecEvent& e);
    void pushPitchFrame (int sampleOffset, juce::MidiBuffer& midi);
    void sendBend (float semitones, int sampleOffset, juce::MidiBuffer& midi);
    void processPreview();
    void drainFifo();
    void finishTake();
    void timerCallback() override;
    std::vector<hum::TakeNote> renderTake (const Take& t, bool withChords) const;
    juce::File writeMidi (const std::vector<hum::TakeNote>& notes, double bpm, const juce::String& name);
    hum::ChordType chordType() const;

    // --- DSP (solo hilo de audio)
    hum::Detector detector;
    hum::PolySynth synth;
    double sr = 44100.0;
    juce::int64 sampleClock = 0;           // muestras procesadas desde que arrancó

    // Acordes y pitch bend en vivo
    int chordNotes[4] {}, chordCount = 0;
    int lastBend = 8192;
    bool bendWasOn = false;

    // Posición de FL (leída al principio de cada bloque)
    bool blockPlaying = false, blockHasPpq = false;
    double blockPpq = 0.0, blockBarPpq = 0.0, blockBpm = 120.0;
    int blockTimeSig = 4;
    int blockStartSample = 0;
    double lastBeatIndex = -1.0;
    juce::uint8 pendingBeat = 0;

    // Histograma de las notas que vas cantando (para detectar la tonalidad sin grabar)
    std::array<std::atomic<float>, 12> pcHistory {};

    // --- Grabación
    // El hilo de audio no puede reservar memoria ni bloquearse (daría cortes de audio), así
    // que mete los eventos en una cola sin bloqueos y un Timer en el hilo de la UI los vacía.
    enum RecState { Idle, StartRequested, Armed, Recording, StopRequested };
    std::atomic<int> recState { Idle };
    bool recSynced = false;                // empezó al dar Play: para al parar FL
    juce::int64 recordStart = 0;
    std::atomic<juce::int64> recordedLength { 0 };
    std::atomic<double> recOffsetQuarters { 0.0 }, recBpm { 120.0 };
    std::atomic<int> recBar { 0 }, recStops { 0 };
    int seenStops = 0;
    juce::AbstractFifo fifo { 2048 };
    std::array<RecEvent, 2048> fifoData {};

    // Tomas (solo hilo de la UI, protegidas por takesLock frente a get/setStateInformation)
    juce::CriticalSection takesLock;
    std::vector<Take> takes;
    Take current;
    int liveNoteCount = 0, takeCounter = 0, fileCounter = 0;
    static constexpr int kMaxTakes = 12;

    // Reproducción de una toma: la UI rellena playData solo cuando el audio la ha soltado
    // (playState == 0); el audio la recorre sin bloqueos.
    enum PlayState { PlayStopped = 0, PlayStart, Playing, PlayStop };
    std::atomic<int> playState { PlayStopped };
    std::array<PlayEvent, 4096> playData {};
    int playCount = 0, playIndex = 0;
    juce::int64 playPos = 0, playLength = 1;
    int previewTake = -1, pendingPreview = -1;

    // Cola sin bloqueos audio -> UI con el tono de cada frame (para el pitch roll).
    juce::AbstractFifo pitchFifo { 2048 };
    std::array<PitchFrame, 2048> pitchData {};
    int lastFrameCount = 0;

    // --- Parámetros (punteros a sus valores: lectura sin bloqueos desde el audio)
    std::atomic<float>* pGate = nullptr;
    std::atomic<float>* pOctave = nullptr;
    std::atomic<float>* pRoot = nullptr;
    std::atomic<float>* pScale = nullptr;
    std::atomic<float>* pSynth = nullptr;
    std::atomic<float>* pSynthVol = nullptr;
    std::atomic<float>* pAutoTune = nullptr;
    std::atomic<float>* pDetail = nullptr;
    std::atomic<float>* pChords = nullptr;
    std::atomic<float>* pBend = nullptr;
    std::atomic<float>* pBendRange = nullptr;
    std::atomic<float>* pVelFixed = nullptr;
    std::atomic<float>* pVoiceMix = nullptr;
    std::atomic<float>* pGrid = nullptr;
    std::atomic<float>* pStrength = nullptr;
    std::atomic<float>* pLegato = nullptr;
    std::atomic<float>* pSyncPlay = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumAudioProcessor)
};
