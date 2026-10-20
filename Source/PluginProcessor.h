#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "HumDsp.h"

// El "motor" del plugin. FL Studio llama a processBlock() cientos de veces por segundo con
// trozos de audio del micro; aquí detectamos notas, sacamos MIDI, tocamos el synth interno
// y, si estás grabando, apuntamos las notas para luego arrastrarlas al piano roll.
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

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Estado para la interfaz (se escribe en el hilo de audio, se lee en el de la UI).
    std::atomic<int>   currentNote  { -1 };
    std::atomic<float> inputLevelDb { -100.0f };
    std::atomic<float> tuneCents { 0.0f };  // compensación de la afinación automática

    // Grabación. Llamar solo desde el hilo de la UI.
    void startRecording();
    void stopRecording();
    bool isRecording() const;
    int  getRecordedNoteCount() const { return recordedNoteCount; }
    juce::File writeTakeToMidiFile (bool quantize);  // devuelve {} si no hay nada grabado

private:
    struct RecEvent { bool on; int note; int velocity; juce::int64 samplePos; bool replace; };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void handleRecordState();
    void emit (const hum::Detector::Event& e, int sampleOffset, juce::MidiBuffer& midi);
    void pushRecEvent (const RecEvent& e);
    void drainFifo();
    void timerCallback() override { drainFifo(); }

    // --- DSP (solo hilo de audio)
    hum::Detector detector;
    hum::MonoSynth synth;
    double sr = 44100.0;
    juce::int64 sampleClock = 0;           // muestras procesadas desde que arrancó

    // --- Grabación
    // El hilo de audio no puede reservar memoria ni bloquearse (daría cortes de audio), así
    // que mete los eventos en una cola sin bloqueos (AbstractFifo) y un Timer en el hilo de
    // la UI los vacía en `take`, que sí puede crecer libremente.
    enum RecState { Idle, StartRequested, Recording, StopRequested };
    std::atomic<int> recState { Idle };
    juce::int64 recordStart = 0;
    std::atomic<juce::int64> recordedLength { 0 };
    std::atomic<double> hostBpm { 120.0 };
    juce::AbstractFifo fifo { 1024 };
    std::array<RecEvent, 1024> fifoData {};
    std::vector<RecEvent> take;            // solo hilo de la UI
    int recordedNoteCount = 0;

    // --- Parámetros (punteros a sus valores: lectura sin bloqueos desde el audio)
    std::atomic<float>* pGate = nullptr;
    std::atomic<float>* pOctave = nullptr;
    std::atomic<float>* pRoot = nullptr;
    std::atomic<float>* pScale = nullptr;
    std::atomic<float>* pSynth = nullptr;
    std::atomic<float>* pSynthVol = nullptr;
    std::atomic<float>* pAutoTune = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HumAudioProcessor)
};
