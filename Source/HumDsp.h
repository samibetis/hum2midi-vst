#pragma once
// Núcleo DSP de hum2midi. C++ puro, sin JUCE, para poder probarlo fuera del DAW.
//
// Cadena completa (clase Detector, al final del archivo):
//   micro -> paso alto 50 Hz      (quita rumble, golpes en la mesa y DC)
//         -> paso bajo 2 kHz      (quita soplido y armónicos altos que confunden a YIN)
//         -> diezmado a ~22 kHz   (YIN es mucho más barato y podemos analizar cada 8 ms)
//         -> YIN + corrección de octava
//         -> filtro de mediana    (quita saltos sueltos de un frame)
//         -> NoteTracker          (decide cuándo empieza/acaba/cambia una nota)
//         -> eventos con su instante real (para que la grabación caiga donde cantaste)

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

// Volumen por encima de la puerta -> velocity. Justo en la puerta = 40; +30 dB = 127.
inline int dbToVelocity (float dbOverGate)
{
    return (int) std::clamp (40.0f + dbOverGate * (87.0f / 30.0f), 40.0f, 127.0f);
}

// ---------------------------------------------------------------- Filtros
// Biquad clásico (fórmulas de RBJ). Dos en serie con Q 0.54 y 1.31 = Butterworth de 4º orden.
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;

    static Biquad make (double sr, double freq, double q, bool highpass)
    {
        const double w0 = 2.0 * 3.141592653589793 * freq / sr;
        const double cs = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        const double k  = highpass ? (1.0 + cs) : (1.0 - cs);
        Biquad f;
        f.b0 = (float) (0.5 * k / a0);
        f.b1 = (float) ((highpass ? -k : k) / a0);
        f.b2 = f.b0;
        f.a1 = (float) (-2.0 * cs / a0);
        f.a2 = (float) ((1.0 - alpha) / a0);
        return f;
    }

    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ---------------------------------------------------------------- Detección de tono (YIN)
// Una señal periódica se parece a sí misma desplazada un periodo. Probamos desplazamientos
// (tau), medimos cuánto difiere la señal de su copia desplazada, y el primer tau con
// diferencia casi cero es el periodo -> frecuencia = sampleRate / tau.
struct Pitch { float hz = -1.0f; float aperiodicity = 1.0f; };  // aperiodicity: 0 = tono puro, 1 = ruido

class Yin
{
public:
    float threshold = 0.2f;

    // integration = nº de muestras que se comparan en cada desplazamiento (~25 ms)
    void prepare (double sampleRate, float fmin, float fmax, int integration)
    {
        sr = sampleRate;
        tauMin = std::max (2, (int) (sr / fmax));
        tauMax = (int) (sr / fmin) + 1;
        w = integration;
        n = w + tauMax + 2;
        cmndf.assign ((size_t) tauMax + 2, 1.0f);  // se reserva aquí, nunca en el hilo de audio
    }

    int windowSize() const { return n; }

