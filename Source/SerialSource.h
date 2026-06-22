#pragma once

#include <juce_core/juce_core.h>

namespace bateria
{

/**
    Interface mínima de uma fonte de bytes serial.

    Abstrai "de onde vêm os bytes crus" para que o SerialAudioBridge não saiba
    se está falando com uma porta /dev/tty real (Arduino) ou com um simulador.
    Toda E/S aqui acontece SEMPRE na thread serial — nunca na thread de áudio.
*/
class ISerialSource
{
public:
    virtual ~ISerialSource() = default;

    /** Abre a fonte. @returns true em sucesso. */
    virtual bool open() = 0;

    /** Fecha a fonte e libera o recurso de SO. */
    virtual void close() = 0;

    /** Lê até maxBytes para dst, sem bloquear indefinidamente.
        @returns número de bytes lidos (0 se nada disponível agora), ou -1 em erro fatal. */
    virtual int read (char* dst, int maxBytes) = 0;

    /** Escreve len bytes (config -> Arduino). @returns bytes escritos, ou -1.
        Default: não suportado (fontes só-leitura). */
    virtual int write (const char* /*data*/, int /*len*/) { return -1; }

    virtual bool isOpen() const = 0;
};

//==============================================================================
/**
    Fonte simulada: gera linhas "pieceId:velocity\n" como se um Arduino
    estivesse mandando hits. Útil para desenvolver/testar o motor de áudio
    sem hardware. Determinística o suficiente para depurar, aleatória o
    suficiente para soar como uma performance.
*/
class SimulatedSerialSource final : public ISerialSource
{
public:
    explicit SimulatedSerialSource (juce::Array<int> pieceIds)
        : availablePieces (std::move (pieceIds)) {}

    bool open() override   { opened = true;  return true; }
    void close() override  { opened = false; }
    bool isOpen() const override { return opened; }

    int write (const char* /*data*/, int len) override { return len; } // no-op (sem hardware)

    int read (char* dst, int maxBytes) override
    {
        if (! opened)
            return -1;

        if (pending.isEmpty())
            maybeGenerateHit();

        if (pending.isEmpty())
            return 0;

        // Consome do início do buffer pendente (ASCII puro: 1 byte por char).
        const int n = juce::jmin (maxBytes, pending.size());
        std::memcpy (dst, pending.getRawDataPointer(), static_cast<size_t> (n));
        pending.removeRange (0, n);
        return n;
    }

private:
    void maybeGenerateHit()
    {
        // ~uma batida a cada poucas chamadas para não saturar.
        if (rng.nextFloat() > 0.15f || availablePieces.isEmpty())
            return;

        const int piece = availablePieces[rng.nextInt (availablePieces.size())];
        const int vel    = 1 + rng.nextInt (127); // 1..127
        const juce::String line = juce::String (piece) + ":" + juce::String (vel) + "\n";

        const char* raw = line.toRawUTF8();
        const int   len = line.getNumBytesAsUTF8();
        pending.addArray (raw, len);
    }

    juce::Array<int> availablePieces;
    juce::Array<char> pending; // buffer de bytes simples
    juce::Random rng;
    bool opened = false;
};

//==============================================================================
/**
    Fonte serial real via termios (macOS / Linux). Leitura não bloqueante com
    timeout curto (VTIME), de modo que a thread serial possa checar threadShouldExit().

    No Windows, ofereça uma implementação equivalente com a Win32 Comm API
    (CreateFile + ReadFile com COMMTIMEOUTS). Mantida fora deste arquivo para foco.
*/
#if JUCE_MAC || JUCE_LINUX
class PosixSerialSource final : public ISerialSource
{
public:
    PosixSerialSource (juce::String devicePath, int baudRate)
        : path (std::move (devicePath)), baud (baudRate) {}

    ~PosixSerialSource() override { close(); }

    bool open() override;
    void close() override;
    bool isOpen() const override { return fd >= 0; }
    int read (char* dst, int maxBytes) override;
    int write (const char* data, int len) override;

private:
    juce::String path;
    int baud = 115200;
    int fd = -1;
};
#endif

} // namespace bateria
