// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_test_macros.hpp>

#include <oxymp/shared/protocol/messages.hpp>

#include <vector>

using namespace oxymp::shared;

namespace {

/// Прогоняет сообщение через сборку пакета и обратный разбор.
template<typename Message>
std::optional<Message> roundTrip(const Message& message) {
    const std::vector<std::uint8_t> packet = encode(message);
    return decode<Message>(ByteView{packet});
}

} // namespace

TEST_CASE("ClientHello survives a round trip", "[messages]") {
    ClientHello sent;
    sent.protocolVersion = 7;
    sent.nickname = "oxy";

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->protocolVersion == 7);
    CHECK(received->nickname == "oxy");
}

TEST_CASE("ServerWelcome survives a round trip", "[messages]") {
    ServerWelcome sent;
    sent.playerId = 42;
    sent.spawnPosition = Vec3{-1234.5F, 567.25F, 88.0F};
    sent.tickRate = 30;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->playerId == 42);
    CHECK(received->spawnPosition == sent.spawnPosition);
    CHECK(received->tickRate == 30);
}

TEST_CASE("ServerReject survives a round trip", "[messages]") {
    ServerReject sent;
    sent.reason = RejectReason::ServerFull;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->reason == RejectReason::ServerFull);
}

TEST_CASE("Ping and Pong survive a round trip", "[messages]") {
    Ping ping;
    ping.timestampMs = 0x0123456789ABCDEFULL;

    const auto receivedPing = roundTrip(ping);
    REQUIRE(receivedPing.has_value());
    CHECK(receivedPing->timestampMs == ping.timestampMs);

    Pong pong;
    pong.timestampMs = ping.timestampMs;

    const auto receivedPong = roundTrip(pong);
    REQUIRE(receivedPong.has_value());
    CHECK(receivedPong->timestampMs == pong.timestampMs);
}

TEST_CASE("PlayerJoined and PlayerLeft survive a round trip", "[messages]") {
    PlayerJoined joined;
    joined.playerId = 3;
    joined.nickname = "гость";

    const auto receivedJoined = roundTrip(joined);
    REQUIRE(receivedJoined.has_value());
    CHECK(receivedJoined->playerId == 3);
    CHECK(receivedJoined->nickname == "гость");

    PlayerLeft left;
    left.playerId = 3;

    const auto receivedLeft = roundTrip(left);
    REQUIRE(receivedLeft.has_value());
    CHECK(receivedLeft->playerId == 3);
}

TEST_CASE("peekMessageId reads the type without decoding", "[messages]") {
    const std::vector<std::uint8_t> packet = encode(Ping{});

    CHECK(peekMessageId(ByteView{packet}) == MessageId::Ping);
}

TEST_CASE("peekMessageId rejects empty and unknown packets", "[messages]") {
    CHECK_FALSE(peekMessageId(ByteView{}).has_value());

    const std::vector<std::uint8_t> unknown{0xFE};
    CHECK_FALSE(peekMessageId(ByteView{unknown}).has_value());
}

TEST_CASE("decode refuses a packet of a different type", "[messages]") {
    const std::vector<std::uint8_t> packet = encode(Ping{});

    CHECK_FALSE(decode<Pong>(ByteView{packet}).has_value());
}

TEST_CASE("decode refuses a truncated packet", "[messages]") {
    std::vector<std::uint8_t> packet = encode(Ping{});
    packet.pop_back();

    CHECK_FALSE(decode<Ping>(ByteView{packet}).has_value());
}

TEST_CASE("decode refuses trailing bytes", "[messages]") {
    // Лишние байты означают, что стороны разошлись в понимании протокола.
    // Молча их игнорировать опаснее, чем отвергнуть пакет.
    std::vector<std::uint8_t> packet = encode(Ping{});
    packet.push_back(0);

    CHECK_FALSE(decode<Ping>(ByteView{packet}).has_value());
}

TEST_CASE("decode refuses an empty packet", "[messages]") {
    CHECK_FALSE(decode<Ping>(ByteView{}).has_value());
}