    Pitch detect (const float* x)
    {
        // 1+2) Función diferencia d(tau) y su normalización acumulada (CMNDF), en una pasada.
        double running = 0.0;
        cmndf[0] = 1.0f;
        for (int tau = 1; tau <= tauMax + 1; ++tau)
        {
            const float* y = x + tau;
            float d = 0.0f;
            for (int j = 0; j < w; ++j)
            {
                const float diff = x[j] - y[j];
                d += diff * diff;
            }
            running += d;
            cmndf[(size_t) tau] = running > 1e-20 ? (float) (d * tau / running) : 1.0f;
        }

        // 3) Primer valle por debajo del umbral (el primero, no el más profundo: evita
        //    errores de octava hacia abajo). Si ninguno baja del umbral, el mínimo global;
        //    quien llama decide con `aperiodicity` si eso es voz o ruido.
        int tau = -1;
        for (int t = tauMin; t < tauMax; ++t)
            if (cmndf[(size_t) t] < threshold) { tau = t; break; }
        if (tau < 0)
        {
            tau = tauMin;
            for (int t = tauMin + 1; t < tauMax; ++t)
                if (cmndf[(size_t) t] < cmndf[(size_t) tau]) tau = t;
        }
        while (tau + 1 < tauMax && cmndf[(size_t) tau + 1] < cmndf[(size_t) tau])
            ++tau;

        // 4) Anti-octava hacia arriba: con vocales tipo "aaa" el 2º armónico puede pesar más
        //    que la fundamental y el primer valle cae en la MITAD del periodo real. Si en el
        //    doble de tau hay un valle claramente mejor, ese es el periodo de verdad.
        if (cmndf[(size_t) tau] > 0.04f)
        {
            const int t2 = 2 * tau, span = std::max (2, t2 / 30);
            if (t2 + span < tauMax)
            {
                int best = t2 - span;
                for (int t = t2 - span + 1; t <= t2 + span; ++t)
                    if (cmndf[(size_t) t] < cmndf[(size_t) best]) best = t;
                if (cmndf[(size_t) best] < 0.5f * cmndf[(size_t) tau])
                    tau = best;
            }
        }

        // 5) Interpolación parabólica: afina el periodo entre muestras.
        const float a = cmndf[(size_t) tau - 1], b = cmndf[(size_t) tau], c = cmndf[(size_t) tau + 1];
        const float denom = a - 2.0f * b + c;
        const float shift = std::abs (denom) > 1e-9f ? std::clamp (0.5f * (a - c) / denom, -1.0f, 1.0f) : 0.0f;
        const float ap = std::clamp (b - 0.25f * (a - c) * shift, 0.0f, 1.0f);
        return { (float) (sr / (tau + shift)), ap };
    }

private:
    double sr = 22050.0;
    int tauMin = 22, tauMax = 368, w = 551, n = 921;
    std::vector<float> cmndf;
};

// Mediana de los últimos 5 valores: un frame suelto con la octava mal no mueve el resultado.
class Median5
{
public:
    void reset() { count = pos = 0; }
    void push (float x) { v[pos] = x; pos = (pos + 1) % 5; count = std::min (count + 1, 5); }
    float get() const
    {
        float t[5];
        std::copy (v, v + count, t);
        std::sort (t, t + count);
        return t[(count - 1) / 2];
    }

private:
    float v[5] {};
    int count = 0, pos = 0;
};

// ---------------------------------------------------------------- Segmentación de notas
// framesAgo: cuántos frames atrás empezó/acabó de verdad el evento. El MIDI en vivo sale
// en cuanto se confirma, pero la grabación usa esto para colocar la nota donde la cantaste.
// replace = esta nota sustituye a la anterior (era un scoop mal confirmado): en la
// grabación se borra la anterior y esta empieza donde empezaba aquella.
struct NoteEvent { bool on; int note; int velocity; int framesAgo; bool replace = false; };

// Convierte un tono por frame en eventos note on / note off.
//  - Estabilidad: una nota se confirma cuando los últimos ~40-48 ms tienen un tono
//    parecido (cabe el vibrato) y SIN tendencia (no sigue subiendo/bajando). Así un scoop
//    o un glissando no generan notas fantasma por el camino.
//  - La nota es el redondeo de la MEDIA de esos frames: cantar algo desafinado no parpadea.
//  - Histéresis: la nota actual aguanta mientras sigas a menos de 0.7 st de ella.
//  - Tiempo real: el inicio se coloca donde el tono cruzó a la nota nueva, o donde
//    empezaste a cantar si entraste con scoop.
//  - Re-ataque: un bajón de volumen de 9 dB seguido de una subida ("da-da-da") corta la
//    nota y empieza otra, aunque sea la misma nota y no haya silencio entre medias.
//  - Silencio de ~56 ms corta la nota (respirar, pausas).
class NoteTracker
{
public:
    int octave = 0, root = 0;
    const int* steps = nullptr;
    int stepCount = 0;
    int current = -1;  // nota que suena, -1 = ninguna

