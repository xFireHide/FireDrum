#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <vector>
#include <juce_audio_devices/juce_audio_devices.h>

#include "SampleBank.h"
#include "DrumSampler.h"
#include "DrumKit.h"
#include "HitTelemetry.h"
#include "SerialAudioBridge.h"
#include "SerialSource.h"

namespace bateria
{

/**
    Motor de áudio standalone.

    Junta as peças e roda SEM MIDI virtual e SEM DAW:
      - SerialAudioBridge: thread serial (MIDI) -> fila lock-free.
      - DrumSampler:       consumo da fila + síntese polifônica.
      - AudioDeviceManager: saída de áudio direta no hardware.

    Troca de kit é RT-safe via DOUBLE-BUFFER: dois SampleBank; a thread de UI
    gera o kit novo no banco inativo e publica o ponteiro com store-release.
    O áudio lê o ponteiro com load-acquire a cada bloco. Vozes já tocando
    seguram o banco antigo (one-shots curtos terminam antes do banco ser reusado).
*/
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    AudioEngine() = default;
    ~AudioEngine() override { shutdown(); }

    bool initialise (std::unique_ptr<ISerialSource> source)
    {
        bridge = std::make_unique<SerialAudioBridge> (std::move (source));

        const juce::String err = deviceManager.initialiseWithDefaultDevices (0, 2);
        if (err.isNotEmpty())
        {
            DBG ("AudioEngine: erro de áudio -> " << err);
            return false;
        }

        if (auto* dev = deviceManager.getCurrentAudioDevice())
            currentSampleRate = dev->getCurrentSampleRate();

        // Kit inicial no banco 0, com o áudio ainda parado (sem corrida).
        regenerateBank (banks[0], currentKit);
        activeBankIndex = 0;
        activeBank.store (&banks[0], std::memory_order_release);

        sampler.setTelemetry (&telemetry);
        sampler.prepare (currentSampleRate, 0);

        deviceManager.addAudioCallback (this);
        bridge->start();
        return true;
    }

    void shutdown()
    {
        deviceManager.removeAudioCallback (this);
        if (bridge != nullptr)
            bridge->stop();
        deviceManager.closeAudioDevice();
    }

    //==============================================================================
    // Controles (thread de UI / mensagens)
    //==============================================================================

    /** Troca o kit de forma RT-safe (double-buffer). */
    void setKit (int kitIndex)
    {
        kitIndex = juce::jlimit (0, kNumKits - 1, kitIndex);
        const int inactive = (activeBankIndex == 0) ? 1 : 0;

        regenerateBank (banks[inactive], kitIndex);
        activeBank.store (&banks[inactive], std::memory_order_release);

        activeBankIndex = inactive;
        currentKit = kitIndex;
    }

    int getKit() const noexcept { return currentKit; }

    /** Carrega samples WAV reais de uma pasta (kick.wav, snare.wav, hihat.wav,
        hihat_open.wav, tom_low/mid/high.wav, crash.wav, ride.wav). Os samples
        sobrepõem a síntese nas notas correspondentes; o resto fica no synth.
        RT-safe: regenera o banco inativo e publica o ponteiro. */
    bool loadSamplesFromFolder (const juce::File& dir)
    {
        if (! dir.isDirectory())
            return false;

        const std::vector<std::pair<juce::String, std::vector<int>>> roles = {
            { "kick.wav",       { 35, 36 } },
            { "snare.wav",      { 37, 38, 40 } },
            { "hihat.wav",      { 42, 44 } },
            { "hihat_open.wav", { 46 } },
            { "tom_low.wav",    { 41, 43 } },
            { "tom_mid.wav",    { 45, 47 } },
            { "tom_high.wav",   { 48, 50 } },
            { "crash.wav",      { 49, 52, 55, 57 } },
            { "ride.wav",       { 51, 53, 59 } },
        };

        std::map<int, juce::File> found;
        for (const auto& [file, notes] : roles)
        {
            const auto f = dir.getChildFile (file);
            if (f.existsAsFile())
                for (int n : notes) found[n] = f;
        }

        if (found.empty())
            return false;

        sampleFiles = std::move (found);

        const int inactive = (activeBankIndex == 0) ? 1 : 0;
        regenerateBank (banks[inactive], currentKit);
        activeBank.store (&banks[inactive], std::memory_order_release);
        activeBankIndex = inactive;
        return true;
    }

    bool usingSamples() const noexcept { return ! sampleFiles.empty(); }
    int  getNumSampledNotes() const noexcept { return (int) sampleFiles.size(); }

    /** Atribui (ou remove, se f inválido) o WAV de uma nota SEM regenerar.
        Use em lote no setup e depois chame rebuildActiveBank() uma vez. */
    void assignSample (int note, const juce::File& f)
    {
        if (f.existsAsFile())
            sampleFiles[note] = f;
        else
            sampleFiles.erase (note);
    }

