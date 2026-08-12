#include <oxymp/shared/protocol/messages.hpp>

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
    writer.writeU32(playerId);
    writer.writeVec3(position);
    writer.writeFloat(heading);
    writer.writeVec3(velocity);
    writer.writeU16(health);
    writer.writeU16(armour);
    writer.writeU32(flags);
    writer.writeU32(weapon);
    writer.writeVec3(aimAt);
    writer.writeU32(vehicleOwner);
    writer.writeU8(static_cast<std::uint8_t>(seat));
}

PlayerState PlayerState::read(ByteReader& reader) {
    PlayerState message;
    message.playerId = reader.readU32();
    message.position = reader.readVec3();
    message.heading = reader.readFloat();
    message.velocity = reader.readVec3();
    message.health = reader.readU16();
    message.armour = reader.readU16();
    message.flags = reader.readU32();
    message.weapon = reader.readU32();
    message.aimAt = reader.readVec3();
    message.vehicleOwner = reader.readU32();
    message.seat = static_cast<std::int8_t>(reader.readU8());
    return message;
}

void VehicleState::write(ByteWriter& writer) const {
    writer.writeU32(owner);
    writer.writeU32(model);
    writer.writeVec3(position);
    writer.writeVec3(rotation);
    writer.writeVec3(velocity);
    writer.writeU16(bodyHealth);
}

VehicleState VehicleState::read(ByteReader& reader) {
    VehicleState message;
    message.owner = reader.readU32();
    message.model = reader.readU32();
    message.position = reader.readVec3();
    message.rotation = reader.readVec3();
    message.velocity = reader.readVec3();
    message.bodyHealth = reader.readU16();
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

void AdminAction::write(ByteWriter& writer) const {
    writer.writeU8(static_cast<std::uint8_t>(command));
    writer.writeU32(target);
    writer.writeVec3(position);
}

AdminAction AdminAction::read(ByteReader& reader) {
    AdminAction message;
    message.command = static_cast<AdminCommand>(reader.readU8());
    message.target = reader.readU32();
    message.position = reader.readVec3();
    return message;
}

void AdminOrder::write(ByteWriter& writer) const {
    writer.writeU8(static_cast<std::uint8_t>(command));
    writer.writeU32(issuer);
    writer.writeVec3(position);
}

AdminOrder AdminOrder::read(ByteReader& reader) {
    AdminOrder message;
    message.command = static_cast<AdminCommand>(reader.readU8());
    message.issuer = reader.readU32();
    message.position = reader.readVec3();
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
    case MessageId::ChatSay:
    case MessageId::ChatLine:
    case MessageId::DamageReport:
    case MessageId::DamageTaken:
    case MessageId::AdminAction:
    case MessageId::AdminOrder:
        return static_cast<MessageId>(packet.front());
    }

    return std::nullopt;
}

} // namespace oxymp::shared