    // Afinación automática: aprende cuánto te desvías de media del La 440 (p. ej. si cantas
    // todo 40 cents bajo) y lo compensa. Media circular: -0.45 y +0.55 son lo mismo.
    bool autoTune = true;
    float tuneOffset() const { return autoTune ? offset : 0.0f; }

    // windowFrames: cuántos frames ocupa la ventana de análisis (lo que "arrastra" el pasado)
    void configure (double frameRate, int windowFrames)
    {
        const auto frames = [frameRate] (double ms) { return std::max (1, (int) std::lround (ms * 0.001 * frameRate)); };
        onsetFrames   = std::min (frames (40), kHist / 2);
        changeFrames  = std::min (frames (48), kHist / 2);
        releaseFrames = frames (56);
        scoopFrames   = frames (160);
        correctFrames = frames (100);
        reviewFrames  = frames (300);
        reviewMinFrames = frames (48);
        dipMaxFrames  = frames (250);
        settleFrames  = std::max (1, (windowFrames + 1) / 2);
        peakLeakDb    = (float) (10.0 / frameRate);  // el pico de referencia baja 10 dB/s
        tuneAlpha     = (float) (1.0 / (1.5 * frameRate));  // se adapta en ~1.5 s de notas estables
    }

    void reset()
    {
        current = -1;
        frame = 0;
        unvoiced = 0;
        ignoreUntil = 0;
        voicedRun = inDip = false;
        clearHistory();
    }