    /** Regenera o banco inativo (synth + samples atuais) e publica — RT-safe. */
    void rebuildActiveBank()
    {
        const int inactive = (activeBankIndex == 0) ? 1 : 0;
        regenerateBank (banks[inactive], currentKit);
        activeBank.store (&banks[inactive], std::memory_order_release);
        activeBankIndex = inactive;
    }

    /** Troca o WAV de uma nota AO VIVO (atribui + regenera + publica). */
    void setNoteSample (int note, const juce::File& f)
    {
        assignSample (note, f);
        rebuildActiveBank();
    }

    void setMasterGain (float linear) noexcept            { sampler.setMasterGain (linear); }
    void setVelocitySensitivity (float exp) noexcept      { sampler.setVelocitySensitivity (exp); }
    void setPieceGain (int note, float linear) noexcept   { sampler.setPieceGain (note, linear); }
    void setPieceMute (int note, bool muted) noexcept     { sampler.setPieceMute (note, muted); }
    void setPieceEnabled (int note, bool enabled) noexcept { sampler.setPieceEnabled (note, enabled); }
    void setPieceSound (int note, int soundNote) noexcept  { sampler.setPieceSound (note, soundNote); }
    void setPieceSensitivity (int note, float exp) noexcept { sampler.setPieceSensitivity (note, exp); }

    /** Dispara uma nota manualmente pela UI (clique no kit / botão de play).
        Empurra na fila de UI (SPSC: produtor = thread de mensagens da UI). */
    void triggerNote (int note, float velocity) noexcept
    {
        DrumMessage m;
        m.pieceId  = note;
        m.velocity = juce::jlimit (0.0f, 1.0f, velocity);
        uiQueue.push (m);
    }

    /** Carrega um WAV numa peça (no banco ativo). Um setKit() posterior sobrescreve. */
    bool loadPiece (int pieceId, const juce::File& file)
    {
        return banks[activeBankIndex].loadSample (pieceId, file, currentSampleRate);
    }

    HitTelemetry& getTelemetry() noexcept { return telemetry; }

    /** Fila de log MIDI (serial -> UI). nullptr antes do initialise(). */
    SerialAudioBridge::LogQueue* getLogQueue() noexcept
    {
        return bridge != nullptr ? &bridge->getLogQueue() : nullptr;
    }

    /** Envia a config de um pad físico (A0-A5) ao Arduino pela serial.
        idx 0-5; valores 0-255 (a UI já limita). */
    void sendPadConfig (int idx, int sens, int thresh, int scan, int mask,
                        int note, int curve, int enable)
    {
        if (bridge == nullptr) return;
        juce::String line;
        line << "C" << idx << " " << sens << " " << thresh << " " << scan << " "
             << mask << " " << note << " " << curve << " " << enable << "\n";
        bridge->sendLine (line);
    }
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    int getDroppedMessages() const noexcept { return bridge != nullptr ? bridge->getDroppedCount() : 0; }

    //==============================================================================
    // juce::AudioIODeviceCallback — CAMINHO DE TEMPO REAL
    //==============================================================================
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        currentSampleRate = device->getCurrentSampleRate();
        sampler.prepare (currentSampleRate, device->getCurrentBufferSizeSamples());
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const* /*inputChannelData*/,
                                           int /*numInputChannels*/,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        juce::AudioBuffer<float> output (outputChannelData, numOutputChannels, numSamples);

        const SampleBank* bank = activeBank.load (std::memory_order_acquire);
        if (bank == nullptr)
        {
            output.clear();
            return;
        }

        sampler.process (output, bridge->getQueue(), &uiQueue, *bank);
    }

private:
    /** Preenche um banco: síntese do kit + samples WAV por cima (se houver). */
    void regenerateBank (SampleBank& b, int kitIndex)
    {
        b.generateSynthKit (currentSampleRate, kKits[kitIndex]);
        for (const auto& [note, file] : sampleFiles)
            b.loadSample (note, file, currentSampleRate);
    }

    juce::AudioDeviceManager deviceManager;
    SerialAudioBridge::Queue uiQueue;   // hits manuais da UI (clique/botão)
    std::map<int, juce::File> sampleFiles; // nota -> WAV real (sobrepõe o synth)

    SampleBank banks[2];                       // double-buffer pra troca de kit
    std::atomic<const SampleBank*> activeBank { nullptr };
    int activeBankIndex = 0;                    // só tocado na thread de UI
    int currentKit = 0;

    DrumSampler<64> sampler;
    HitTelemetry telemetry;
    std::unique_ptr<SerialAudioBridge> bridge;

    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace bateria
