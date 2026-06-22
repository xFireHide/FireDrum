#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>

#include "DrumKit.h"

namespace bateria
{

/**
    Banco de samples pré-carregados em memória.

    Todo o carregamento de disco, decodificação e resampling acontece em
    loadSample() — chamado na thread de mensagens/setup, NUNCA na thread de
    áudio. Os AudioBuffer<float> resultantes ficam residentes em RAM.

    A thread de áudio só chama get(), que devolve um ponteiro const para um
    buffer imutável — leitura pura, sem locks, sem alocação.

    Os samples são resampleados OFFLINE para o sample rate do motor, de modo
    que o caminho de tempo real seja uma cópia 1:1 sem interpolação por amostra.
*/
class SampleBank
{
public:
    static constexpr int maxPieces = 128; // uma peça por nota MIDI (0..127)

    SampleBank()
    {
        formatManager.registerBasicFormats();
        loaded.fill (false);
    }

    /** Carrega/decodifica/resampleia um arquivo para o slot pieceId.
        NÃO é real-time safe — chame no setup. @returns true em sucesso. */
    bool loadSample (int pieceId, const juce::File& file, double engineSampleRate)
    {
        if (! juce::isPositiveAndBelow (pieceId, maxPieces))
            return false;

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
            return false;

        const int numChannels = static_cast<int> (reader->numChannels);
        const auto numSamples  = static_cast<int> (reader->lengthInSamples);

        juce::AudioBuffer<float> source (numChannels, numSamples);
        reader->read (&source, 0, numSamples, 0, true, true);

        auto& dest = buffers[static_cast<size_t> (pieceId)];

        if (std::abs (reader->sampleRate - engineSampleRate) < 1.0)
        {
            dest = std::move (source); // já está no SR certo
        }
        else
        {
            const double ratio = reader->sampleRate / engineSampleRate;
            const int outSamples = static_cast<int> (std::ceil (numSamples / ratio));
            dest.setSize (numChannels, outSamples);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::LagrangeInterpolator interp;
                interp.process (ratio,
                                source.getReadPointer (ch),
                                dest.getWritePointer (ch),
                                outSamples);
            }
        }

        // Normaliza o pico para ~0.9: WAVs vêm com volumes muito diferentes
        // (ex.: um crash "grab" gravado baixo soaria sumido ao lado dos outros).
        float peak = 0.0f;
        for (int ch = 0; ch < dest.getNumChannels(); ++ch)
            peak = juce::jmax (peak, dest.getMagnitude (ch, 0, dest.getNumSamples()));
        if (peak > 1.0e-4f)
            dest.applyGain (0.9f / peak);

        loaded[static_cast<size_t> (pieceId)] = true;
        return true;
    }

    /** Gera um kit sintético completo (uma peça por nota MIDI) direto em RAM.
        Útil quando não há WAVs: cada nota vira um one-shot percussivo distinto —
        notas graves soam como bumbo/tom, agudas como caixa/prato. NÃO é
        real-time safe (chamar no setup, antes de iniciar o áudio). */
    void generateSynthKit (double engineSampleRate, const KitParams& kit)
    {
        for (int note = 0; note < maxPieces; ++note)
            generateDrumSound (note, engineSampleRate, kit);
    }

    /** Ponteiro para o sample da peça, ou nullptr se não carregado.
        REAL-TIME SAFE: só lê estado imutável montado no load. */
    const juce::AudioBuffer<float>* get (int pieceId) const noexcept
    {
        if (! juce::isPositiveAndBelow (pieceId, maxPieces))
            return nullptr;
        if (! loaded[static_cast<size_t> (pieceId)])
            return nullptr;
        return &buffers[static_cast<size_t> (pieceId)];
    }

private:
    //==========================================================================
    // Síntese de bateria por MODELO dedicado a cada tipo de peça (offline).
    //==========================================================================
    enum class DrumType { Kick, Snare, Tom, ClosedHat, OpenHat, Crash, Ride, Cowbell, Clap, Other };