    // Devuelve cuántos eventos ha escrito en `out` (0, 1 o 2).
    int update (bool voiced, float midi, float levelDb, int velocity, NoteEvent out[2])
    {
        ++frame;
        int n = 0;

        // ---- Re-ataque: bajón de energía y subida mientras suena una nota
        if (current >= 0)
        {
            notePeakDb = std::max (levelDb, notePeakDb - peakLeakDb);
            if (! inDip)
            {
                if (levelDb < notePeakDb - dipDb) { inDip = true; dipMinDb = levelDb; dipFrame = dipStart = frame; }
            }
            else
            {
                if (levelDb < dipMinDb) { dipMinDb = levelDb; dipFrame = frame; }
                if (frame - dipStart > dipMaxFrames)
                {
                    inDip = false;           // era un diminuendo largo, no una sílaba nueva
                    notePeakDb = levelDb;
                }
                else if (voiced && levelDb > dipMinDb + riseDb)
                {
                    out[n++] = { false, current, 0, frame - dipFrame };
                    current = -1;
                    inDip = false;
                    clearHistory();
                    voicedRun = true;
                    voicedStart = dipFrame;               // la nota siguiente empieza en el fondo del bajón
                    ignoreUntil = dipFrame + settleFrames; // frames cuya ventana aún "oye" la sílaba anterior
                }
            }
        }

        // ---- Silencio
        if (! voiced)
        {
            if (++unvoiced >= releaseFrames)
            {
                if (current >= 0)
                {
                    out[n++] = { false, current, 0, frame - lastVoiced };
                    current = -1;
                }
                voicedRun = inDip = false;
                clearHistory();
            }
            return n;  // un hueco corto (< releaseFrames) no corta nada
        }

        unvoiced = 0;
        lastVoiced = frame;
        if (! voicedRun) { voicedRun = true; voicedStart = frame; }
        if (frame < ignoreUntil)
            return n;

        median.push (midi);
        const float shift = 12.0f * (float) octave - tuneOffset();
        const float m = median.get() + shift;
        pushHistory (m, midi + shift, velocity);

        // ---- Revisión: en los primeros ~300 ms de una nota comparamos la MEDIA real del tono
        //      con la nota elegida. Si nunca se asentó en su semitono (scoop lento, vibrato
        //      que la pilló en un valle), se corrige a la buena desde el principio.
        if (current >= 0)
        {
            noteSum += m;
            ++noteCount;
            if (! reviewed && frame - confirmFrame <= reviewFrames && noteCount >= reviewMinFrames)
            {
                const float mean = noteSum / (float) noteCount;
                const int target = std::clamp (snap (mean, root, steps, stepCount), 0, 127);
                if (target != current && std::abs (target - current) <= 2 && std::abs (mean - (float) current) > 0.55f
                    && recentlyStable (mean))
                {
                    out[n++] = { false, current, 0, frame - noteStart };
                    out[n++] = { true, target, noteVelocity, frame - noteStart, true };
                    current = target;
                    reviewed = true;
                    return n;
                }
            }
        }

        // ---- ¿Sigues en la nota actual? (vibrato, desafinación leve)
        if (current >= 0 && belongsToCurrent (m))
        {
            if (frame - confirmFrame > reviewMinFrames)  // ya pasado el ataque
                learnTuning (m + tuneOffset() - (float) current);
            return n;
        }

        // ---- ¿Los últimos K frames forman una nota estable?
        const int K = current >= 0 ? changeFrames : onsetFrames;
        if (histCount < K)
            return n;

        float sum = 0.0f, lo = 1e9f, hi = -1e9f, rawSum = 0.0f, cov = 0.0f, var = 0.0f;
        int peakVel = 0;
        for (int i = 0; i < K; ++i)  // i = 0 es el más antiguo de los K
        {
            const auto& h = hist (K - 1 - i);
            if (current >= 0 && belongsToCurrent (h.m))
                return n;  // aún hay frames de la nota anterior: esperar
            sum += h.m;
            rawSum += h.raw;
            lo = std::min (lo, h.m);
            hi = std::max (hi, h.m);
            peakVel = std::max (peakVel, h.velocity);
        }
        // Tendencia = cuánto sube/baja el tono a lo largo de los K frames (recta de regresión
        // sobre el tono SIN mediana: la mediana a medio llenar escondería la subida de un scoop).
        const float iMean = 0.5f * (float) (K - 1), rawMean = rawSum / (float) K;
        for (int i = 0; i < K; ++i)
        {
            const float di = (float) i - iMean;
            cov += di * (hist (K - 1 - i).raw - rawMean);
            var += di * di;
        }
        const float trend = cov / var * (float) (K - 1);
        if (hi - lo > maxSpread || std::abs (trend) > (current >= 0 ? maxTrend : maxOnsetTrend))
            return n;  // se mueve demasiado: scoop, glissando o vibrato muy fuerte. Esperar.

        const float mean = sum / (float) K;
        if (current >= 0 && belongsToCurrent (mean))
            return n;

        const int note = std::clamp (snap (mean, root, steps, stepCount), 0, 127);

        // ---- ¿Dónde empezó de verdad? Recorremos hacia atrás mientras el tono ya estaba
        //      más cerca de la nota nueva que de la anterior (o cerca de ella, si no había).
        int start = frame;
        for (int i = 0; i < histCount; ++i)
        {
            const auto& h = hist (i);
            const bool towardsNew = current >= 0 ? std::abs (h.m - (float) note) < std::abs (h.m - (float) current)
                                                 : std::abs (h.m - (float) note) < 0.75f;
            if (! towardsNew) break;
            start = h.frame;
        }
        start -= medianLag;  // la mediana retrasa ~2 frames

        bool replace = false;
        if (current >= 0)
        {
            // ¿La nota actual acaba de empezar y saltamos a una vecina? Era el final de un
            // scoop confirmado demasiado pronto: la nueva ocupa su lugar desde el principio.
            // La actual nunca se asentó en su semitono (sin contar los K frames de la nueva)
            // y dura poquísimo: era un paso de glissando o el final de un scoop.
            const int before = noteCount - K;
            replace = frame - confirmFrame <= correctFrames && std::abs (note - current) <= 2
                      && (before < 3 || std::abs ((noteSum - sum) / (float) before - (float) current) > 0.35f);
            out[n++] = { false, current, 0, frame - start };
            if (replace)
                start = noteStart;
        }
        else if (start - voicedStart <= scoopFrames)
            start = voicedStart;  // entraste con scoop: la nota empieza cuando empezaste a cantar

        start = std::min (start, frame);
        out[n++] = { true, note, peakVel, frame - start, replace };
        confirmFrame = frame;
        noteStart = start;
        noteSum = 0.0f;
        noteCount = 0;
        noteVelocity = peakVel;
        reviewed = replace;
        current = note;
        inDip = false;
        notePeakDb = levelDb;
        return n;
    }

private:
    struct HistEntry { float m, raw; int frame; int velocity; };  // m = con mediana, raw = sin ella
    static constexpr int kHist = 48;

