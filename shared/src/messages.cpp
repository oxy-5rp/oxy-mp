#include <oxymp/shared/protocol/messages.hpp>

#include <algorithm>

namespace oxymp::shared {

void ClientHello::write(ByteWriter& writer) const {
    writer.writeU16(protocolVersion);
    writer.writeString(nickname);
}

ClientHello ClientHello::read(ByteReader& reader) {
    ClientHello message;
    message.protocolVersion = reader.readU16();
    message.nickname = reader.readString();
    return message;
}

void ServerWelcome::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeVec3(spawnPosition);
    writer.writeU16(tickRate);
}

ServerWelcome ServerWelcome::read(ByteReader& reader) {
    ServerWelcome message;
    message.playerId = reader.readU32();
    message.spawnPosition = reader.readVec3();
    message.tickRate = reader.readU16();
    return message;
}

void ServerReject::write(ByteWriter& writer) const {
    writer.writeU8(static_cast<std::uint8_t>(reason));
}

ServerReject ServerReject::read(ByteReader& reader) {
    ServerReject message;
    message.reason = static_cast<RejectReason>(reader.readU8());
    return message;
}

void Ping::write(ByteWriter& writer) const {
    writer.writeU64(timestampMs);
}

Ping Ping::read(ByteReader& reader) {
    Ping message;
    message.timestampMs = reader.readU64();
    return message;
}

void Pong::write(ByteWriter& writer) const {
    writer.writeU64(timestampMs);
}

Pong Pong::read(ByteReader& reader) {
    Pong message;
    message.timestampMs = reader.readU64();
    return message;
}

void PlayerJoined::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeString(nickname);
}

PlayerJoined PlayerJoined::read(ByteReader& reader) {
    PlayerJoined message;
    message.playerId = reader.readU32();
    message.nickname = reader.readString();
    return message;
}

void PlayerLeft::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
}

PlayerLeft PlayerLeft::read(ByteReader& reader) {
    PlayerLeft message;
    message.playerId = reader.readU32();
    return message;
}

void PlayerState::write(ByteWriter& writer) const {
    writer.writeU32(sentAt);
    writer.writeU32(playerId);
    writer.writeVec3(position);
    writer.writeFloat(heading);
    writer.writeVec3(velocity);
    writer.writeU16(health);
    writer.writeU16(armour);
    writer.writeU32(flags);
    writer.writeU32(weapon);
    writer.writeU16(ammo);
    writer.writeVec3(aimAt);
    writer.writeU32(vehicleId);
    writer.writeU8(static_cast<std::uint8_t>(seat));
    writer.writeU8(static_cast<std::uint8_t>(action));
    writer.writeU8(actionSequence);
}

PlayerState PlayerState::read(ByteReader& reader) {
    PlayerState message;
    message.sentAt = reader.readU32();
    message.playerId = reader.readU32();
    message.position = reader.readVec3();
    message.heading = reader.readFloat();
    message.velocity = reader.readVec3();
    message.health = reader.readU16();
    message.armour = reader.readU16();
    message.flags = reader.readU32();
    message.weapon = reader.readU32();
    message.ammo = reader.readU16();
    message.aimAt = reader.readVec3();
    message.vehicleId = reader.readU32();
    message.seat = static_cast<std::int8_t>(reader.readU8());
    message.action = static_cast<PedAction>(reader.readU8());
    message.actionSequence = reader.readU8();
    return message;
}

void VehicleState::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU32(sentAt);
    writer.writeU32(model);
    writer.writeVec3(position);
    writer.writeVec3(rotation);
    writer.writeVec3(velocity);
    writer.writeVec3(angularVelocity);
    writer.writeFloat(steer);
    writer.writeFloat(throttle);
    writer.writeFloat(brake);
    writer.writeU16(bodyHealth);
    writer.writeU16(engineHealth);
    writer.writeU16(tankHealth);
    writer.writeU16(flags);
    writer.writeU8(doorsOpen);
    writer.writeU8(doorsBroken);
    writer.writeU8(windowsBroken);
    writer.writeU8(tyresBurst);
}

