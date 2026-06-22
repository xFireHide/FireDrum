#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

#include "DrumMessage.h"
#include "SampleBank.h"
#include "SerialAudioBridge.h"
#include "HitTelemetry.h"

namespace bateria
{

/**
    Motor de sampler polifônico de tempo real.

    Pool de vozes de tamanho FIXO (alocado uma vez). Em process():
      1. Drena a fila lock-free e dispara uma voz por mensagem.
      2. Mixa todas as vozes ativas no buffer de saída.

    Garantias do caminho de áudio (process()):
      - SEM new/malloc.
      - SEM mutex/lock.
      - SEM I/O, SEM exceções.
    Apenas leitura de samples imutáveis + aritmética de ponto flutuante.

    O banco de samples é passado POR PROCESSO (não guardado), o que permite
    trocar de kit com double-buffer: a thread de UI gera um banco novo e
    publica o ponteiro atomicamente, sem corrida com o áudio.

    Controles por peça (ganho/mute) e a sensibilidade de velocity são
    std::atomic, ajustáveis pela UI ao vivo sem locks.

    @tparam MaxVoices  Polifonia máxima. Cada voz é barata; 64 é folgado.
*/
template <int MaxVoices = 64>
class DrumSampler
{
public:
    static constexpr int numPieces = 128;

    DrumSampler()
    {
        for (auto& g : pieceGain) g.store (1.0f, std::memory_order_relaxed);
        for (auto& m : pieceMute) m.store (false, std::memory_order_relaxed);
        for (auto& e : pieceEnabled) e.store (true, std::memory_order_relaxed);
        for (auto& s : pieceSens)    s.store (0.6f, std::memory_order_relaxed);
        // Mapa de som identidade: nota tocada usa o próprio sample por padrão.
        for (int i = 0; i < numPieces; ++i)
            soundFor[static_cast<size_t> (i)].store (i, std::memory_order_relaxed);
    }

    /** Configuração antes de tocar (thread de áudio em estado parado). */
    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        currentSampleRate = sampleRate;
        for (auto& v : voices)
            v.active = false;
    }

    /** Telemetria de hits (áudio -> UI). Setar antes de iniciar o áudio. */
    void setTelemetry (HitTelemetry* t) noexcept { telemetry = t; }

    /** Ganho master linear (slider de volume). Atômico. */
    void setMasterGain (float linear) noexcept
    {
        masterGain.store (linear, std::memory_order_relaxed);
    }

    /** Sensibilidade de velocity: expoente da curva (0.5 sensível, 1.0 linear). */
    void setVelocitySensitivity (float exponent) noexcept
    {
        velocitySensitivity.store (exponent, std::memory_order_relaxed);
    }

    /** Volume linear de uma peça (mixer por pad). */
    void setPieceGain (int note, float linear) noexcept
    {
        if (juce::isPositiveAndBelow (note, numPieces))
            pieceGain[static_cast<size_t> (note)].store (linear, std::memory_order_relaxed);
    }

    /** Mute de uma peça. */
    void setPieceMute (int note, bool muted) noexcept
    {
        if (juce::isPositiveAndBelow (note, numPieces))
            pieceMute[static_cast<size_t> (note)].store (muted, std::memory_order_relaxed);
    }

    /** Liga/desliga um pad (nota recebida). Desligado = ignora os hits. */
    void setPieceEnabled (int note, bool enabled) noexcept
    {
        if (juce::isPositiveAndBelow (note, numPieces))
            pieceEnabled[static_cast<size_t> (note)].store (enabled, std::memory_order_relaxed);
    }

    /** Sensibilidade (expoente da curva de velocity) por peça. */
    void setPieceSensitivity (int note, float exponent) noexcept
    {
        if (juce::isPositiveAndBelow (note, numPieces))
            pieceSens[static_cast<size_t> (note)].store (exponent, std::memory_order_relaxed);
    }

    /** Remapeia o SOM de um pad: a nota recebida toca o sample de outra nota.
        Ex.: setPieceSound (25, 38) faz o pad da nota 25 soar como caixa (38). */
    void setPieceSound (int note, int soundNote) noexcept
    {
        if (juce::isPositiveAndBelow (note, numPieces)
            && juce::isPositiveAndBelow (soundNote, numPieces))
            soundFor[static_cast<size_t> (note)].store (soundNote, std::memory_order_relaxed);
    }