    /** Classifica a nota MIDI no tipo de peça (mapa General MIDI percussion). */
    static DrumType classify (int note)
    {
        switch (note)
        {
            case 35: case 36:                 return DrumType::Kick;
            case 37: case 38: case 40:        return DrumType::Snare;
            case 39:                          return DrumType::Clap;
            case 41: case 43: case 45:
            case 47: case 48: case 50:        return DrumType::Tom;
            case 42: case 44:                 return DrumType::ClosedHat;
            case 46:                          return DrumType::OpenHat;
            case 49: case 52: case 55: case 57: return DrumType::Crash;
            case 51: case 53: case 59:        return DrumType::Ride;
            case 56:                          return DrumType::Cowbell;
            default:
                if (note < 36) return DrumType::Kick;
                if (note < 48) return DrumType::Tom;
                return DrumType::ClosedHat;
        }
    }

    static double baseDuration (DrumType t)
    {
        switch (t)
        {
            case DrumType::Kick:      return 0.42;
            case DrumType::Snare:     return 0.28;
            case DrumType::Clap:      return 0.30;
            case DrumType::Tom:       return 0.55;
            case DrumType::ClosedHat: return 0.085;
            case DrumType::OpenHat:   return 0.50;
            case DrumType::Crash:     return 1.60;
            case DrumType::Ride:      return 1.00;
            case DrumType::Cowbell:   return 0.35;
            default:                  return 0.30;
        }
    }

    void generateDrumSound (int note, double sr, const KitParams& kit)
    {
        const DrumType type = classify (note);
        const double dur = juce::jlimit (0.04, 3.0, baseDuration (type) * kit.decayScale);
        const int len = juce::jmax (1, static_cast<int> (dur * sr));

        auto& dest = buffers[static_cast<size_t> (note)];
        dest.setSize (1, len, false, true, false);
        float* w = dest.getWritePointer (0);
        std::fill (w, w + len, 0.0f);

        juce::Random rng (1234 + note); // determinístico por nota

        switch (type)
        {
            case DrumType::Kick:      renderKick   (w, len, sr, kit, rng); break;
            case DrumType::Snare:     renderSnare  (w, len, sr, kit, rng); break;
            case DrumType::Clap:      renderClap   (w, len, sr, kit, rng); break;
            case DrumType::Tom:       renderTom    (note, w, len, sr, kit, rng); break;
            case DrumType::ClosedHat: renderHat    (w, len, sr, kit, rng, false); break;
            case DrumType::OpenHat:   renderHat    (w, len, sr, kit, rng, true);  break;
            case DrumType::Crash:     renderCymbal (w, len, sr, kit, rng, true);  break;
            case DrumType::Ride:      renderCymbal (w, len, sr, kit, rng, false); break;
            case DrumType::Cowbell:   renderCowbell(w, len, sr, kit, rng); break;
            default:                  renderTom    (note, w, len, sr, kit, rng); break;
        }

        applyLoFi   (w, len, kit);
        normalise   (w, len);
        fadeOut      (w, len, sr);
        loaded[static_cast<size_t> (note)] = true;
    }

    //== Utilidades de DSP =====================================================
    static constexpr double kTwoPi = juce::MathConstants<double>::twoPi;

    /** Filtro de 1 polo (passa-baixa/passa-alta). Estado em z. */
    struct OnePole
    {
        double z = 0.0;
        double lowpass  (double x, double a) noexcept { z += a * (x - z); return z; }
        double highpass (double x, double a) noexcept { z += a * (x - z); return x - z; }
    };
    static double poleCoeff (double cutoff, double sr) noexcept
    {
        return 1.0 - std::exp (-kTwoPi * juce::jmax (10.0, cutoff) / sr);
    }
    static double square (double phase) noexcept { return std::sin (phase) >= 0.0 ? 1.0 : -1.0; }
    static double noise (juce::Random& r) noexcept { return r.nextDouble() * 2.0 - 1.0; }

