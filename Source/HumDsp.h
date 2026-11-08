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
#include <array>
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
        fps = frameRate;
        const auto frames = [frameRate] (double ms) { return std::max (1, (int) std::lround (ms * 0.001 * frameRate)); };
        applyDetail();
        scoopFrames   = frames (160);
        reviewFrames  = frames (300);
        reviewMinFrames = frames (48);
        dipMaxFrames  = frames (250);
        settleFrames  = std::max (1, (windowFrames + 1) / 2);
        peakLeakDb    = (float) (10.0 / frameRate);  // el pico de referencia baja 10 dB/s
        tuneAlpha     = (float) (1.0 / (1.5 * frameRate));  // se adapta en ~1.5 s de notas estables
    }

    // Detalle: 1 = lo capta todo (adornos, notas de paso, temblores); 0 = solo la melodía
    // principal. Con menos detalle una nota tiene que durar más y desviarse más para contar.
    void setDetail (float d)
    {
        d = std::clamp (d, 0.0f, 1.0f);
        if (std::abs (d - detail) < 0.001f) return;
        detail = d;
        applyDetail();
    }
    float getDetail() const { return detail; }

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

    void applyDetail()
    {
        const double s = 1.0 - detail;  // 0 = fino, 1 = suave
        const auto frames = [this] (double ms) { return std::max (1, (int) std::lround (ms * 0.001 * fps)); };
        // Solo cambia el TIEMPO: cuánto tiene que durar algo para ser nota. La tolerancia de tono
        // y los re-ataques no se tocan, o se perderían pasos de semitono y notas repetidas reales.
        onsetFrames   = std::min (frames (40 + 40 * s), kHist / 2);   // 40 -> 80 ms para empezar una nota
        changeFrames  = std::min (frames (48 + 90 * s), kHist / 2);   // 48 -> 138 ms para cambiar de nota
        releaseFrames = frames (56 + 30 * s);                          // huecos algo más largos no cortan
        correctFrames = frames (100 + 80 * s);
        hysteresis = (float) (0.5 + 0.1 * s);
    }

    bool belongsToCurrent (float m) const
    {
        const float dCur = std::abs (m - (float) current);
        if (dCur < tolerance)
            return true;
        const int note = snap (m, root, steps, stepCount);
        return note == current || dCur - std::abs (m - (float) note) < hysteresis;
    }

    static constexpr float maxSpread = 1.1f, maxTrend = 0.55f, maxOnsetTrend = 0.3f;
    static constexpr float tolerance = 0.7f, dipDb = 9.0f, riseDb = 6.0f;
    float hysteresis = 0.5f;
    float detail = 1.0f;
    double fps = 125.0;
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

// ---------------------------------------------------------------- Simplificar la toma
// Al exportar: una nota más corta que minLen se funde con la vecina con la que está pegada
// (la anterior si la hay; si no, la siguiente). Si está aislada entre silencios, se quita.
struct TakeNote { int note, velocity; double start, end; };

inline void mergeShortNotes (std::vector<TakeNote>& notes, double minLen, double touch)
{
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (notes[i].end - notes[i].start >= minLen) continue;
            const bool prevTouch = i > 0 && notes[i].start - notes[i - 1].end <= touch;
            const bool nextTouch = i + 1 < notes.size() && notes[i + 1].start - notes[i].end <= touch;
            if (prevTouch && nextTouch && notes[i + 1].note == notes[i - 1].note)
            {
                // 60 -> 61 (cortita) -> 60: era un adorno o un temblor, todo es un único 60.
                notes[i - 1].end = notes[i + 1].end;
                notes.erase (notes.begin() + (long) i, notes.begin() + (long) i + 2);
            }
            else
            {
                if (prevTouch)      notes[i - 1].end = notes[i].end;
                else if (nextTouch) notes[i + 1].start = notes[i].start;
                notes.erase (notes.begin() + (long) i);
            }
            changed = true;
            break;
        }
    }
}

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
    int frameCount = 0;              // sube en cada análisis (cada 8 ms)

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
        ++frameCount;

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