    void clearHistory() { histCount = histPos = 0; median.reset(); }
    void pushHistory (float m, float raw, int velocity)
    {
        histBuf[histPos] = { m, raw, frame, velocity };
        histPos = (histPos + 1) % kHist;
        histCount = std::min (histCount + 1, kHist);
    }
    void learnTuning (float deviation)
    {
        constexpr float twoPi = 6.2831853f;
        tuneC += tuneAlpha * (std::cos (twoPi * deviation) - tuneC);
        tuneS += tuneAlpha * (std::sin (twoPi * deviation) - tuneS);
        offset = std::atan2 (tuneS, tuneC) / twoPi;
    }

    bool recentlyStable (float centre) const
    {
        for (int i = 0; i < std::min (4, histCount); ++i)
            if (std::abs (hist (i).m - centre) > 0.8f) return false;
        return true;
    }
    const HistEntry& hist (int age) const { return histBuf[(histPos - 1 - age + 2 * kHist) % kHist]; }  // 0 = el más nuevo

    bool belongsToCurrent (float m) const
    {
        const float dCur = std::abs (m - (float) current);
        if (dCur < tolerance)
            return true;
        const int note = snap (m, root, steps, stepCount);
        return note == current || dCur - std::abs (m - (float) note) < hysteresis;
    }

    static constexpr float tolerance = 0.7f, hysteresis = 0.5f, maxSpread = 1.1f, maxTrend = 0.55f, maxOnsetTrend = 0.3f;
    static constexpr float dipDb = 9.0f, riseDb = 6.0f;
    static constexpr int medianLag = 2;

    int onsetFrames = 5, changeFrames = 6, releaseFrames = 7, scoopFrames = 20, dipMaxFrames = 31, settleFrames = 3, correctFrames = 12, reviewFrames = 37, reviewMinFrames = 10;
    float peakLeakDb = 0.08f, tuneAlpha = 0.005f;
    float offset = 0.0f, tuneC = 1.0f, tuneS = 0.0f;

    int frame = 0, unvoiced = 0, lastVoiced = 0, voicedStart = 0, ignoreUntil = 0;
    bool voicedRun = false, reviewed = false;
    int noteStart = 0, confirmFrame = 0, noteCount = 0, noteVelocity = 0;
    float noteSum = 0.0f;
    Median5 median;

    HistEntry histBuf[kHist] {};
    int histCount = 0, histPos = 0;

    bool inDip = false;
    float notePeakDb = -100.0f, dipMinDb = 0.0f;
    int dipFrame = 0, dipStart = 0;
};

// ---------------------------------------------------------------- Detector completo
// Se le pasa el audio muestra a muestra. Devuelve eventos con `samplesAgo`: cuántas muestras
// atrás (respecto a la muestra actual) ocurrió de verdad el evento.
class Detector
{
public:
    struct Event { bool on; int note; int velocity; int samplesAgo; bool replace; };