    //== Modelos por peça ======================================================
    static void renderKick (float* w, int len, double sr, const KitParams& kit, juce::Random& rng)
    {
        const double fStart = 135.0 * kit.tuning, fEnd = 47.0 * kit.tuning;
        const double pTau = 0.045, aTau = 0.16 * kit.decayScale, clickT = 0.006;
        double ph = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            const double f = fEnd + (fStart - fEnd) * std::exp (-t / pTau);
            ph += kTwoPi * f / sr;
            const double env = std::exp (-t / aTau);
            double s = std::sin (ph);
            if (t < clickT) s += (1.0 - t / clickT) * 0.8 * noise (rng); // batida do beater
            s = std::tanh (s * (1.6 * kit.saturation));
            w[i] = static_cast<float> (s * env);
        }
    }

    static void renderSnare (float* w, int len, double sr, const KitParams& kit, juce::Random& rng)
    {
        const double f1 = 185.0 * kit.tuning, f2 = 332.0 * kit.tuning;
        const double bTau = 0.09 * kit.decayScale, nTau = 0.13 * kit.decayScale;
        double p1 = 0, p2 = 0;
        OnePole hp, lp;
        const double hpA = poleCoeff (1400.0, sr);
        const double lpA = poleCoeff (7500.0 * kit.brightness, sr);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            p1 += kTwoPi * f1 / sr; p2 += kTwoPi * f2 / sr;
            const double body = (std::sin (p1) * 0.6 + std::sin (p2) * 0.4) * std::exp (-t / bTau);
            const double bp = lp.lowpass (hp.highpass (noise (rng), hpA), lpA); // esteira
            const double snares = bp * std::exp (-t / nTau) * kit.noiseScale;
            double s = body * 0.55 + snares * 1.0;
            s = std::tanh (s * kit.saturation);
            w[i] = static_cast<float> (s);
        }
    }

    static void renderTom (int note, float* w, int len, double sr, const KitParams& kit, juce::Random& rng)
    {
        double f0 = (95.0 + (note - 41) * 9.0) * kit.tuning;
        f0 = juce::jlimit (70.0, 320.0, f0);
        const double fStart = f0 * 1.5, pTau = 0.05, aTau = 0.32 * kit.decayScale;
        double ph = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            const double f = f0 + (fStart - f0) * std::exp (-t / pTau);
            ph += kTwoPi * f / sr;
            double s = std::sin (ph);
            if (t < 0.005) s += (1.0 - t / 0.005) * 0.4 * noise (rng);
            s = std::tanh (s * (1.2 * kit.saturation));
            w[i] = static_cast<float> (s * std::exp (-t / aTau));
        }
    }

    // Som metálico = soma de 6 quadradas inarmônicas (estilo TR-808) + passa-alta.
    static void renderHat (float* w, int len, double sr, const KitParams& kit, juce::Random& rng, bool open)
    {
        static const double ratios[6] = { 2.0, 3.0, 4.16, 5.43, 6.79, 8.21 };
        const double base = 320.0 * kit.tuning;
        const double aTau = (open ? 0.30 : 0.045) * kit.decayScale;
        double ph[6] = { 0, 0, 0, 0, 0, 0 };
        OnePole hp;
        const double hpA = poleCoeff (juce::jlimit (3000.0, 12000.0, 7200.0 * kit.brightness), sr);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            double sum = 0.0;
            for (int k = 0; k < 6; ++k) { ph[k] += kTwoPi * base * ratios[k] / sr; sum += square (ph[k]); }
            sum /= 6.0;
            const double mix = 0.72 * sum + 0.28 * noise (rng) * kit.noiseScale;
            double s = hp.highpass (mix, hpA);
            s = std::tanh (s * 1.3);
            w[i] = static_cast<float> (s * std::exp (-t / aTau));
        }
    }

    static void renderCymbal (float* w, int len, double sr, const KitParams& kit, juce::Random& rng, bool crash)
    {
        static const double ratios[6] = { 2.0, 3.0, 4.16, 5.43, 6.79, 8.21 };
        const double base = (crash ? 280.0 : 360.0) * kit.tuning;
        const double aTau = (crash ? 0.95 : 0.62) * kit.decayScale;
        double ph[6] = { 0, 0, 0, 0, 0, 0 }, bellPh = 0.0;
        OnePole hp;
        const double hpA = poleCoeff (juce::jlimit (2500.0, 9000.0, (crash ? 4800.0 : 6200.0) * kit.brightness), sr);
        const double bellF = 545.0 * kit.tuning;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            double sum = 0.0;
            for (int k = 0; k < 6; ++k) { ph[k] += kTwoPi * base * ratios[k] / sr; sum += square (ph[k]); }
            sum /= 6.0;
            const double wash = (crash ? 0.5 : 0.18) * noise (rng) * kit.noiseScale;
            double s = hp.highpass (0.6 * sum + wash, hpA);
            if (! crash) // ride tem o "ping" do bell
            {
                bellPh += kTwoPi * bellF / sr;
                s += 0.28 * std::sin (bellPh) * std::exp (-t / (0.5 * kit.decayScale));
            }
            s = std::tanh (s * 1.2);
            w[i] = static_cast<float> (s * std::exp (-t / aTau));
        }
    }

    static void renderCowbell (float* w, int len, double sr, const KitParams& kit, juce::Random&)
    {
        const double f1 = 560.0 * kit.tuning, f2 = 845.0 * kit.tuning;
        const double aTau = 0.25 * kit.decayScale;
        double p1 = 0, p2 = 0;
        OnePole hp; const double hpA = poleCoeff (500.0, sr);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            p1 += kTwoPi * f1 / sr; p2 += kTwoPi * f2 / sr;
            double s = hp.highpass (square (p1) * 0.5 + square (p2) * 0.5, hpA);
            s = std::tanh (s * 1.4);
            w[i] = static_cast<float> (s * std::exp (-t / aTau));
        }
    }

    static void renderClap (float* w, int len, double sr, const KitParams& kit, juce::Random& rng)
    {
        const double bursts[4] = { 0.0, 0.010, 0.020, 0.032 };
        OnePole hp, lp;
        const double hpA = poleCoeff (1100.0, sr);
        const double lpA = poleCoeff (3200.0 * kit.brightness, sr);
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            double amp = 0.0;
            for (double b : bursts) if (t >= b) amp += std::exp (-(t - b) / 0.009);
            amp += 0.6 * std::exp (-t / (0.13 * kit.decayScale)); // cauda da sala
            const double bp = lp.lowpass (hp.highpass (noise (rng), hpA), lpA);
            w[i] = static_cast<float> (std::tanh (bp * amp * 1.2));
        }
    }

    //== Pós-processamento =====================================================
    static void applyLoFi (float* w, int len, const KitParams& kit)
    {
        const int hold = juce::jmax (1, static_cast<int> (1.0 / juce::jlimit (0.02, 1.0, (double) kit.srCrush)));
        if (hold <= 1) return;
        double held = 0.0;
        for (int i = 0; i < len; ++i) { if (i % hold == 0) held = w[i]; w[i] = (float) held; }
    }

    static void normalise (float* w, int len)
    {
        float peak = 0.0f;
        for (int i = 0; i < len; ++i) peak = juce::jmax (peak, std::abs (w[i]));
        if (peak > 1.0e-4f)
        {
            const float g = 0.92f / peak;
            for (int i = 0; i < len; ++i) w[i] *= g;
        }
    }

    static void fadeOut (float* w, int len, double sr)
    {
        const int f = juce::jmin (len, juce::jmax (16, (int) (0.003 * sr))); // ~3ms anti-clique
        for (int i = 0; i < f; ++i)
        {
            const float g = (float) i / (float) f;
            w[len - 1 - i] *= g;
        }
    }

    juce::AudioFormatManager formatManager;
    std::array<juce::AudioBuffer<float>, maxPieces> buffers;
    std::array<bool, maxPieces> loaded {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleBank)
};

} // namespace bateria
