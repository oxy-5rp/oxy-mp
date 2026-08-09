#pragma once

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/message_id.hpp>
#include <oxymp/shared/protocol/protocol_version.hpp>
#include <oxymp/shared/protocol/serialization.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace oxymp::shared {

/// Предел длины имени игрока.
inline constexpr std::size_t kMaxNicknameLength = 24;

/// Идентификатор игрока, выданный сервером. Ноль не выдаётся никогда и
/// используется как «игрока нет».
using PlayerId = std::uint32_t;

inline constexpr PlayerId kInvalidPlayerId = 0;

struct ClientHello {
    static constexpr MessageId kId = MessageId::ClientHello;

    std::uint16_t protocolVersion = kProtocolVersion;
    std::string nickname;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ClientHello read(ByteReader& reader);
};

struct ServerWelcome {
    static constexpr MessageId kId = MessageId::ServerWelcome;

    PlayerId playerId = kInvalidPlayerId;
    Vec3 spawnPosition;
    std::uint16_t tickRate = kDefaultTickRate;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ServerWelcome read(ByteReader& reader);
};

struct ServerReject {
    static constexpr MessageId kId = MessageId::ServerReject;

    RejectReason reason = RejectReason::ProtocolMismatch;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ServerReject read(ByteReader& reader);
};

struct Ping {
    static constexpr MessageId kId = MessageId::Ping;

    /// Отметка времени отправителя. Сервер возвращает её без изменений, поэтому
    /// её смысл известен только отправителю и часы сторон сверять не нужно.
    std::uint64_t timestampMs = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static Ping read(ByteReader& reader);
};

struct Pong {
    static constexpr MessageId kId = MessageId::Pong;

    std::uint64_t timestampMs = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static Pong read(ByteReader& reader);
};

struct PlayerJoined {
    static constexpr MessageId kId = MessageId::PlayerJoined;

    PlayerId playerId = kInvalidPlayerId;
    std::string nickname;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerJoined read(ByteReader& reader);
};

struct PlayerLeft {
    static constexpr MessageId kId = MessageId::PlayerLeft;

    PlayerId playerId = kInvalidPlayerId;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerLeft read(ByteReader& reader);
};

// --- Упаковка сообщений в пакеты ----------------------------------------------
//
// Пакет — это байт с типом сообщения и следом его поля. Границы пакетов
// обеспечивает транспорт, поэтому длину сообщения передавать не нужно.

/// Собирает пакет из сообщения.
template<typename Message>
[[nodiscard]] std::vector<std::uint8_t> encode(const Message& message) {
    ByteWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(Message::kId));
    message.write(writer);
    return std::move(writer).take();
}

/// Тип сообщения в пакете, не разбирая остального. nullopt на пустом пакете
/// или на неизвестном значении.
[[nodiscard]] std::optional<MessageId> peekMessageId(ByteView packet) noexcept;

/// Разбирает пакет в сообщение ожидаемого типа.
///
/// Возвращает nullopt, если тип не тот, данных не хватило или, наоборот,
/// остались лишние байты: и то и другое означает, что стороны разошлись в
/// понимании протокола, и продолжать разбор нельзя.
template<typename Message>
[[nodiscard]] std::optional<Message> decode(ByteView packet) {
    ByteReader reader{packet};

    if (reader.readU8() != static_cast<std::uint8_t>(Message::kId)) {
        return std::nullopt;
    }

    Message message = Message::read(reader);
    if (!reader.ok() || !reader.exhausted()) {
        return std::nullopt;
    }

    return message;
}

} // namespace oxymp::shared