    /**
        Renderiza um bloco. Chamado SOMENTE pela thread de áudio.

        @param output   buffer de saída
        @param queue    fila lock-free da thread serial (hardware)
        @param uiQueue  fila lock-free da thread de UI (cliques/botão), ou nullptr
        @param bank     banco de samples ativo (ponteiro publicado pela UI)
    */
    void process (juce::AudioBuffer<float>& output,
                  SerialAudioBridge::Queue& queue,
                  SerialAudioBridge::Queue* uiQueue,
                  const SampleBank& bank) noexcept
    {
        output.clear();

        // (1) Drena mensagens da serial e da UI; cada uma é SPSC com seu próprio
        // produtor (serial / message thread) e este consumidor único (áudio).
        DrumMessage msg;
        while (queue.pop (msg))
            triggerVoice (msg, bank);
        if (uiQueue != nullptr)
            while (uiQueue->pop (msg))
                triggerVoice (msg, bank);

        // (2) Mixa as vozes ativas.
        const int numOutCh   = output.getNumChannels();
        const int numSamples = output.getNumSamples();
        const float master   = masterGain.load (std::memory_order_relaxed);

        for (auto& voice : voices)
        {
            if (! voice.active)
                continue;

            renderVoice (voice, output, numOutCh, numSamples, master);
        }
    }

private:
    //==============================================================================
    struct Voice
    {
        const juce::AudioBuffer<float>* sample = nullptr;
        int   position   = 0;     // posição de leitura em amostras
        float gain       = 0.0f;  // ganho final (velocity * volume da peça)
        bool  active     = false;
    };

    /** Dispara uma voz para a mensagem. Faz voice-stealing se o pool encher. */
    void triggerVoice (const DrumMessage& msg, const SampleBank& bank) noexcept
    {
        const int note = msg.pieceId;
        if (! juce::isPositiveAndBelow (note, numPieces))
            return;

        const auto un = static_cast<size_t> (note);

        if (! pieceEnabled[un].load (std::memory_order_relaxed))
            return; // pad desligado: ignora o hit

        if (pieceMute[un].load (std::memory_order_relaxed))
            return; // peça mutada: nem dispara

        // Remapeia o som: a nota recebida pode tocar o sample de outra nota.
        const int sound = soundFor[un].load (std::memory_order_relaxed);
        const juce::AudioBuffer<float>* sample = bank.get (sound);
        if (sample == nullptr || sample->getNumSamples() == 0)
            return; // som sem sample carregado

        // Telemetria pra UI (flash do pad) — antes do voice-stealing, sempre registra.
        if (telemetry != nullptr)
            telemetry->record (note, msg.velocity);

        Voice* slot = findFreeVoice();
        if (slot == nullptr)
            slot = stealQuietestVoice();

        const float sens = pieceSens[un].load (std::memory_order_relaxed);
        const float pg   = pieceGain[un].load (std::memory_order_relaxed);

        slot->sample   = sample;
        slot->position = 0;
        // Curva de velocity (expoente < 1 realça hits leves) * volume da peça.
        slot->gain     = std::pow (msg.velocity, sens) * pg;
        slot->active   = true;
    }

    /** Mixa uma voz no output e avança/desativa quando o sample termina. */
    static void renderVoice (Voice& voice,
                             juce::AudioBuffer<float>& output,
                             int numOutCh, int numSamples, float master) noexcept
    {
        const auto& src        = *voice.sample;
        const int sampleLen    = src.getNumSamples();
        const int sampleCh     = src.getNumChannels();
        const float g          = voice.gain * master;

        int pos = voice.position;
        const int remaining    = sampleLen - pos;
        const int n            = juce::jmin (numSamples, remaining);

        for (int ch = 0; ch < numOutCh; ++ch)
        {
            // Sample mono toca em todos os canais; estéreo respeita o canal.
            const int srcCh = juce::jmin (ch, sampleCh - 1);
            const float* in = src.getReadPointer (srcCh, pos);
            float* out      = output.getWritePointer (ch);

            juce::FloatVectorOperations::addWithMultiply (out, in, g, n);
        }

        voice.position = pos + n;
        if (voice.position >= sampleLen)
            voice.active = false; // sample terminou: libera o slot
    }

    Voice* findFreeVoice() noexcept
    {
        for (auto& v : voices)
            if (! v.active)
                return &v;
        return nullptr;
    }

    /** Rouba a voz mais silenciosa — artefato menos audível que cortar a oldest. */
    Voice* stealQuietestVoice() noexcept
    {
        Voice* quietest = &voices[0];
        for (auto& v : voices)
            if (v.gain < quietest->gain)
                quietest = &v;
        return quietest;
    }

    //==============================================================================
    std::array<Voice, static_cast<size_t> (MaxVoices)> voices {};

    double currentSampleRate = 44100.0;
    std::atomic<float> masterGain { 1.0f };
    std::atomic<float> velocitySensitivity { 0.6f };

    std::array<std::atomic<float>, numPieces> pieceGain {};
    std::array<std::atomic<bool>,  numPieces> pieceMute {};
    std::array<std::atomic<bool>,  numPieces> pieceEnabled {};
    std::array<std::atomic<float>, numPieces> pieceSens {};
    std::array<std::atomic<int>,   numPieces> soundFor {};

    HitTelemetry* telemetry = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSampler)
};

} // namespace bateria