VehicleState VehicleState::read(ByteReader& reader) {
    VehicleState message;
    message.id = reader.readU32();
    message.sentAt = reader.readU32();
    message.model = reader.readU32();
    message.position = reader.readVec3();
    message.rotation = reader.readVec3();
    message.velocity = reader.readVec3();
    message.angularVelocity = reader.readVec3();
    message.steer = reader.readFloat();
    message.throttle = reader.readFloat();
    message.brake = reader.readFloat();
    message.bodyHealth = reader.readU16();
    message.engineHealth = reader.readU16();
    message.tankHealth = reader.readU16();
    message.flags = reader.readU16();
    message.doorsOpen = reader.readU8();
    message.doorsBroken = reader.readU8();
    message.windowsBroken = reader.readU8();
    message.tyresBurst = reader.readU8();
    return message;
}

void VehicleAppearance::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU8(primaryColour);
    writer.writeU8(secondaryColour);
    writer.writeU8(pearlescentColour);
    writer.writeU8(wheelColour);
    writer.writeString(plate);
    writer.writeU8(plateStyle);
    writer.writeU8(static_cast<std::uint8_t>(livery));
    writer.writeU8(static_cast<std::uint8_t>(wheelType));
    writer.writeU8(static_cast<std::uint8_t>(windowTint));
    writer.writeFloat(dirtLevel);

    // Длина набора не пишется: она задана протоколом и одинакова у обеих сторон.
    // Написанная в пакет, она стала бы полем, которому получатель обязан верить,
    // — а верить ему нельзя.
    for (const std::int8_t mod : mods) {
        writer.writeU8(static_cast<std::uint8_t>(mod));
    }

    writer.writeU32(toggleMods);
}

VehicleAppearance VehicleAppearance::read(ByteReader& reader) {
    VehicleAppearance message;
    message.id = reader.readU32();
    message.primaryColour = reader.readU8();
    message.secondaryColour = reader.readU8();
    message.pearlescentColour = reader.readU8();
    message.wheelColour = reader.readU8();
    message.plate = reader.readString();
    message.plateStyle = reader.readU8();
    message.livery = static_cast<std::int8_t>(reader.readU8());
    message.wheelType = static_cast<std::int8_t>(reader.readU8());
    message.windowTint = static_cast<std::int8_t>(reader.readU8());
    message.dirtLevel = reader.readFloat();

    for (std::int8_t& mod : message.mods) {
        mod = static_cast<std::int8_t>(reader.readU8());
    }

    message.toggleMods = reader.readU32();

    if (message.plate.size() > kMaxPlateLength) {
        message.plate.resize(kMaxPlateLength);
    }

    return message;
}

void VehicleAdded::write(ByteWriter& writer) const {
    // Снимок вкладывается целиком и своим же порядком полей. Переписывать его
    // здесь заново значило бы завести второе описание одних и тех же данных,
    // которое рано или поздно разойдётся с первым.
    state.write(writer);
    writer.writeU32(owner);
}

VehicleAdded VehicleAdded::read(ByteReader& reader) {
    VehicleAdded message;
    message.state = VehicleState::read(reader);
    message.owner = reader.readU32();
    return message;
}

void VehicleRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

VehicleRemoved VehicleRemoved::read(ByteReader& reader) {
    VehicleRemoved message;
    message.id = reader.readU32();
    return message;
}

void VehicleAuthority::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU32(owner);
}

VehicleAuthority VehicleAuthority::read(ByteReader& reader) {
    VehicleAuthority message;
    message.id = reader.readU32();
    message.owner = reader.readU32();
    return message;
}

void WorldState::write(ByteWriter& writer) const {
    writer.writeString(weather);
    writer.writeU8(hour);
    writer.writeU8(minute);
    writer.writeU8(second);
}

WorldState WorldState::read(ByteReader& reader) {
    WorldState message;
    message.weather = reader.readString();
    message.hour = reader.readU8();
    message.minute = reader.readU8();
    message.second = reader.readU8();

    if (message.weather.size() > kMaxWeatherLength) {
        message.weather.resize(kMaxWeatherLength);
    }

    return message;
}

void PlayerLoadout::write(ByteWriter& writer) const {
    const auto count = static_cast<std::uint16_t>(
        std::min<std::size_t>(weapons.size(), kMaxWeaponSlots));

    writer.writeU8(static_cast<std::uint8_t>(replace));
    writer.writeU16(count);

    for (std::uint16_t index = 0; index < count; ++index) {
        writer.writeU32(weapons[index].weapon);
        writer.writeU16(weapons[index].ammo);
    }
}

