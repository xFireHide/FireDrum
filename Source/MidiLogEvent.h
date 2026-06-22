#pragma once

#include <cstdint>
#include <type_traits>

namespace bateria
{

/**
    Evento de log MIDI para o monitor da UI (estilo console do Hairless).

    Produzido pela thread serial a cada mensagem decodificada, consumido pela
    thread de UI via fila lock-free. POD trivialmente copiável (requisito da FIFO).
    Separado do DrumMessage: o monitor mostra TODO o tráfego (inclusive note-off
    e hits mutados), enquanto o áudio só recebe os disparos.
*/
struct MidiLogEvent
{
    std::uint32_t timeMs   = 0;   // juce::Time::getMillisecondCounter() na captura
    std::int32_t  note     = 0;
    std::int32_t  velocity = 0;
    bool          noteOn   = false;
};

static_assert (std::is_trivially_copyable_v<MidiLogEvent>,
               "MidiLogEvent precisa ser trivially copyable para a FIFO lock-free");

} // namespace bateria
