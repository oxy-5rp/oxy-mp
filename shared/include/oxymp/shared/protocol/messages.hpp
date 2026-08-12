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

/// Предел длины строки чата.
inline constexpr std::size_t kMaxChatLength = 160;

/// Идентификатор игрока, выданный сервером.
///
/// Это номер места на сервере, а не порядковый номер подключения: первый
/// вошедший получает ноль, второй — единицу, а освободившееся место достаётся
/// следующему вошедшему. Так же устроены идентификаторы у RAGE MP, и на то есть
/// причина помимо привычки: игрок называет себя этим числом в чате, и оно должно
/// быть коротким и повторяемым, а не расти до бесконечности за сессию.
///
/// Отсюда и значение «игрока нет»: ноль занят настоящим игроком, поэтому пустым
/// служит наибольшее возможное число.
using PlayerId = std::uint32_t;

inline constexpr PlayerId kInvalidPlayerId = 0xFFFFFFFFU;

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

/// Чем игрок занят помимо перемещения.
///
/// Битами, а не отдельными полями: признаков много, каждый занимает один бит, и
/// снимок уходит по сети двадцать раз в секунду от каждого игрока.
enum class PlayerFlag : std::uint32_t {
    /// Игрок мёртв.
    Dead = 1U << 0U,

    /// Целится из оружия.
    Aiming = 1U << 1U,

    /// Стреляет прямо сейчас.
    Shooting = 1U << 2U,

    /// Тело обмякло: сбит машиной, упал, оглушён.
    Ragdoll = 1U << 3U,

    /// В прыжке.
    Jumping = 1U << 4U,

    /// Сидит в машине. Какой именно — в полях vehicleOwner и seat.
    InVehicle = 1U << 5U,
};

[[nodiscard]] constexpr std::uint32_t operator|(PlayerFlag left, PlayerFlag right) noexcept {
    return static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right);
}

[[nodiscard]] constexpr bool has(std::uint32_t flags, PlayerFlag flag) noexcept {
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

/// Место водителя. Пассажирские места нумеруются с нуля — так же, как в игре.
inline constexpr std::int8_t kDriverSeat = -1;

/// Снимок состояния игрока в мире.
///
/// Ходит по ненадёжному каналу и часто: потерянный снимок дешевле заменить
/// следующим, чем переотправлять устаревший.
struct PlayerState {
    static constexpr MessageId kId = MessageId::PlayerState;

    /// Чей это снимок.
    ///
    /// В сообщении от клиента не заполняется: сервер знает, с какого соединения
    /// пришёл пакет, и проставляет идентификатор сам. Верить здесь клиенту
    /// нельзя — иначе он сможет выдать себя за другого.
    PlayerId playerId = kInvalidPlayerId;

    Vec3 position;

    /// Направление взгляда в градусах.
    float heading = 0.0F;

    /// Скорость. Нужна, чтобы получатель мог достроить движение между снимками,
    /// а не дёргать модель от точки к точке.
    Vec3 velocity;

    std::uint16_t health = 200;
    std::uint16_t armour = 0;

    /// Набор PlayerFlag.
    std::uint32_t flags = 0;

    /// Хеш оружия в руках. Ноль — безоружен.
    std::uint32_t weapon = 0;

    /// Куда направлено оружие. Имеет смысл только при Aiming или Shooting.
    ///
    /// Точка, а не направление: получатель ставит по ней задачу прицеливания, а
    /// той нужны координаты в мире.
    Vec3 aimAt;

    /// Кому принадлежит машина, в которой сидит игрок.
    ///
    /// Машина числится за своим водителем, и пассажир называет здесь его
    /// идентификатор. Так получателю не нужно заводить отдельную нумерацию
    /// машин: их ровно столько, сколько игроков за рулём.
    PlayerId vehicleOwner = kInvalidPlayerId;

    /// Место в машине: -1 водитель, 0 и дальше — пассажирские.
    std::int8_t seat = kDriverSeat;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerState read(ByteReader& reader);
};

/// Снимок машины, за рулём которой сидит отправитель.
///
/// Машину ведёт её водитель, и только он рассылает её состояние. Пассажиры
/// молчат: две стороны, правящие одну машину, дают дрожание, а не точность.
struct VehicleState {
    static constexpr MessageId kId = MessageId::VehicleState;

    /// Водитель. Как и у состояния игрока, проставляется сервером.
    PlayerId owner = kInvalidPlayerId;

    /// Хеш модели. По нему получатель закажет и создаст такую же.
    std::uint32_t model = 0;

    Vec3 position;

    /// Поворот по трём осям в градусах, порядок игры (2).
    Vec3 rotation;

    Vec3 velocity;

    /// Прочность кузова, от нуля до тысячи. Ниже нуля машина считается сломанной.
    std::uint16_t bodyHealth = 1000;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static VehicleState read(ByteReader& reader);
};

/// Реплика игрока в чат.
struct ChatSay {
    static constexpr MessageId kId = MessageId::ChatSay;

    std::string text;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ChatSay read(ByteReader& reader);
};

/// Строка чата, разосланная сервером.
struct ChatLine {
    static constexpr MessageId kId = MessageId::ChatLine;

    ChatKind kind = ChatKind::Say;

    /// Кто это сказал. kInvalidPlayerId — сервер.
    PlayerId playerId = kInvalidPlayerId;

    /// Имя автора. Приходит вместе со строкой, а не берётся из списка игроков:
    /// сообщение о выходе иначе оказалось бы без имени — игрока уже нет.
    std::string nickname;

    std::string text;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ChatLine read(ByteReader& reader);
};

/// Клиент сообщает, что попал по чужому игроку.
struct DamageReport {
    static constexpr MessageId kId = MessageId::DamageReport;

    PlayerId victim = kInvalidPlayerId;
    std::uint16_t amount = 0;
    std::uint32_t weapon = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static DamageReport read(ByteReader& reader);
};

/// Распоряжение из админ-меню.
///
/// Проверка прав — на сервере: клиент вправе попросить о чём угодно, а решает,
/// слушать ли его, тот, кто держит сессию.
struct AdminAction {
    static constexpr MessageId kId = MessageId::AdminAction;

    AdminCommand command = AdminCommand::Summon;

    /// Кого касается. kInvalidPlayerId — всех.
    PlayerId target = kInvalidPlayerId;

    /// Куда переносить. Имеет смысл только для Summon.
    Vec3 position;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static AdminAction read(ByteReader& reader);
};

/// То же распоряжение, переданное тому, кого оно касается.
struct AdminOrder {
    static constexpr MessageId kId = MessageId::AdminOrder;

    AdminCommand command = AdminCommand::Summon;

    /// Кто распорядился. Нужен для строки в чате: молча перенесённый игрок
    /// решит, что игра сломалась.
    PlayerId issuer = kInvalidPlayerId;

    Vec3 position;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static AdminOrder read(ByteReader& reader);
};

/// Сервер сообщает игроку, что по нему попали.
struct DamageTaken {
    static constexpr MessageId kId = MessageId::DamageTaken;

    PlayerId attacker = kInvalidPlayerId;
    std::uint16_t amount = 0;
    std::uint32_t weapon = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static DamageTaken read(ByteReader& reader);
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
