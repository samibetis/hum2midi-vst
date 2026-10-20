#pragma once
// Núcleo DSP de hum2midi. C++ puro, sin JUCE: es la misma lógica que el script de Python,
// traducida casi línea por línea. Así se puede probar fuera del DAW.

#include <algorithm>
#include <cmath>
#include <vector>

namespace hum
{

inline float hzToMidi (float hz) { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }

// ---------------------------------------------------------------- Escalas
struct ScaleDef { const char* name; int count; int steps[7]; };

// count = 0 -> cromática (sin ajuste a escala). Los nombres van en UTF-8.
inline constexpr ScaleDef kScales[] = {
    { "Cromática",   0, {} },
    { "Mayor",       7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "Menor",       7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "Pentatónica", 5, { 0, 2, 4, 7, 9 } },
    { "Penta menor", 5, { 0, 3, 5, 7, 10 } },
    { "Blues",       6, { 0, 3, 5, 6, 7, 10 } },
};
inline constexpr int kNumScales = (int) (sizeof (kScales) / sizeof (kScales[0]));

// Redondea a la nota más cercana; si hay escala, solo a notas de esa escala.
inline int snap (float m, int root, const int* steps, int count)
{
    if (count == 0)
        return (int) std::lround (m);

    const int octave = (int) std::floor (m / 12.0f);
    int best = 0;
    float bestDist = 1e9f;
    for (int o = octave - 1; o <= octave + 1; ++o)
        for (int i = 0; i < count; ++i)
        {
            const int c = o * 12 + root + steps[i];
            const float d = std::abs ((float) c - m);
            if (d < bestDist) { bestDist = d; best = c; }
        }
    return best;
}

// Volumen -> velocity. Justo en la puerta = 40; 30 dB por encima = 127.
inline int rmsToVelocity (float rms, float gate)
{
    const float dbOverGate = 20.0f * std::log10 ((rms + 1e-12f) / gate);
    return (int) std::clamp (40.0f + dbOverGate * (87.0f / 30.0f), 40.0f, 127.0f);
}

// ---------------------------------------------------------------- Detección de tono (YIN)
// Una señal periódica se parece a sí misma desplazada un periodo. Probamos desplazamientos
// (tau), medimos cuánto difiere la señal de su copia desplazada, y el primer tau con
// diferencia casi cero es el periodo -> frecuencia = sampleRate / tau.
class Yin
{
public:
    void prepare (double sampleRate, int windowSize, float fmin = 70.0f, float fmax = 1000.0f)
    {
        sr = sampleRate;
        n = windowSize;
        tauMin = std::max (2, (int) (sr / fmax));
        tauMax = std::min ((int) (sr / fmin), n / 2);
        w = n - tauMax;
        cmndf.assign ((size_t) tauMax + 1, 1.0f);  // se reserva aquí, nunca en el hilo de audio
    }

    // Devuelve la frecuencia en Hz, o -1 si no hay un tono claro.
    float detect (const float* x, float threshold = 0.15f)
    {
        // 1+2) Función diferencia d(tau) y su normalización acumulada (CMNDF), en una pasada.
        //      En C++ el bucle directo ya es rápido de sobra; en Python hacía falta la FFT.
        double running = 0.0;
        cmndf[0] = 1.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double d = 0.0;
            for (int j = 0; j < w; ++j)
            {
                const float diff = x[j] - x[j + tau];
                d += (double) diff * diff;
            }
            running += d;
            cmndf[(size_t) tau] = running > 1e-12 ? (float) (d * tau / running) : 1.0f;
        }

        // 3) Primer valle por debajo del umbral (el primero, no el más profundo: evita errores de octava).
        int tau = -1;
        for (int t = tauMin; t < tauMax; ++t)
            if (cmndf[(size_t) t] < threshold) { tau = t; break; }
        if (tau < 0)
            return -1.0f;
        while (tau + 1 < tauMax && cmndf[(size_t) tau + 1] < cmndf[(size_t) tau])
            ++tau;  // bajamos hasta el fondo del valle