// ---------------------------------------------------------------- Teoría musical
// Detección de tonalidad (Krumhansl-Kessler): compara cuánto tiempo suena cada nota
// (do, do#, re...) con el "perfil" típico de cada tonalidad mayor y menor.
struct KeyResult { int root = 0; bool minor = false; float score = 0.0f, margin = 0.0f; bool valid = false; };

inline KeyResult detectKey (const float pitchClassWeight[12])
{
    static constexpr float major[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static constexpr float minor[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

    float total = 0.0f;
    int distinct = 0;
    for (int i = 0; i < 12; ++i) { total += pitchClassWeight[i]; distinct += pitchClassWeight[i] > 0.0f; }
    KeyResult best;
    if (total <= 0.0f || distinct < 3)
        return best;  // con menos de 3 notas distintas no hay forma de saberlo

    const auto corr = [&] (const float* profile, int root)
    {
        float mx = 0.0f, my = 0.0f;
        for (int i = 0; i < 12; ++i) { mx += pitchClassWeight[(i + root) % 12]; my += profile[i]; }
        mx /= 12.0f; my /= 12.0f;
        float sxy = 0.0f, sxx = 0.0f, syy = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            const float dx = pitchClassWeight[(i + root) % 12] - mx, dy = profile[i] - my;
            sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
        }
        return sxx > 0.0f ? sxy / std::sqrt (sxx * syy) : 0.0f;
    };

    // Además del perfil: ¿cuánto de lo cantado cabe en la escala? Una sola nota fuera (p. ej.
    // un fa natural en Mi menor) delata a la candidata equivocada cuando el perfil duda.
    static constexpr int majorSet[] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int minorSet[] = { 0, 2, 3, 5, 7, 8, 10, 11 };  // natural + 7ª armónica
    const auto fit = [&] (bool isMinor, int root)
    {
        float in = 0.0f;
        const int* set = isMinor ? minorSet : majorSet;
        const int n = isMinor ? 8 : 7;
        for (int i = 0; i < n; ++i) in += pitchClassWeight[(root + set[i]) % 12];
        return in / total;
    };

    float second = -2.0f;
    best.score = -2.0f;
    for (int mode = 0; mode < 2; ++mode)
        for (int root = 0; root < 12; ++root)
        {
            const float c = corr (mode == 0 ? major : minor, root) - (1.0f - fit (mode == 1, root));
            if (c > best.score) { second = best.score; best = { root, mode == 1, c, 0.0f, true }; }
            else if (c > second) second = c;
        }
    best.margin = best.score - second;
    return best;
}

// Acordes que acompañan a la nota cantada, por debajo de ella (la melodía queda arriba).
// Con escala: acorde de la propia escala (diatónico). Sin escala: tríada mayor.
enum ChordType { NoChord = 0, Triad, Seventh, Fifth, Octave };

inline int chordTones (int note, ChordType type, int root, const int* steps, int count, int out[4])
{
    if (type == NoChord) return 0;
    if (type == Octave) { out[0] = note - 12; return 1; }
    if (type == Fifth)  { out[0] = note - 12; out[1] = note - 5; return 2; }

    int offsets[3] = { 4, 7, 11 };  // mayor con 7ª mayor, si la nota no está en la escala
    const int rel = ((note - root) % 12 + 12) % 12;
    int degree = -1;
    for (int i = 0; i < count; ++i) if (steps[i] == rel) degree = i;
    if (degree >= 0 && count >= 5)
        for (int k = 0; k < 3; ++k)
        {
            const int d = degree + 2 * (k + 1);  // apilar terceras: grados +2, +4, +6
            offsets[k] = steps[d % count] - steps[degree] + 12 * (d / count);
        }

    const int numUpper = type == Seventh ? 3 : 2;
    int n = 0;
    out[n++] = note - 12;                        // fundamental, una octava por debajo
    for (int k = 0; k < numUpper; ++k)
    {
        int t = note - 12 + offsets[k];
        while (t >= note) t -= 12;               // siempre por debajo de la melodía
        out[n++] = t;
    }
    return n;
}

// Cuantización con fuerza: 1 = a la rejilla exacta, 0.5 = a medio camino (suena más humano).
// legato = cada nota se alarga hasta la siguiente si el hueco es menor de un tiempo.
inline void quantizeNotes (std::vector<TakeNote>& notes, double grid, double strength, bool legato, double beat)
{
    if (grid > 0.0 && strength > 0.0)
        for (auto& n : notes)
        {
            const double len = n.end - n.start;
            const double qs = std::round (n.start / grid) * grid;
            const double qe = std::round (n.end / grid) * grid;
            n.start += (qs - n.start) * strength;
            n.end   += (qe - n.end) * strength;
            const double minLen = grid * strength + len * (1.0 - strength);  // ni más corta que una casilla
            if (n.end - n.start < minLen) n.end = n.start + minLen;
        }
    if (legato)
        for (size_t i = 0; i + 1 < notes.size(); ++i)
            if (notes[i + 1].start - notes[i].end < beat)
                notes[i].end = notes[i + 1].start;
}

// Monofónico: ninguna nota puede pisar a la siguiente. Si dos caen en el mismo sitio al
// cuantizar, se queda la segunda.
inline void makeMonophonic (std::vector<TakeNote>& notes)
{
    std::vector<TakeNote> clean;
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
    notes.swap (clean);
}

// ---------------------------------------------------------------- Synth interno
// Polifónico (8 voces) para poder oír los acordes y escuchar las tomas. Cada voz lleva una
// etiqueta: 0 = lo que cantas en vivo, 1 = reproducción de una toma. El pitch bend solo
// afecta a las voces en vivo.
class PolySynth
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        attackCoeff  = 1.0 - std::exp (-1.0 / (0.005 * sr));  // 5 ms
        releaseCoeff = 1.0 - std::exp (-1.0 / (0.120 * sr));  // 120 ms
        for (auto& v : voices) v = {};
        bendRatio = 1.0;
    }

    void noteOn (int note, int tag, float velocity = 1.0f)
    {
        Voice* target = nullptr;
        for (auto& v : voices) if (v.gate && v.note == note && v.tag == tag) target = &v;  // misma nota: reutilizar
        if (! target) for (auto& v : voices) if (! v.gate && v.env < 1e-4) { target = &v; break; }
        if (! target)  // todas ocupadas: la más silenciosa
        {
            target = &voices[0];
            for (auto& v : voices) if (v.env < target->env) target = &v;
        }
        target->note = note;
        target->tag = tag;
        target->gate = true;
        target->gain = 0.35f + 0.65f * velocity;
        target->freq = 440.0 * std::pow (2.0, (note - 69) / 12.0);
    }

    void noteOff (int note, int tag)
    {
        for (auto& v : voices) if (v.gate && v.note == note && v.tag == tag) v.gate = false;
    }

    void allOff (int tag)
    {
        for (auto& v : voices) if (v.tag == tag) v.gate = false;
    }

    void setBend (float semitones) { bendRatio = std::pow (2.0, semitones / 12.0); }

    // gains[tag]: volumen de las voces en vivo y de las de reproducción
    float next (float liveGain, float previewGain)
    {
        constexpr double twoPi = 6.283185307179586;
        double out = 0.0;
        for (auto& v : voices)
        {
            if (! v.gate && v.env < 1e-5) continue;
            v.env += ((v.gate ? 1.0 : 0.0) - v.env) * (v.gate ? attackCoeff : releaseCoeff);
            v.phase += v.freq * (v.tag == 0 ? bendRatio : 1.0) / sr;
            if (v.phase >= 1.0) v.phase -= 1.0;
            const double s = std::sin (twoPi * v.phase) + 0.22 * std::sin (2.0 * twoPi * v.phase)
                           + 0.12 * std::sin (3.0 * twoPi * v.phase);
            out += s * v.env * v.gain * (v.tag == 0 ? liveGain : previewGain);
        }
        return (float) (0.45 * std::tanh (out));  // tanh: varias voces a la vez no saturan
    }

private:
    struct Voice { int note = -1, tag = 0; bool gate = false; float gain = 1.0f; double freq = 440.0, phase = 0.0, env = 0.0; };
    std::array<Voice, 8> voices {};
    double sr = 44100.0, attackCoeff = 0.0, releaseCoeff = 0.0, bendRatio = 1.0;
};

} // namespace hum
