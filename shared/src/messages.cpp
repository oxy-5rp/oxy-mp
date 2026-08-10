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
}

PlayerState PlayerState::read(ByteReader& reader) {
    PlayerState message;
    message.playerId = reader.readU32();
    message.position = reader.readVec3();
    message.heading = reader.readFloat();
    message.velocity = reader.readVec3();
    message.health = reader.readU16();
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
        return static_cast<MessageId>(packet.front());
    }

    return std::nullopt;
}

} // namespace oxymp::shared