    NoteTracker tracker;
    float gateDb = -40.0f;
    float voicingThreshold = 0.35f;  // aperiodicidad máxima para considerar que es voz
    float levelDb = -100.0f;         // nivel de entrada (para el medidor)
    float lastMidi = 0.0f;           // último tono detectado (con decimales), para la interfaz
    bool lastVoiced = false;

    void prepare (double sampleRate)
    {
        decim = std::max (1, (int) (sampleRate / 20000.0));
        const double dsr = sampleRate / decim;

        hp  = Biquad::make (sampleRate, 50.0,   0.7071, true);
        lp1 = Biquad::make (sampleRate, 2000.0, 0.5412, false);
        lp2 = Biquad::make (sampleRate, 2000.0, 1.3066, false);

        yin.prepare (dsr, 60.0f, 1000.0f, (int) (dsr * 0.025));
        n = yin.windowSize();
        ring.assign ((size_t) n, 0.0f);
        linear.assign ((size_t) n, 0.0f);
        writePos = 0;

        hop = std::max (1, (int) std::lround (dsr * 0.008));  // un análisis cada 8 ms
        hopCount = decimCount = 0;
        hopSumSq = prevHopSumSq = 0.0;

        tracker.configure (dsr / hop, (n + hop - 1) / hop);
        tracker.reset();

        hopOrig   = hop * decim;
        onsetLag  = (int) (onsetLagFraction  * n) * decim;
        offsetLag = (int) (offsetLagFraction * n) * decim;
        levelDb = -100.0f;
    }

    // Mete una muestra. Si toca analizar, devuelve cuántos eventos ha escrito en `out`.
    int push (float x, Event out[2])
    {
        const float y = lp2.process (lp1.process (hp.process (x)));
        if (++decimCount < decim)
            return 0;
        decimCount = 0;

        ring[(size_t) writePos] = y;
        writePos = (writePos + 1) % n;
        hopSumSq += (double) y * y;
        if (++hopCount < hop)
            return 0;
        hopCount = 0;
        return analyse (out);
    }

    // Calibrados con el banco de pruebas: cuánto tarda la ventana en "ver" el inicio/fin.
    float onsetLagFraction = 0.72f, offsetLagFraction = 0.08f;

private:
    int analyse (Event out[2])
    {
        // Nivel sobre los 2 últimos bloques (~16 ms), con la señal ya filtrada.
        const double rms = std::sqrt ((hopSumSq + prevHopSumSq) / (2.0 * hop));
        prevHopSumSq = hopSumSq;
        hopSumSq = 0.0;
        levelDb = 20.0f * std::log10 ((float) rms + 1e-9f);

        bool voiced = false;
        float midi = 0.0f;
        if (levelDb >= gateDb)
        {
            for (int i = 0; i < n; ++i)  // el buffer circular, ordenado del más viejo al más nuevo
                linear[(size_t) i] = ring[(size_t) ((writePos + i) % n)];
            const auto p = yin.detect (linear.data());
            if (p.hz > 0.0f && p.aperiodicity < voicingThreshold)
            {
                voiced = true;
                midi = hzToMidi (p.hz);
            }
        }

        lastVoiced = voiced;
        if (voiced) lastMidi = midi;

        NoteEvent ev[2];
        const int c = tracker.update (voiced, midi, levelDb, dbToVelocity (levelDb - gateDb), ev);
        for (int k = 0; k < c; ++k)
            out[k] = { ev[k].on, ev[k].note, ev[k].velocity,
                       ev[k].framesAgo * hopOrig + (ev[k].on ? onsetLag : offsetLag), ev[k].replace };
        return c;
    }

    Biquad hp, lp1, lp2;
    Yin yin;
    std::vector<float> ring, linear;
    int n = 0, writePos = 0, decim = 1, decimCount = 0, hop = 176, hopCount = 0;
    int hopOrig = 352, onsetLag = 0, offsetLag = 0;
    double hopSumSq = 0.0, prevHopSumSq = 0.0;
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
