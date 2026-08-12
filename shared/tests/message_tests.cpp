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

TEST_CASE("PlayerState survives a round trip", "[messages]") {
    PlayerState sent;
    sent.playerId = 17;
    sent.position = Vec3{-1337.5F, 220.25F, 58.0F};
    sent.heading = 91.5F;
    sent.velocity = Vec3{1.5F, -2.25F, 0.0F};
    sent.health = 175;
    sent.armour = 50;
    sent.flags = PlayerFlag::Aiming | PlayerFlag::InVehicle;
    sent.weapon = 0x1B06D571;
    sent.aimAt = Vec3{10.0F, 20.0F, 30.0F};
    sent.vehicleOwner = 3;
    sent.seat = 1;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->playerId == 17);
    CHECK(received->position == sent.position);
    CHECK(received->heading == 91.5F);
    CHECK(received->velocity == sent.velocity);
    CHECK(received->health == 175);
    CHECK(received->armour == 50);
    CHECK(has(received->flags, PlayerFlag::Aiming));
    CHECK(has(received->flags, PlayerFlag::InVehicle));
    CHECK_FALSE(has(received->flags, PlayerFlag::Dead));
    CHECK(received->weapon == 0x1B06D571);
    CHECK(received->aimAt == sent.aimAt);
    CHECK(received->vehicleOwner == 3);
    CHECK(received->seat == 1);
}

TEST_CASE("PlayerState keeps the driver seat negative", "[messages]") {
    // Место водителя — минус единица, а по сети едет одним беззнаковым байтом.
    // Приведи его обратно неверно — и водитель станет пассажиром на месте 255.
    PlayerState sent;
    sent.seat = kDriverSeat;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->seat == kDriverSeat);
}

TEST_CASE("VehicleState survives a round trip", "[messages]") {
    VehicleState sent;
    sent.owner = 2;
    sent.model = 0x9B909C94;
    sent.position = Vec3{100.5F, -200.25F, 30.0F};
    sent.rotation = Vec3{1.0F, -2.0F, 175.5F};
    sent.velocity = Vec3{12.0F, 0.5F, -0.25F};
    sent.bodyHealth = 640;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->owner == 2);
    CHECK(received->model == 0x9B909C94);
    CHECK(received->position == sent.position);
    CHECK(received->rotation == sent.rotation);
    CHECK(received->velocity == sent.velocity);
    CHECK(received->bodyHealth == 640);
}

TEST_CASE("chat messages survive a round trip", "[messages]") {
    ChatSay say;
    say.text = "привет, как дела";

    const auto receivedSay = roundTrip(say);
    REQUIRE(receivedSay.has_value());
    CHECK(receivedSay->text == "привет, как дела");

    ChatLine line;
    line.kind = ChatKind::Join;
    line.playerId = 0;
    line.nickname = "player";
    line.text = "player (id 0) зашёл на сервер";

    const auto receivedLine = roundTrip(line);
    REQUIRE(receivedLine.has_value());
    CHECK(receivedLine->kind == ChatKind::Join);
    CHECK(receivedLine->playerId == 0);
    CHECK(receivedLine->nickname == "player");
    CHECK(receivedLine->text == line.text);
}

TEST_CASE("damage messages survive a round trip", "[messages]") {
    DamageReport report;
    report.victim = 4;
    report.amount = 35;
    report.weapon = 0x1B06D571;

    const auto receivedReport = roundTrip(report);
    REQUIRE(receivedReport.has_value());
    CHECK(receivedReport->victim == 4);
    CHECK(receivedReport->amount == 35);
    CHECK(receivedReport->weapon == 0x1B06D571);

    DamageTaken taken;
    taken.attacker = 1;
    taken.amount = 35;
    taken.weapon = 0x1B06D571;

    const auto receivedTaken = roundTrip(taken);
    REQUIRE(receivedTaken.has_value());
    CHECK(receivedTaken->attacker == 1);
    CHECK(receivedTaken->amount == 35);
    CHECK(receivedTaken->weapon == 0x1B06D571);
}

TEST_CASE("admin messages survive a round trip", "[messages]") {
    AdminAction action;
    action.command = AdminCommand::Summon;
    action.target = 2;
    action.position = Vec3{10.5F, -20.25F, 30.0F};

    const auto receivedAction = roundTrip(action);
    REQUIRE(receivedAction.has_value());
    CHECK(receivedAction->command == AdminCommand::Summon);
    CHECK(receivedAction->target == 2);
    CHECK(receivedAction->position == action.position);

    AdminOrder order;
    order.command = AdminCommand::Summon;
    order.issuer = 0;
    order.position = action.position;

    const auto receivedOrder = roundTrip(order);
    REQUIRE(receivedOrder.has_value());
    CHECK(receivedOrder->command == AdminCommand::Summon);
    CHECK(receivedOrder->issuer == 0);
    CHECK(receivedOrder->position == order.position);
}

TEST_CASE("an admin action for everyone keeps its empty target", "[messages]") {
    // Пустая цель означает «всем» и обязана дожить до сервера именно пустой:
    // принятая за настоящий номер, она собрала бы к себе одного игрока вместо
    // всех — и никто бы не понял, почему.
    AdminAction sent;
    sent.target = kInvalidPlayerId;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->target == kInvalidPlayerId);
}

TEST_CASE("player zero is a real player", "[messages]") {
    // Номера выдаются с нуля: первый вошедший получает ноль. Значит, «игрока
    // нет» обязано быть чем-то другим — иначе первый же игрок сессии считался
    // бы несуществующим везде, где его номер сверяют с пустым.
    CHECK(kInvalidPlayerId != 0);

    PlayerJoined joined;
    joined.playerId = 0;

    const auto received = roundTrip(joined);

    REQUIRE(received.has_value());
    CHECK(received->playerId == 0);
    CHECK(received->playerId != kInvalidPlayerId);
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