PlayerLoadout PlayerLoadout::read(ByteReader& reader) {
    PlayerLoadout message;
    message.replace = reader.readU8() != 0;

    const std::uint16_t count = reader.readU16();

    // Длине из пакета верить нельзя, но и отбрасывать её молча — тоже. Память
    // резервируется по пределу, а не по названному числу: испорченное или
    // враждебное число иначе заставило бы выделить её под список, которого нет.
    //
    // Читается при этом ровно столько, сколько названо. Пакет, назвавший больше,
    // чем принёс, оборвётся на нехватке данных, и разбор откажет сам — а
    // молчаливый возврат пустого списка выглядел бы как «сервер разоружил
    // игрока», то есть был бы ложью, а не отказом.
    message.weapons.reserve(std::min<std::size_t>(count, kMaxWeaponSlots));

    for (std::uint16_t index = 0; index < count; ++index) {
        WeaponSlot slot;
        slot.weapon = reader.readU32();
        slot.ammo = reader.readU16();

        if (message.weapons.size() < kMaxWeaponSlots) {
            message.weapons.push_back(slot);
        }
    }

    return message;
}

void HealthChanged::write(ByteWriter& writer) const {
    writer.writeU16(health);
    writer.writeU16(armour);
    writer.writeU32(attacker);
}

HealthChanged HealthChanged::read(ByteReader& reader) {
    HealthChanged message;
    message.health = reader.readU16();
    message.armour = reader.readU16();
    message.attacker = reader.readU32();
    return message;
}

void ObjectAdded::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU32(model);
    writer.writeVec3(position);
    writer.writeVec3(rotation);
}

ObjectAdded ObjectAdded::read(ByteReader& reader) {
    ObjectAdded message;
    message.id = reader.readU32();
    message.model = reader.readU32();
    message.position = reader.readVec3();
    message.rotation = reader.readVec3();
    return message;
}

void ObjectRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

ObjectRemoved ObjectRemoved::read(ByteReader& reader) {
    ObjectRemoved message;
    message.id = reader.readU32();
    return message;
}

void ChatSay::write(ByteWriter& writer) const {
    writer.writeString(text);
}

ChatSay ChatSay::read(ByteReader& reader) {
    ChatSay message;
    message.text = reader.readString();
    return message;
}

void ChatLine::write(ByteWriter& writer) const {
    writer.writeU8(static_cast<std::uint8_t>(kind));
    writer.writeU32(playerId);
    writer.writeString(nickname);
    writer.writeString(text);
}

ChatLine ChatLine::read(ByteReader& reader) {
    ChatLine message;
    message.kind = static_cast<ChatKind>(reader.readU8());
    message.playerId = reader.readU32();
    message.nickname = reader.readString();
    message.text = reader.readString();
    return message;
}

void DamageReport::write(ByteWriter& writer) const {
    writer.writeU32(victim);
    writer.writeU16(amount);
    writer.writeU32(weapon);
}

DamageReport DamageReport::read(ByteReader& reader) {
    DamageReport message;
    message.victim = reader.readU32();
    message.amount = reader.readU16();
    message.weapon = reader.readU32();
    return message;
}

void DamageTaken::write(ByteWriter& writer) const {
    writer.writeU32(attacker);
    writer.writeU16(amount);
    writer.writeU32(weapon);
}

DamageTaken DamageTaken::read(ByteReader& reader) {
    DamageTaken message;
    message.attacker = reader.readU32();
    message.amount = reader.readU16();
    message.weapon = reader.readU32();
    return message;
}

void PlayerTeleport::write(ByteWriter& writer) const {
    writer.writeVec3(position);
}

PlayerTeleport PlayerTeleport::read(ByteReader& reader) {
    PlayerTeleport message;
    message.position = reader.readVec3();
    return message;
}

