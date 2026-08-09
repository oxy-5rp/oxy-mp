#pragma once

#include <oxymp/shared/protocol/protocol_version.hpp>

#include <cstdint>
#include <vector>

namespace oxymp::net {

/// Идентификатор соединения в пределах одного хоста.
///
/// Свой, а не указатель на структуру транспорта: наружу не должно торчать ничего,
/// что заставит остальной код знать про устройство библиотеки сети. Ноль не
/// выдаётся никогда и означает «соединения нет».
using PeerId = std::uint32_t;

inline constexpr PeerId kInvalidPeerId = 0;

/// Что произошло на транспорте.
struct Event {
    enum class Type {
        /// Соединение установлено.
        Connected,

        /// Соединение закрыто — другой стороной, по таймауту или нами.
        Disconnected,

        /// Пришёл пакет.
        Message,
    };

    Type type = Type::Disconnected;
    PeerId peer = kInvalidPeerId;

    /// Канал, по которому пришёл пакет. Осмыслен только для Message.
    shared::Channel channel = shared::Channel::Control;

    /// Содержимое пакета. Копия: время жизни буфера транспорта короче, чем
    /// время жизни события, и завязываться на него нельзя.
    std::vector<std::uint8_t> payload;
};

} // namespace oxymp::net
