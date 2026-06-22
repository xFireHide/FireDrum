#pragma once

#include <memory>
#include <juce_core/juce_core.h>

#include "DrumMessage.h"
#include "MidiLogEvent.h"
#include "LockFreeQueue.h"
#include "SerialSource.h"

namespace bateria
{

/**
    Ponte Serial -> Áudio.

    Encapsula uma thread dedicada (juce::Thread) que:
      1. Abre a fonte serial (Arduino real ou simulador).
      2. Lê bytes crus em blocos.
      3. Faz o "framing" das linhas terminadas em '\n'.
      4. Faz o parse do protocolo "pieceId:velocity".
      5. Empurra uma DrumMessage na fila lock-free.

    A thread de áudio NUNCA toca nesta classe além de drenar a fila pública via
    getQueue().pop(). Todo o trabalho de I/O, parsing e alocação de string fica
    confinado AQUI, longe do caminho de tempo real.

    Capacidade da fila dimensionada com folga: mesmo numa rajada de hits, a
    thread de áudio drena tudo no início de cada bloco.
*/
class SerialAudioBridge final : private juce::Thread
{
public:
    static constexpr int queueCapacity = 1024;
    using Queue = LockFreeQueue<DrumMessage, queueCapacity>;

    static constexpr int logCapacity = 2048;
    using LogQueue = LockFreeQueue<MidiLogEvent, logCapacity>;

    /// Comando de saída (config -> Arduino). POD pra fila lock-free.
    struct OutCommand { char data[48]; int len = 0; };
    using OutQueue = LockFreeQueue<OutCommand, 64>;

    explicit SerialAudioBridge (std::unique_ptr<ISerialSource> sourceToOwn)
        : juce::Thread ("SerialAudioBridge"),
          source (std::move (sourceToOwn))
    {
        jassert (source != nullptr);
    }

    ~SerialAudioBridge() override { stop(); }

    /** Inicia a thread serial. Idempotente. */
    void start()
    {
        if (! isThreadRunning())
            startThread (juce::Thread::Priority::high);
    }

    /** Para a thread de forma limpa e fecha a porta. */
    void stop()
    {
        signalThreadShouldExit();
        stopThread (2000); // dá 2s para a read() destravar via VTIME
        if (source != nullptr)
            source->close();
    }

    /** Acesso (somente leitura/pop) à fila para a thread de áudio. */
    Queue& getQueue() noexcept { return queue; }

    /** Acesso (somente leitura/pop) à fila de log para a thread de UI. */
    LogQueue& getLogQueue() noexcept { return logQueue; }

    /** Enfileira uma linha de comando para enviar ao Arduino (thread de UI).
        A thread serial faz o write() de verdade. Linhas > 47 chars são cortadas. */
    void sendLine (const juce::String& line)
    {
        OutCommand cmd;
        const char* raw = line.toRawUTF8();
        cmd.len = juce::jmin ((int) sizeof (cmd.data), (int) line.getNumBytesAsUTF8());
        std::memcpy (cmd.data, raw, static_cast<size_t> (cmd.len));
        outQueue.push (cmd);
    }

    /** Mensagens descartadas por fila cheia (diagnóstico, atômico). */
    int getDroppedCount() const noexcept { return dropped.load (std::memory_order_relaxed); }

private:
    //==============================================================================
    void run() override
    {
        if (! source->open())
        {
            DBG ("SerialAudioBridge: falha ao abrir a fonte serial.");
            return;
        }

        char readBuf[256];

        while (! threadShouldExit())
        {
            // Envia comandos de config pendentes (UI -> Arduino).
            flushOutgoing();

            const int n = source->read (readBuf, sizeof (readBuf));

            if (n < 0)
                break; // erro fatal na porta

            if (n == 0)
            {
                // Nada disponível; cede um pouco de CPU sem furar latência.
                wait (1);
                continue;
            }

            for (int i = 0; i < n; ++i)
                handleMidiByte (static_cast<std::uint8_t> (readBuf[i]));
        }

        source->close();
    }