namespace {

/// Пишет именованное событие.
///
/// Обрезка при записи, а не только при чтении: длинное имя или нагрузку следует
/// оборвать у того, кто их сочинил, — иначе получатель отвергнет пакет целиком,
/// и отправитель об этом не узнает.
void writeEvent(ByteWriter& writer, const std::string& name, const std::string& payload) {
    writer.writeString(name.substr(0, std::min(name.size(), kMaxEventNameLength)));
    writer.writeText(payload);
}

/// Читает его обратно.
void readEvent(ByteReader& reader, std::string& name, std::string& payload) {
    name = reader.readString();
    payload = reader.readText();

    // Предел применяется и при чтении: пакет мог прийти откуда угодно, и верить
    // его полям нельзя.
    if (name.size() > kMaxEventNameLength) {
        name.resize(kMaxEventNameLength);
    }
    if (payload.size() > kMaxEventPayloadLength) {
        payload.resize(kMaxEventPayloadLength);
    }
}

} // namespace

void ClientEvent::write(ByteWriter& writer) const {
    writeEvent(writer, name, payload);
}

ClientEvent ClientEvent::read(ByteReader& reader) {
    ClientEvent message;
    readEvent(reader, message.name, message.payload);
    return message;
}

void ServerEvent::write(ByteWriter& writer) const {
    writeEvent(writer, name, payload);
}

ServerEvent ServerEvent::read(ByteReader& reader) {
    ServerEvent message;
    readEvent(reader, message.name, message.payload);
    return message;
}

void MoneyChanged::write(ByteWriter& writer) const {
    // Знаковое пишется беззнаковым, и обратное превращение возвращает то же
    // число: дополнительный код и его обход по кругу определены языком, а не
    // оставлены на усмотрение сборки.
    writer.writeU64(static_cast<std::uint64_t>(amount));
}

MoneyChanged MoneyChanged::read(ByteReader& reader) {
    MoneyChanged message;
    message.amount = static_cast<std::int64_t>(reader.readU64());
    return message;
}

void ResourceList::write(ByteWriter& writer) const {
    const auto count =
        static_cast<std::uint16_t>(std::min<std::size_t>(entries.size(), kMaxResources));

    writer.writeU16(count);

    for (std::uint16_t i = 0; i < count; ++i) {
        writer.writeString(entries[i].name);
        writer.writeString(entries[i].hash);
        writer.writeU64(entries[i].size);
        writer.writeU8(entries[i].page ? 1 : 0);
    }
}

ResourceList ResourceList::read(ByteReader& reader) {
    ResourceList message;

    const std::uint16_t count = reader.readU16();

    // Предел применяется при чтении, а не только при записи: пакет мог прийти
    // откуда угодно, и верить его полю длины нельзя.
    const std::uint16_t safe = std::min<std::uint16_t>(count, kMaxResources);

    message.entries.reserve(safe);

    for (std::uint16_t i = 0; i < safe; ++i) {
        ResourceEntry entry;
        entry.name = reader.readString();
        entry.hash = reader.readString();
        entry.size = reader.readU64();
        entry.page = reader.readU8() != 0;

        message.entries.push_back(std::move(entry));
    }

    return message;
}

std::optional<MessageId> peekMessageId(ByteView packet) noexcept {
    if (packet.empty()) {
        return std::nullopt;
    }

    switch (static_cast<MessageId>(packet.front())) {
    case MessageId::ClientHello:
    case MessageId::ServerWelcome:
    case MessageId::ServerReject:
    case MessageId::Ping:
    case MessageId::Pong:
    case MessageId::PlayerJoined:
    case MessageId::PlayerLeft:
    case MessageId::PlayerState:
    case MessageId::VehicleState:
    case MessageId::VehicleAppearance:
    case MessageId::ChatSay:
    case MessageId::ChatLine:
    case MessageId::DamageReport:
    case MessageId::DamageTaken:
    case MessageId::PlayerTeleport:
    case MessageId::MoneyChanged:
    case MessageId::ResourceList:
    case MessageId::VehicleAdded:
    case MessageId::VehicleRemoved:
    case MessageId::VehicleAuthority:
    case MessageId::WorldState:
    case MessageId::PlayerLoadout:
    case MessageId::HealthChanged:
    case MessageId::ObjectAdded:
    case MessageId::ObjectRemoved:
    case MessageId::ClientEvent:
    case MessageId::ServerEvent:
        return static_cast<MessageId>(packet.front());
    }

    return std::nullopt;
}

} // namespace oxymp::shared
