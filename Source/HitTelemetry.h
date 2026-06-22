#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace bateria
{

/**
    Canal de telemetria de hits da thread de ÁUDIO para a thread de UI.

    O sampler (áudio) chama record() a cada nota disparada; a UI faz polling
    (num Timer) e detecta hits novos comparando o contador. Tudo via std::atomic
    com memory_order relaxed — wait-free, sem locks, seguro no caminho de tempo real.

    Não carrega ordenação forte porque é puramente visual: um flash de pad pode
    chegar um frame depois sem qualquer prejuízo.
*/
class HitTelemetry
{
public:
    static constexpr int numNotes = 128;

    /** Chamado na thread de áudio ao disparar uma peça. */
    void record (int note, float velocity) noexcept
    {
        if (note < 0 || note >= numNotes)
            return;
        lastVelocity[static_cast<size_t> (note)].store (velocity, std::memory_order_relaxed);
        counter[static_cast<size_t> (note)].fetch_add (1, std::memory_order_relaxed);
    }

    /** Lido pela UI: contador monotônico de hits da nota. */
    std::uint32_t getCount (int note) const noexcept
    {
        return counter[static_cast<size_t> (note)].load (std::memory_order_relaxed);
    }

    /** Lido pela UI: velocity [0,1] do último hit da nota. */
    float getLastVelocity (int note) const noexcept
    {
        return lastVelocity[static_cast<size_t> (note)].load (std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<std::uint32_t>, numNotes> counter {};
    std::array<std::atomic<float>, numNotes> lastVelocity {};
};

} // namespace bateria