        // 4) Interpolación parabólica: afina el periodo entre muestras.
        const float a = cmndf[(size_t) tau - 1], b = cmndf[(size_t) tau], c = cmndf[(size_t) tau + 1];
        const float denom = a - 2.0f * b + c;
        const float shift = std::abs (denom) > 1e-9f ? 0.5f * (a - c) / denom : 0.0f;
        return (float) (sr / (tau + shift));
    }

private:
    double sr = 44100.0;
    int n = 2048, tauMin = 44, tauMax = 630, w = 1418;
    std::vector<float> cmndf;
};

// ---------------------------------------------------------------- Segmentación de notas
struct NoteEvent { bool on; int note; int velocity; };

// Convierte un tono por frame en eventos note on / note off.
//  - Una nota nueva solo se confirma si aguanta `stable` frames seguidos.
//  - Mientras suena una nota se tolera desviarse `tolerance` semitonos (vibrato).
//  - Histéresis: solo se cambia si la nueva está claramente más cerca (0.5 st).
//  - El silencio tiene que durar `release` frames para cortar la nota.
class NoteTracker
{
public:
    int stable = 3, release = 3;
    float tolerance = 0.7f;
    int octave = 0, root = 0;
    const int* steps = nullptr;
    int stepCount = 0;
    int current = -1;  // nota que suena, -1 = ninguna

    void reset() { current = candidate = -1; count = silence = 0; }

    // Devuelve cuántos eventos ha escrito en `out` (0, 1 o 2).
    int update (float midi, bool voiced, int velocity, NoteEvent out[2])
    {
        int numEvents = 0;

        if (! voiced)
        {
            candidate = -1; count = 0;
            if (current >= 0 && ++silence >= release)
            {
                out[numEvents++] = { false, current, 0 };
                current = -1;
            }
            return numEvents;
        }

        silence = 0;
        midi += 12.0f * (float) octave;

        // Vibrato: si sigues cerca de la nota actual, no pasa nada.
        if (current >= 0 && std::abs (midi - (float) current) < tolerance)
        {
            candidate = -1; count = 0;
            return numEvents;
        }

        const int note = std::clamp (snap (midi, root, steps, stepCount), 0, 127);
        if (note == current
            || (current >= 0 && std::abs (midi - (float) current) - std::abs (midi - (float) note) < 0.5f))
        {
            candidate = -1; count = 0;
            return numEvents;
        }

        if (note == candidate) ++count;
        else { candidate = note; count = 1; }

        if (count >= stable)
        {
            if (current >= 0)
                out[numEvents++] = { false, current, 0 };
            out[numEvents++] = { true, note, velocity };
            current = note;
            candidate = -1; count = 0;
        }
        return numEvents;
    }

private:
    int candidate = -1, count = 0, silence = 0;
};

// ---------------------------------------------------------------- Synth interno
// Monofónico y simple: seno + un poco de tercer armónico, con envolvente suave para no
// hacer clics. Solo sirve para que oigas lo que se detecta sin enrutar MIDI en FL.
class MonoSynth
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        attackCoeff  = 1.0 - std::exp (-1.0 / (0.005 * sr));  // 5 ms
        releaseCoeff = 1.0 - std::exp (-1.0 / (0.080 * sr));  // 80 ms
        env = phase = 0.0;
        gate = false;
    }

    void noteOn (int note) { freq = 440.0 * std::pow (2.0, (note - 69) / 12.0); gate = true; }
    void noteOff()         { gate = false; }

    float next()
    {
        env += ((gate ? 1.0 : 0.0) - env) * (gate ? attackCoeff : releaseCoeff);
        phase += freq / sr;
        if (phase >= 1.0) phase -= 1.0;
        constexpr double twoPi = 6.283185307179586;
        const double s = std::sin (twoPi * phase) + 0.25 * std::sin (3.0 * twoPi * phase);
        return (float) (0.5 * s * env);
    }

private:
    double sr = 44100.0, freq = 440.0, phase = 0.0, env = 0.0;
    double attackCoeff = 0.0, releaseCoeff = 0.0;
    bool gate = false;
};

} // namespace hum