    void flushOutgoing()
    {
        OutCommand cmd;
        while (outQueue.pop (cmd))
            source->write (cmd.data, cmd.len);
    }

    //==============================================================================
    // Decodificador de MIDI serial (o Arduino emite bytes MIDI crus, como pro
    // Hairless). Máquina de estados que respeita running status e ignora
    // mensagens de tempo real (0xF8-0xFF) intercaladas. Drums são one-shot:
    // só Note On com velocity > 0 dispara uma peça; Note Off é descartado.
    //==============================================================================
    void handleMidiByte (std::uint8_t b)
    {
        if (b & 0x80) // byte de STATUS
        {
            if (b >= 0xF8)
                return; // System Real-Time (clock/sense): não mexe no running status

            if (b >= 0xF0)
            {
                runningStatus = 0; // System Common: invalida running status
                dataIndex = 0;
                return;
            }

            runningStatus = b;
            dataIndex     = 0;
            dataExpected  = expectedDataBytes (b);
            return;
        }

        // byte de DADO
        if (runningStatus == 0)
            return; // dado órfão sem status válido

        if (dataIndex < 2)
            data[dataIndex++] = b;

        if (dataIndex >= dataExpected)
        {
            dispatchChannelMessage();
            dataIndex = 0; // pronto pra próxima msg (running status preserva o status)
        }
    }

    /** Quantos bytes de dados um status de canal carrega. */
    static int expectedDataBytes (std::uint8_t status) noexcept
    {
        switch (status & 0xF0)
        {
            case 0xC0: // Program Change
            case 0xD0: // Channel Pressure
                return 1;
            default:   // Note On/Off, Poly Pressure, CC, Pitch Bend
                return 2;
        }
    }

    /** Processa uma mensagem de canal completa: dispara hit e/ou registra no log. */
    void dispatchChannelMessage()
    {
        const std::uint8_t hi       = runningStatus & 0xF0;
        const std::uint8_t note     = data[0] & 0x7F;
        const std::uint8_t velocity = data[1] & 0x7F;

        const bool isNoteOn  = (hi == 0x90 && velocity > 0);
        const bool isNoteOff = (hi == 0x80) || (hi == 0x90 && velocity == 0);

        if (! isNoteOn && ! isNoteOff)
            return; // CC / pitch bend / etc.: ignorado

        // Log de TODO o tráfego (mostrado no monitor da UI, estilo Hairless).
        pushLog (note, velocity, isNoteOn);

        if (! isNoteOn)
            return; // note-off não dispara nada (drums são one-shot)

        DrumMessage msg;
        msg.pieceId  = static_cast<std::int32_t> (note); // pieceId = nota MIDI (0..127)
        msg.velocity = static_cast<float> (velocity) / 127.0f;

        if (! queue.push (msg))
            dropped.fetch_add (1, std::memory_order_relaxed);
    }

    /** Empurra um evento no log da UI (descarta silenciosamente se a fila encher). */
    void pushLog (std::uint8_t note, std::uint8_t velocity, bool noteOn) noexcept
    {
        MidiLogEvent ev;
        ev.timeMs   = juce::Time::getMillisecondCounter();
        ev.note     = static_cast<std::int32_t> (note);
        ev.velocity = static_cast<std::int32_t> (velocity);
        ev.noteOn   = noteOn;
        logQueue.push (ev);
    }

    //==============================================================================
    std::unique_ptr<ISerialSource> source;
    Queue queue;
    LogQueue logQueue;
    OutQueue outQueue;

    // Estado do decodificador MIDI.
    std::uint8_t runningStatus = 0;
    std::uint8_t data[2] {};
    int dataIndex    = 0;
    int dataExpected = 0;

    std::atomic<int> dropped { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SerialAudioBridge)
};

} // namespace bateria
