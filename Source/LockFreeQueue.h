#pragma once

#include <array>
#include <type_traits>
#include <juce_core/juce_core.h>

namespace bateria
{

/**
    Fila circular lock-free Single-Producer / Single-Consumer (SPSC).

    Produtor: thread serial (SerialAudioBridge).
    Consumidor: thread de áudio (callback de tempo real).

    Implementada sobre juce::AbstractFifo, que apenas gerencia os índices de
    leitura/escrita de forma atômica (acquire/release) — nós fornecemos o
    armazenamento. O buffer é um std::array de tamanho FIXO alocado na
    construção do objeto, portanto:

      - NENHUMA alocação dinâmica em push()/pop().
      - NENHUM mutex/lock em push()/pop().

    Ambas as operações são wait-free no caminho comum.

    @tparam T         Tipo trivialmente copiável a transportar.
    @tparam Capacity  Número de slots. Use potência de 2 por higiene de cache.
*/
template <typename T, int Capacity>
class LockFreeQueue
{
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "LockFreeQueue exige um tipo trivially copyable (sem locks no destrutor)");
    static_assert(Capacity > 1, "Capacity precisa ser > 1");

    LockFreeQueue() = default;

    /** Empurra um item. Chamado SOMENTE pela thread produtora (serial).
        @returns false se a fila estiver cheia (item descartado — preferível a bloquear). */
    bool push(const T& item) noexcept
    {
        const auto scope = abstractFifo.write(1);

        if (scope.blockSize1 > 0)
        {
            storage[static_cast<size_t>(scope.startIndex1)] = item;
            return true;
        }

        // blockSize1 == 0 -> sem espaço livre.
        return false;
    }

    /** Retira um item. Chamado SOMENTE pela thread consumidora (áudio).
        @returns false se a fila estiver vazia. */
    bool pop(T& dest) noexcept
    {
        const auto scope = abstractFifo.read(1);

        if (scope.blockSize1 > 0)
        {
            dest = storage[static_cast<size_t>(scope.startIndex1)];
            return true;
        }

        return false;
    }

    /** Quantidade aproximada de itens prontos para leitura (apenas diagnóstico). */
    int getNumReady() const noexcept { return abstractFifo.getNumReady(); }

    /** Esvazia a fila. NÃO é thread-safe — usar só quando ambas as pontas estão paradas. */
    void reset() noexcept { abstractFifo.reset(); }

private:
    juce::AbstractFifo abstractFifo { Capacity };
    std::array<T, static_cast<size_t>(Capacity)> storage {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LockFreeQueue)
};

} // namespace bateria
