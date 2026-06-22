#pragma once

#include <cstdint>
#include <type_traits>

namespace bateria
{

/**
    Mensagem de gatilho de bateria.

    É um POD trivialmente copiável: isso é PRÉ-REQUISITO para trafegar pela
    fila lock-free via memcpy bruto, sem construtores/destrutores e sem
    qualquer alocação na thread de áudio.

    A thread serial preenche e empurra; a thread de áudio consome e descarta.
*/
struct DrumMessage
{
    /// Identificador da peça (caixa, bumbo, prato...). -1 = inválido.
    std::int32_t pieceId = -1;

    /// Velocity já normalizada para [0, 1]. A conversão 0..127 -> 0..1
    /// acontece na thread serial para manter o áudio o mais enxuto possível.
    float velocity = 0.0f;
};

// Garantias verificadas em tempo de compilação (preferimos erro de compilação
// a surpresa em runtime). Se alguém quebrar a trivialidade, o build falha.
static_assert(std::is_trivially_copyable_v<DrumMessage>,
              "DrumMessage precisa ser trivially copyable para a FIFO lock-free");
static_assert(std::is_standard_layout_v<DrumMessage>,
              "DrumMessage precisa ter standard layout");

} // namespace bateria
