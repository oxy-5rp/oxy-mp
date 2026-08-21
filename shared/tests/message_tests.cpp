// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oxymp/shared/math/joaat.hpp>
#include <oxymp/shared/protocol/messages.hpp>
#include <oxymp/shared/resource/vault.hpp>

#include <algorithm>
#include <array>
#include <vector>

using namespace oxymp::shared;
using Catch::Approx;

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
    sent.password = "открой";

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->protocolVersion == 7);
    CHECK(received->nickname == "oxy");
    CHECK(received->password == "открой");
}

TEST_CASE("ClientHello without a password survives a round trip", "[messages]") {
    // Сервер без пароля — обычное дело, и пустая строка обязана пережить дорогу
    // так же, как непустая: разбор, споткнувшийся на ней, закрыл бы вход в
    // подавляющее большинство сессий.
    ClientHello sent;
    sent.nickname = "oxy";

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->nickname == "oxy");
    CHECK(received->password.empty());
}

TEST_CASE("ServerWelcome survives a round trip", "[messages]") {
    ServerWelcome sent;
    sent.playerId = 42;
    sent.spawnPosition = Vec3{-1234.5F, 567.25F, 88.0F};
    sent.tickRate = 30;
    sent.name = "Тестовый сервер";

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->playerId == 42);
    CHECK(received->spawnPosition == sent.spawnPosition);
    CHECK(received->tickRate == 30);
    CHECK(received->name == "Тестовый сервер");
}

TEST_CASE("PlayerAppearance survives a round trip", "[messages]") {
    PlayerAppearance sent;
    sent.playerId = 3;
    sent.model = 0x705E61F2;

    sent.components[3] = PedComponent{.drawable = 12, .texture = 4, .palette = 1};
    sent.components[11] = PedComponent{.drawable = 250, .texture = 9, .palette = 0};

    sent.props[0] = PedProp{.drawable = 7, .texture = 2};
    sent.props[4] = PedProp{.drawable = -1, .texture = 0};

    sent.shapeFirst = 21;
    sent.skinSecond = 33;
    sent.shapeMix = 0.25F;
    sent.thirdMix = 0.75F;

    sent.overlays[2] = PedOverlay{
        .index = 5, .colourType = 1, .colour = 3, .secondColour = 4, .opacity = 0.5F};

    sent.hairColour = 6;
    sent.hairHighlight = 7;
    sent.eyeColour = 8;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->playerId == 3);
    CHECK(received->model == 0x705E61F2);

    CHECK(received->components[3].drawable == 12);
    CHECK(received->components[3].texture == 4);
    CHECK(received->components[3].palette == 1);
    CHECK(received->components[11].drawable == 250);

    CHECK(received->props[0].drawable == 7);
    CHECK(received->props[0].texture == 2);

    // Пустой аксессуар — это минус единица, и знак обязан пережить дорогу:
    // байт без знака превратил бы «ничего не надето» в 255-ю шляпу.
    CHECK(received->props[4].drawable == -1);

    CHECK(received->shapeFirst == 21);
    CHECK(received->skinSecond == 33);
    CHECK(received->shapeMix == 0.25F);
    CHECK(received->thirdMix == 0.75F);

    CHECK(received->overlays[2].index == 5);
    CHECK(received->overlays[2].colourType == 1);
    CHECK(received->overlays[2].secondColour == 4);
    CHECK(received->overlays[2].opacity == 0.5F);

    CHECK(received->hairColour == 6);
    CHECK(received->hairHighlight == 7);
    CHECK(received->eyeColour == 8);
}

TEST_CASE("an untouched PlayerAppearance means nothing is worn", "[messages]") {
    // Умолчания важны не меньше заполненного: сервер вправе разослать внешность,
    // о которой ему ничего не сказали, и она обязана означать «как есть», а не
    // «надеть нулевую шляпу».
    const auto received = roundTrip(PlayerAppearance{});

    REQUIRE(received.has_value());
    CHECK(received->model == 0);

    for (const PedProp& prop : received->props) {
        CHECK(prop.drawable == -1);
    }

    for (const PedOverlay& overlay : received->overlays) {
        CHECK(overlay.index == 255);
    }
}

TEST_CASE("a bundle of player states survives a round trip", "[messages]") {
    PlayerStates sent;

    for (std::uint32_t i = 0; i < 3; ++i) {
        PlayerState state;
        state.playerId = i;
        state.sentAt = 1000 + i;
        state.position = Vec3{static_cast<float>(i), 2.0F, 3.0F};
        state.health = static_cast<std::uint16_t>(100 + i);
        sent.players.push_back(state);
    }

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    REQUIRE(received->players.size() == 3);

    for (std::uint32_t i = 0; i < 3; ++i) {
        CHECK(received->players[i].playerId == i);
        CHECK(received->players[i].sentAt == 1000 + i);
        CHECK(received->players[i].position.x == static_cast<float>(i));
        CHECK(received->players[i].health == 100 + i);
    }
}

TEST_CASE("an empty bundle of player states survives a round trip", "[messages]") {
    // Пустая связка не отправляется, но разобраться обязана: испорченный пакет
    // не должен превращаться в отказ разбирать всё остальное.
    const auto received = roundTrip(PlayerStates{});

    REQUIRE(received.has_value());
    CHECK(received->players.empty());
}

TEST_CASE("ServerReject survives a round trip", "[messages]") {
    ServerReject sent;
    sent.reason = RejectReason::ServerFull;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->reason == RejectReason::ServerFull);
}

TEST_CASE("ServerReject carries the wrong password reason", "[messages]") {
    // Отдельным случаем, а не строкой в предыдущем: номер причины закреплён за
    // ней навсегда, и проверка нужна именно за номером — разъехавшись, клиент и
    // сервер назвали бы игроку не ту беду.
    ServerReject sent;
    sent.reason = RejectReason::WrongPassword;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->reason == RejectReason::WrongPassword);
    CHECK(static_cast<std::uint8_t>(received->reason) == 5);
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
    sent.vehicleId = 31;
    sent.seat = 1;
    sent.action = PedAction::HeavyPunch;
    sent.actionSequence = 42;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->playerId == 17);
    CHECK(received->position == sent.position);

    // Угол и скорость едут квантованными, и сравнивать их точно нельзя.
    // Допуск взят по шагу квантования: круг разложен на 65536 делений, скорость
    // — на шестьдесят четыре деления на метр в секунду.
    CHECK(received->heading == Approx(91.5F).margin(0.01F));
    CHECK(received->velocity.x == Approx(sent.velocity.x).margin(0.02F));
    CHECK(received->velocity.y == Approx(sent.velocity.y).margin(0.02F));
    CHECK(received->velocity.z == Approx(sent.velocity.z).margin(0.02F));
    CHECK(received->health == 175);
    CHECK(received->armour == 50);
    CHECK(has(received->flags, PlayerFlag::Aiming));
    CHECK(has(received->flags, PlayerFlag::InVehicle));
    CHECK_FALSE(has(received->flags, PlayerFlag::Dead));
    CHECK(received->weapon == 0x1B06D571);
    CHECK(received->aimAt == sent.aimAt);
    CHECK(received->vehicleId == 31);
    CHECK(received->seat == 1);
    CHECK(received->action == PedAction::HeavyPunch);
    CHECK(received->actionSequence == 42);
}

TEST_CASE("PlayerLoadout survives a round trip", "[messages]") {
    PlayerLoadout sent;
    sent.replace = true;
    sent.weapons = {
        WeaponSlot{.weapon = 0x1B06D571, .ammo = 250},
        WeaponSlot{.weapon = 0x83BF0278, .ammo = 0},
    };

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->replace);
    REQUIRE(received->weapons.size() == 2);
    CHECK(received->weapons[0].weapon == 0x1B06D571);
    CHECK(received->weapons[0].ammo == 250);

    // Ноль патронов — не то же, что отсутствие оружия: пустой ствол в руках
    // остаётся стволом, и потерять эту разницу нельзя.
    CHECK(received->weapons[1].weapon == 0x83BF0278);
    CHECK(received->weapons[1].ammo == 0);
}

TEST_CASE("an overlong loadout is refused rather than trusted", "[messages]") {
    // Длине из пакета верить нельзя: испорченное или враждебное число заставило
    // бы получателя выделить память под список, которого нет.
    ByteWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(MessageId::PlayerLoadout));
    writer.writeU8(0);
    writer.writeU16(kMaxWeaponSlots + 1);

    const auto bytes = std::move(writer).take();
    const auto received = decode<PlayerLoadout>(ByteView{bytes});

    CHECK_FALSE(received.has_value());
}

TEST_CASE("HealthChanged survives a round trip", "[messages]") {
    HealthChanged sent;
    sent.health = 0;
    sent.armour = 37;
    sent.attacker = 4;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());

    // Ноль здоровья — обычное значение, а не «поля нет»: именно им сервер
    // сообщает о смерти.
    CHECK(received->health == 0);
    CHECK(received->armour == 37);
    CHECK(received->attacker == 4);
}

TEST_CASE("PlayerState carries the ammo of the weapon in hand", "[messages]") {
    // Без числа патронов получатель выдавал бы кукле полный магазин, и чужой
    // игрок продолжал бы стрелять ровно тогда, когда хозяин перезаряжается.
    PlayerState sent;
    sent.weapon = 0x1B06D571;
    sent.ammo = 17;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->weapon == 0x1B06D571);
    CHECK(received->ammo == 17);
}

TEST_CASE("VehicleAdded carries the whole vehicle", "[messages]") {
    // Объявление несёт состояние целиком, и это его смысл: получатель обязан
    // суметь показать машину немедленно, не дожидаясь первого снимка. Вложенный
    // снимок обязан пережить дорогу наравне с отдельным.
    VehicleAdded sent;
    sent.state.id = 9;
    sent.state.model = 0x9B909C94;
    sent.state.position = Vec3{1.0F, 2.0F, 3.0F};
    sent.state.rotation = Vec3{0.0F, 0.0F, 90.0F};
    sent.state.bodyHealth = 812;
    sent.state.windowsBroken = 0b0000'0011;
    sent.owner = 4;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->state.id == 9);
    CHECK(received->state.model == 0x9B909C94);
    CHECK(received->state.position == sent.state.position);
    CHECK(received->state.rotation == sent.state.rotation);
    CHECK(received->state.bodyHealth == 812);
    CHECK(received->state.windowsBroken == 0b0000'0011);
    CHECK(received->owner == 4);
}

TEST_CASE("VehicleAuthority carries an absent owner", "[messages]") {
    // «Ведущего нет» — обычное положение вещей, а не поломка: брошенная вдали от
    // всех машина стоит там, где её оставили. Признак этот едет тем же полем,
    // что и настоящий номер игрока, и обязан отличаться от него после дороги.
    VehicleAuthority sent;
    sent.id = 17;
    sent.owner = kInvalidPlayerId;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->id == 17);
    CHECK(received->owner == kInvalidPlayerId);
}

TEST_CASE("PlayerState keeps the driver seat negative", "[messages]") {
    // Место водителя — минус единица, а по сети едет одним беззнаковым байтом.
    // Приведи его обратно неверно — и водитель станет пассажиром на месте 255.
    //
    // Машина здесь обязательна: место без машины в снимок не пишется вовсе —
    // сидеть не в чем, и байт на это тратить незачем.
    PlayerState sent;
    sent.vehicleId = 4;
    sent.seat = kDriverSeat;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->seat == kDriverSeat);
}

TEST_CASE("a snapshot of a walking man carries nothing extra", "[messages]") {
    // Идущий безоружный человек — самый частый случай в сессии, и снимков таких
    // приходит столько, сколько игроков, умноженное на самих себя. Всё, чего у
    // него нет, не должно занимать в снимке ни байта.
    PlayerState walking;
    walking.playerId = 1;
    walking.position = Vec3{1.0F, 2.0F, 3.0F};

    PlayerState armed = walking;
    armed.weapon = 0x1B06D571;
    armed.ammo = 30;

    PlayerState aiming = walking;
    aiming.flags = static_cast<std::uint32_t>(PlayerFlag::Aiming);
    aiming.aimAt = Vec3{4.0F, 5.0F, 6.0F};

    PlayerState driving = walking;
    driving.vehicleId = 9;
    driving.seat = kDriverSeat;

    const std::size_t plain = encode(walking).size();

    CHECK(encode(armed).size() == plain + 6);
    CHECK(encode(aiming).size() == plain + 12);
    CHECK(encode(driving).size() == plain + 5);

    // И само число: снимок идущего обязан оставаться коротким. Проверка не
    // ради числа как такового — ради того, чтобы прибавка к нему не прошла
    // незамеченной.
    CHECK(plain == 38);
}

TEST_CASE("optional fields survive a round trip when present", "[messages]") {
    PlayerState sent;
    sent.playerId = 2;
    sent.flags = static_cast<std::uint32_t>(PlayerFlag::Shooting);
    sent.weapon = 0xDEADBEEF;
    sent.ammo = 250;
    sent.aimAt = Vec3{-7.5F, 8.25F, 9.0F};
    sent.vehicleId = 77;
    sent.seat = 2;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->weapon == 0xDEADBEEF);
    CHECK(received->ammo == 250);
    CHECK(received->aimAt == sent.aimAt);
    CHECK(received->vehicleId == 77);
    CHECK(received->seat == 2);
}

TEST_CASE("an angle survives the round trip through the whole circle", "[messages]") {
    // Углы приходят от игры и отрицательными, и больше трёхсот шестидесяти:
    // приведение к кругу делает запись, и делать его обязана именно она.
    for (const float degrees : {0.0F, 0.5F, 90.0F, 179.9F, 270.25F, 359.9F}) {
        PlayerState sent;
        sent.heading = degrees;

        const auto received = roundTrip(sent);

        REQUIRE(received.has_value());
        CHECK(received->heading == Approx(degrees).margin(0.01F));
    }

    PlayerState negative;
    negative.heading = -90.0F;

    const auto received = roundTrip(negative);

    REQUIRE(received.has_value());
    CHECK(received->heading == Approx(270.0F).margin(0.01F));
}

TEST_CASE("a very fast body does not wrap around in the snapshot", "[messages]") {
    // Предел скорости в снимке — пятьсот метров в секунду с небольшим. Выход за
    // него обязан упереться в предел, а не перевернуться знаком: перевернувшись,
    // падающий самолёт полетел бы у соседа вверх.
    PlayerState sent;
    sent.velocity = Vec3{5000.0F, -5000.0F, 0.0F};

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->velocity.x > 500.0F);
    CHECK(received->velocity.y < -500.0F);
}

TEST_CASE("a vehicle carries its neon, smoke and body extras", "[messages]") {
    VehicleAppearance sent;
    sent.id = 5;
    sent.primaryColour = 12;

    sent.neonSides = NeonSide::Left | NeonSide::Back;
    sent.neonRed = 10;
    sent.neonGreen = 20;
    sent.neonBlue = 30;

    sent.tyreSmokeRed = 40;
    sent.tyreSmokeGreen = 50;
    sent.tyreSmokeBlue = 60;

    sent.extras = 0b0000'0000'0000'1001;
    sent.customTyres = true;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->primaryColour == 12);

    CHECK(has(received->neonSides, NeonSide::Left));
    CHECK(has(received->neonSides, NeonSide::Back));
    CHECK_FALSE(has(received->neonSides, NeonSide::Right));
    CHECK_FALSE(has(received->neonSides, NeonSide::Front));

    CHECK(received->neonRed == 10);
    CHECK(received->neonGreen == 20);
    CHECK(received->neonBlue == 30);

    CHECK(received->tyreSmokeRed == 40);
    CHECK(received->tyreSmokeBlue == 60);

    CHECK(received->extras == 0b0000'0000'0000'1001);
    CHECK(received->customTyres);
}

TEST_CASE("a stock vehicle keeps its neon and smoke at the factory white", "[messages]") {
    // Умолчания важны не меньше заполненного: машина, о внешности которой
    // ничего не сказано, обязана остаться заводской, а не почернеть.
    const auto received = roundTrip(VehicleAppearance{});

    REQUIRE(received.has_value());
    CHECK(received->neonSides == 0);
    CHECK(received->neonRed == 255);
    CHECK(received->tyreSmokeRed == 255);
    CHECK(received->extras == 0);
    CHECK_FALSE(received->customTyres);
}

TEST_CASE("VehicleState survives a round trip", "[messages]") {
    VehicleState sent;
    sent.id = 25;
    sent.model = 0x9B909C94;
    sent.position = Vec3{100.5F, -200.25F, 30.0F};
    sent.rotation = Vec3{1.0F, -2.0F, 175.5F};
    sent.velocity = Vec3{12.0F, 0.5F, -0.25F};
    sent.angularVelocity = Vec3{0.0F, 0.0F, 1.75F};
    sent.steer = -0.5F;
    sent.throttle = 1.0F;
    sent.brake = 0.25F;
    sent.bodyHealth = 640;
    sent.engineHealth = 300;
    sent.tankHealth = 950;
    sent.flags = VehicleFlag::EngineOn | VehicleFlag::LightsOn;
    sent.doorsOpen = 0b0000'0101;
    sent.doorsBroken = 0b0000'0010;
    sent.windowsBroken = 0b1000'0001;
    sent.tyresBurst = 0b0000'1000;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->id == 25);
    CHECK(received->model == 0x9B909C94);
    CHECK(received->position == sent.position);
    CHECK(received->rotation == sent.rotation);
    CHECK(received->velocity == sent.velocity);
    CHECK(received->angularVelocity == sent.angularVelocity);
    CHECK(received->steer == -0.5F);
    CHECK(received->throttle == 1.0F);
    CHECK(received->brake == 0.25F);
    CHECK(received->bodyHealth == 640);
    CHECK(received->engineHealth == 300);
    CHECK(received->tankHealth == 950);
    CHECK(has(received->flags, VehicleFlag::EngineOn));
    CHECK(has(received->flags, VehicleFlag::LightsOn));
    CHECK_FALSE(has(received->flags, VehicleFlag::SirenOn));
    CHECK(received->doorsOpen == 0b0000'0101);
    CHECK(received->doorsBroken == 0b0000'0010);
    CHECK(received->windowsBroken == 0b1000'0001);
    CHECK(received->tyresBurst == 0b0000'1000);
}

TEST_CASE("VehicleAppearance survives a round trip", "[messages]") {
    VehicleAppearance sent;
    sent.id = 42;
    sent.primaryColour = 12;
    sent.secondaryColour = 111;
    sent.pearlescentColour = 3;
    sent.wheelColour = 156;
    sent.plate = "OXYMP 1";
    sent.plateStyle = 2;
    sent.livery = 4;
    sent.wheelType = 7;
    sent.windowTint = 1;
    sent.dirtLevel = 8.5F;
    sent.mods[0] = 3;
    sent.mods[kVehicleModSlots - 1] = 11;
    sent.toggleMods = 1U << 18U;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->id == 42);
    CHECK(received->primaryColour == 12);
    CHECK(received->secondaryColour == 111);
    CHECK(received->pearlescentColour == 3);
    CHECK(received->wheelColour == 156);
    CHECK(received->plate == "OXYMP 1");
    CHECK(received->plateStyle == 2);
    CHECK(received->livery == 4);
    CHECK(received->wheelType == 7);
    CHECK(received->windowTint == 1);
    CHECK(received->dirtLevel == 8.5F);
    CHECK(received->mods[0] == 3);
    CHECK(received->mods[1] == kStockMod);
    CHECK(received->mods[kVehicleModSlots - 1] == 11);
    CHECK(received->toggleMods == 1U << 18U);
}

TEST_CASE("VehicleAppearance keeps stock places stock", "[messages]") {
    // Заводское место обозначено минус единицей, а едет беззнаковым байтом.
    // Приведи его обратно неверно — и на машине окажется деталь номер 255,
    // которой у модели нет; игра на такое отвечает по-разному, и ни один из
    // ответов не является тем, что хотел отправитель.
    const VehicleAppearance sent;

    const auto received = roundTrip(sent);

    REQUIRE(received.has_value());
    CHECK(received->livery == kStockMod);
    CHECK(received->wheelType == kStockMod);
    CHECK(received->windowTint == kStockMod);
    CHECK(std::ranges::all_of(received->mods, [](std::int8_t mod) { return mod == kStockMod; }));
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

TEST_CASE("money survives a round trip", "[messages]") {
    MoneyChanged money;
    money.amount = 100'000;

    const auto received = roundTrip(money);
    REQUIRE(received.has_value());
    CHECK(received->amount == 100'000);
}

TEST_CASE("a debt stays a debt", "[messages]") {
    // Отрицательные деньги — обычное дело, а по проводу они идут беззнаковым
    // числом. Ошибись в обратном превращении — и долг в сто тысяч превратился бы
    // в восемнадцать квинтиллионов.
    MoneyChanged owed;
    owed.amount = -2'500;

    const auto received = roundTrip(owed);
    REQUIRE(received.has_value());
    CHECK(received->amount == -2'500);
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

// --- Ресурсы ------------------------------------------------------------------

TEST_CASE("a resource survives packing and unpacking", "[vault]") {
    const std::string original = "RPF7 это притворяется файлом игры";
    const std::span<const std::uint8_t> plain{
        reinterpret_cast<const std::uint8_t*>(original.data()), original.size()};

    const std::vector<std::uint8_t> key = Vault::builtInKey();
    const std::vector<std::uint8_t> packed = Vault::pack(plain, key);

    // Содержимое обязано перестать быть видимым: иначе весь смысл теряется.
    const std::string asText(reinterpret_cast<const char*>(packed.data()), packed.size());
    CHECK(asText.find("RPF7") == std::string::npos);

    std::string error;
    const std::vector<std::uint8_t> restored = Vault::unpack(packed, key, error);

    REQUIRE(error.empty());
    REQUIRE(restored.size() == original.size());

    // Сравнение через беззнаковый вид: char знаковый, и кириллица в нём
    // отрицательна — сравнение с байтом дало бы ложное расхождение.
    const std::string restoredText(reinterpret_cast<const char*>(restored.data()),
                                   restored.size());
    CHECK(restoredText == original);
}

TEST_CASE("a resource refuses a wrong key", "[vault]") {
    // Расшифровка чужим ключом не отказывает сама по себе — она молча даёт
    // мусор. Отпечаток для того и лежит в заголовке, чтобы этот мусор не уехал
    // дальше под видом содержимого.
    const std::array<std::uint8_t, 4> original = {1, 2, 3, 4};

    std::vector<std::uint8_t> key = Vault::builtInKey();
    const std::vector<std::uint8_t> packed = Vault::pack(original, key);

    key[0] ^= 0xFF;

    std::string error;
    const std::vector<std::uint8_t> restored = Vault::unpack(packed, key, error);

    CHECK(restored.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("packing the same thing twice gives the same name", "[vault]") {
    // От этого зависит, будет ли клиент качать заново то, что не менялось.
    const std::array<std::uint8_t, 3> data = {9, 9, 9};

    CHECK(fingerprint(data) == fingerprint(data));
    CHECK(fingerprint(data).size() == 64);
}

TEST_CASE("the name hash matches what the game computes", "[protocol]") {
    // Сверка с тем, что возвращает GET_HASH_KEY в самой игре: эти два значения
    // клиент уже проверяет у себя при запуске, и разойтись с ними нельзя —
    // сервер называет модель по имени, а создаёт её игра по числу.
    CHECK(joaat("mp_m_freemode_01") == 0x705E61F2);
    CHECK(joaat("WEAPON_UNARMED") == 0xA2719263);

    // Регистр не значит ничего: игра приводит имя к нижнему сама.
    CHECK(joaat("Adder") == joaat("adder"));

    // Пустое имя даёт ноль, и это удобно: ноль у нас всюду означает «модели
    // нет», и проверять пустую строку отдельно не приходится.
    CHECK(joaat("") == 0);
}

TEST_CASE("a named event survives the trip", "[protocol]") {
    ClientEvent event;
    event.name = "admin:vehicle";
    event.payload = R"({"model":"adder"})";

    const auto packet = encode(event);
    const auto back = decode<ClientEvent>(ByteView{packet});

    REQUIRE(back);
    CHECK(back->name == event.name);
    CHECK(back->payload == event.payload);
}

TEST_CASE("an overlong event is cut, not refused", "[protocol]") {
    ServerEvent event;
    event.name = std::string(kMaxEventNameLength + 20, 'x');
    event.payload = std::string(kMaxEventPayloadLength + 500, 'y');

    // Обрезается у того, кто сочинил: отвергни получатель пакет целиком —
    // отправитель об этом не узнал бы, а страница интерфейса молча замерла бы.
    const auto packet = encode(event);
    const auto back = decode<ServerEvent>(ByteView{packet});

    REQUIRE(back);
    CHECK(back->name.size() == kMaxEventNameLength);
    CHECK(back->payload.size() == kMaxEventPayloadLength);
}

TEST_CASE("a teleport carries only where to", "[protocol]") {
    PlayerTeleport teleport;
    teleport.position = Vec3{.x = 1.5F, .y = -2.5F, .z = 3.5F};

    const auto packet = encode(teleport);
    const auto back = decode<PlayerTeleport>(ByteView{packet});

    REQUIRE(back);
    CHECK(back->position.y == -2.5F);
}

TEST_CASE("a resource list of a real game mode survives a round trip", "[messages]") {
    // Раздаётся не «ресурс», а каждый его файл: у живого режима клиентская
    // половина — это собранная страница интерфейса, полторы-две тысячи картинок,
    // шрифтов и кусков сборки. Прежний предел в пятьсот двенадцать обрезал
    // список молча, и клиент открывал страницу, которой нет.
    ResourceList list;

    for (int i = 0; i < 1879; ++i) {
        list.entries.push_back(ResourceEntry{.name = std::format("main/client/ui/file{}.js", i),
                                             .hash = std::string(64, 'a'),
                                             .size = 1024,
                                             .page = i == 0});
    }

    ByteWriter writer;
    list.write(writer);

    ByteReader reader{ByteView{writer.bytes()}};
    const ResourceList restored = ResourceList::read(reader);

    CHECK(reader.ok());
    CHECK(reader.exhausted());
    REQUIRE(restored.entries.size() == 1879);
    CHECK(restored.entries.front().page);
    CHECK(restored.entries.back().name == "main/client/ui/file1878.js");
}

TEST_CASE("a vehicle teleport survives the round trip", "[messages]") {
    VehicleTeleport sent;
    sent.id = 0x00010007;
    sent.position = Vec3{.x = -1037.7F, .y = -2738.0F, .z = 20.2F};
    sent.heading = 328.0F;

    const auto packet = encode(sent);
    const auto got = decode<VehicleTeleport>(packet);

    REQUIRE(got);
    CHECK(got->id == sent.id);
    CHECK(got->position.x == Catch::Approx(sent.position.x));
    CHECK(got->position.y == Catch::Approx(sent.position.y));
    CHECK(got->position.z == Catch::Approx(sent.position.z));

    // Угол квантован шестью тысячными градуса — так он и ходит по сети.
    CHECK(got->heading == Catch::Approx(sent.heading).margin(0.01));
}

TEST_CASE("a vehicle repair survives the round trip", "[messages]") {
    VehicleRepair sent;
    sent.id = 0x00020003;

    const auto packet = encode(sent);
    const auto got = decode<VehicleRepair>(packet);

    REQUIRE(got);
    CHECK(got->id == sent.id);
}

TEST_CASE("a vehicle command is recognised by its first byte", "[messages]") {
    const auto teleport = encode(VehicleTeleport{});
    const auto repair = encode(VehicleRepair{});

    CHECK(peekMessageId(teleport) == MessageId::VehicleTeleport);
    CHECK(peekMessageId(repair) == MessageId::VehicleRepair);
}

TEST_CASE("a blip survives the round trip", "[messages]") {
    BlipState sent;
    sent.id = 7;
    sent.position = Vec3{.x = 100.5F, .y = -200.25F, .z = 30.0F};
    sent.sprite = 402;
    sent.colour = 5;
    sent.alpha = 200;
    sent.display = 4;
    sent.shortRange = true;
    sent.scale = 0.8F;
    sent.name = "Банк";

    const auto packet = encode(sent);
    const auto got = decode<BlipState>(packet);

    REQUIRE(got);
    CHECK(got->id == sent.id);
    CHECK(got->position.x == Catch::Approx(sent.position.x));
    CHECK(got->sprite == sent.sprite);
    CHECK(got->colour == sent.colour);
    CHECK(got->alpha == sent.alpha);
    CHECK(got->display == sent.display);
    CHECK(got->shortRange == sent.shortRange);
    CHECK(got->scale == Catch::Approx(sent.scale));
    CHECK(got->name == sent.name);
}

TEST_CASE("a removed blip survives the round trip", "[messages]") {
    BlipRemoved sent;
    sent.id = 42;

    const auto packet = encode(sent);
    const auto got = decode<BlipRemoved>(packet);

    REQUIRE(got);
    CHECK(got->id == sent.id);
}

TEST_CASE("a marker survives the round trip", "[messages]") {
    MarkerState sent;
    sent.id = 3;
    sent.type = 27;
    sent.position = Vec3{.x = 10.0F, .y = 20.0F, .z = 30.0F};
    sent.rotation = Vec3{.x = 0.0F, .y = 0.0F, .z = 90.0F};
    sent.direction = Vec3{.x = 1.0F, .y = 0.0F, .z = 0.0F};
    sent.scale = Vec3{.x = 2.0F, .y = 2.0F, .z = 1.5F};
    sent.red = 10;
    sent.green = 20;
    sent.blue = 30;
    sent.alpha = 40;
    sent.visible = true;
    sent.bobUpAndDown = true;
    sent.faceCamera = false;
    sent.rotate = true;
    sent.streamingDistance = 75.0F;

    const auto packet = encode(sent);
    const auto got = decode<MarkerState>(packet);

    REQUIRE(got);
    CHECK(got->id == sent.id);
    CHECK(got->type == sent.type);
    CHECK(got->position.z == Catch::Approx(sent.position.z));
    CHECK(got->rotation.z == Catch::Approx(sent.rotation.z));
    CHECK(got->direction.x == Catch::Approx(sent.direction.x));
    CHECK(got->scale.y == Catch::Approx(sent.scale.y));
    CHECK(got->red == sent.red);
    CHECK(got->green == sent.green);
    CHECK(got->blue == sent.blue);
    CHECK(got->alpha == sent.alpha);
    CHECK(got->streamingDistance == Catch::Approx(sent.streamingDistance));
}

// Признаки едут одним байтом, и перепутать их местами легче всего именно там.
// Проверяется каждый по отдельности: набор, где все четыре подняты, прошёл бы и
// при перепутанных битах.
TEST_CASE("marker flags do not bleed into each other", "[messages]") {
    MarkerState sent;
    sent.visible = false;
    sent.bobUpAndDown = false;
    sent.faceCamera = true;
    sent.rotate = false;

    const auto got = decode<MarkerState>(encode(sent));

    REQUIRE(got);
    CHECK_FALSE(got->visible);
    CHECK_FALSE(got->bobUpAndDown);
    CHECK(got->faceCamera);
    CHECK_FALSE(got->rotate);
}

TEST_CASE("a removed marker survives the round trip", "[messages]") {
    MarkerRemoved sent;
    sent.id = 11;

    const auto got = decode<MarkerRemoved>(encode(sent));

    REQUIRE(got);
    CHECK(got->id == sent.id);
}

TEST_CASE("a checkpoint survives the round trip", "[messages]") {
    CheckpointState sent;
    sent.id = 5;
    sent.type = 4;
    sent.position = Vec3{.x = -100.0F, .y = 50.0F, .z = 12.5F};
    sent.nextPosition = Vec3{.x = -80.0F, .y = 50.0F, .z = 12.5F};
    sent.radius = 4.5F;
    sent.height = 3.0F;
    sent.red = 1;
    sent.green = 2;
    sent.blue = 3;
    sent.alpha = 4;
    sent.iconRed = 5;
    sent.iconGreen = 6;
    sent.iconBlue = 7;
    sent.iconAlpha = 8;
    sent.visible = false;
    sent.streamingDistance = 120.0F;

    const auto got = decode<CheckpointState>(encode(sent));

    REQUIRE(got);
    CHECK(got->id == sent.id);
    CHECK(got->type == sent.type);
    CHECK(got->position.x == Catch::Approx(sent.position.x));
    CHECK(got->nextPosition.x == Catch::Approx(sent.nextPosition.x));
    CHECK(got->radius == Catch::Approx(sent.radius));
    CHECK(got->height == Catch::Approx(sent.height));
    CHECK(got->red == sent.red);
    CHECK(got->iconRed == sent.iconRed);
    CHECK(got->iconAlpha == sent.iconAlpha);
    CHECK_FALSE(got->visible);
    CHECK(got->streamingDistance == Catch::Approx(sent.streamingDistance));
}

TEST_CASE("a removed checkpoint survives the round trip", "[messages]") {
    CheckpointRemoved sent;
    sent.id = 9;

    const auto got = decode<CheckpointRemoved>(encode(sent));

    REQUIRE(got);
    CHECK(got->id == sent.id);
}

// Номер сообщения читается прежде его полей, и по нему получатель решает, чем
// разбирать пришедшее. Незнакомый номер здесь — это не ошибка разбора, а тихо
// потерянное сообщение.
TEST_CASE("markers and checkpoints are recognised by their message id", "[messages]") {
    CHECK(peekMessageId(encode(MarkerState{})) == MessageId::MarkerState);
    CHECK(peekMessageId(encode(MarkerRemoved{})) == MessageId::MarkerRemoved);
    CHECK(peekMessageId(encode(CheckpointState{})) == MessageId::CheckpointState);
    CHECK(peekMessageId(encode(CheckpointRemoved{})) == MessageId::CheckpointRemoved);
}

TEST_CASE("an animation survives the round trip", "[messages]") {
    PlayerAnimation sent;
    sent.playerId = 4;
    sent.dictionary = "amb@world_human_hang_out_street@male_c@base";
    sent.name = "base";
    sent.blendIn = 4.0F;
    sent.blendOut = -4.0F;
    sent.duration = 5000;
    sent.flags = 1;
    sent.playbackRate = 0.5F;
    sent.locks = static_cast<std::uint8_t>(AnimationLock::X) |
                 static_cast<std::uint8_t>(AnimationLock::Z);

    const auto got = decode<PlayerAnimation>(encode(sent));

    REQUIRE(got);
    CHECK(got->playerId == sent.playerId);
    CHECK(got->dictionary == sent.dictionary);
    CHECK(got->name == sent.name);
    CHECK(got->blendIn == Catch::Approx(sent.blendIn));
    CHECK(got->blendOut == Catch::Approx(sent.blendOut));
    CHECK(got->duration == sent.duration);
    CHECK(got->flags == sent.flags);
    CHECK(got->playbackRate == Catch::Approx(sent.playbackRate));
    CHECK(has(got->locks, AnimationLock::X));
    CHECK_FALSE(has(got->locks, AnimationLock::Y));
    CHECK(has(got->locks, AnimationLock::Z));
}

// Минус единица в длительности означает «до конца», и через сеть она ходит
// беззнаковой. Перепутанное знаковое расширение превратило бы её в четыре
// миллиарда миллисекунд — то есть в движение длиной в полтора месяца.
TEST_CASE("an endless animation keeps its minus one", "[messages]") {
    PlayerAnimation sent;
    sent.duration = -1;

    const auto got = decode<PlayerAnimation>(encode(sent));

    REQUIRE(got);
    CHECK(got->duration == -1);
}

TEST_CASE("an animation is recognised by its message id", "[messages]") {
    CHECK(peekMessageId(encode(PlayerAnimation{})) == MessageId::PlayerAnimation);
}

TEST_CASE("an attachment survives the round trip", "[messages]") {
    EntityAttachment sent;
    sent.kind = EntityKind::Object;
    sent.id = 7;
    sent.targetKind = EntityKind::Player;
    sent.target = 0;
    sent.bone = 28422;
    sent.boneName = "SKEL_R_Hand";
    sent.position = Vec3{.x = 0.1F, .y = -0.2F, .z = 0.3F};
    sent.rotation = Vec3{.x = 90.0F, .y = 0.0F, .z = -45.0F};
    sent.collision = true;
    sent.fixedRotation = false;

    const auto got = decode<EntityAttachment>(encode(sent));

    REQUIRE(got);
    CHECK(*got == sent);

    // Ноль здесь — законный номер игрока, а не «никого»: у игроков пустой номер
    // это единицы во всех разрядах. Отвязку от привязки отличает род цели.
    CHECK(got->targetKind == EntityKind::Player);
    CHECK(got->target == 0);
}

// Кость «сама сущность» ходит по сети беззнаковой, как и длительность движения.
// Перепутанное знаковое расширение сделало бы из неё кость номер четыре
// миллиарда, и привязанное улетело бы в начало координат.
TEST_CASE("attaching to the entity itself keeps its minus one", "[messages]") {
    EntityAttachment sent;
    sent.bone = -1;

    const auto got = decode<EntityAttachment>(encode(sent));

    REQUIRE(got);
    CHECK(got->bone == -1);
}

TEST_CASE("a detached entity names no target", "[messages]") {
    EntityAttachment sent;
    sent.kind = EntityKind::Vehicle;
    sent.id = 3;

    const auto got = decode<EntityAttachment>(encode(sent));

    REQUIRE(got);
    CHECK(got->kind == EntityKind::Vehicle);
    CHECK(got->targetKind == EntityKind::None);
}

// Род сущности приходит числом, и числом этим распоряжается отправитель. Род,
// которого мы не знаем, обязан прочитаться как «ни к кому»: он решает, в каком
// списке искать сущность, и незнакомое число ушло бы в этот выбор.
TEST_CASE("an attachment to a kind we do not know reads as detached", "[messages]") {
    std::vector<std::uint8_t> packet = encode(EntityAttachment{});

    // Первый байт — номер сообщения, за ним род привязываемого.
    packet[1] = 99;

    const auto got = decode<EntityAttachment>(ByteView{packet});

    REQUIRE(got);
    CHECK(got->kind == EntityKind::None);
}

TEST_CASE("an attachment is recognised by its message id", "[messages]") {
    CHECK(peekMessageId(encode(EntityAttachment{})) == MessageId::EntityAttachment);
}

TEST_CASE("a ped survives the round trip", "[messages]") {
    PedState sent;
    sent.id = 4;
    sent.model = 0x9C9EFFD8;
    sent.position = Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F};
    sent.rotation = Vec3{.x = 0.0F, .y = 0.0F, .z = 90.0F};
    sent.health = 150;
    sent.maxHealth = 300;
    sent.armour = 50;
    sent.weapon = 0x1B06D571;

    const auto got = decode<PedState>(encode(sent));

    REQUIRE(got);
    CHECK(*got == sent);
}

TEST_CASE("peds are recognised by their message ids", "[messages]") {
    CHECK(peekMessageId(encode(PedState{})) == MessageId::PedState);
    CHECK(peekMessageId(encode(PedRemoved{})) == MessageId::PedRemoved);
}

// Прохожий стал четвёртым родом сущности, и род этот ходит по сети числом.
// Приписав его в конец, мы не тронули номеров остальных трёх — а тронув, сломали
// бы всякую уже отправленную привязку.
TEST_CASE("a ped is a kind an attachment can name", "[messages]") {
    EntityAttachment sent;
    sent.kind = EntityKind::Object;
    sent.id = 1;
    sent.targetKind = EntityKind::Ped;
    sent.target = 7;

    const auto got = decode<EntityAttachment>(encode(sent));

    REQUIRE(got);
    CHECK(got->targetKind == EntityKind::Ped);
    CHECK(got->target == 7);
}

TEST_CASE("a weapon carries its components and tint", "[messages]") {
    PlayerLoadout sent;
    sent.replace = true;
    sent.weapons.push_back(WeaponSlot{
        .weapon = 0x1B06D571,
        .ammo = 250,
        .tint = 4,
        .components = {0xC0A3098D, 0xA0D89C42},
    });

    // Второй ствол — без насадок вовсе: у пустого списка своя длина, и спутать
    // её с длиной соседа проще всего именно здесь.
    sent.weapons.push_back(WeaponSlot{.weapon = 0x83BF0278, .ammo = 30});

    const auto got = decode<PlayerLoadout>(encode(sent));

    REQUIRE(got);
    REQUIRE(got->weapons.size() == 2);

    CHECK(got->replace);
    CHECK(got->weapons[0].weapon == sent.weapons[0].weapon);
    CHECK(got->weapons[0].ammo == sent.weapons[0].ammo);
    CHECK(got->weapons[0].tint == 4);
    CHECK(got->weapons[0].components == sent.weapons[0].components);

    CHECK(got->weapons[1].weapon == sent.weapons[1].weapon);
    CHECK(got->weapons[1].tint == 0);
    CHECK(got->weapons[1].components.empty());
}

// Длина списка насадок приходит из пакета, и верить ей нельзя: испорченное
// число заставило бы получателя выделить память под список, которого нет.
TEST_CASE("a weapon takes no more components than it can hold", "[messages]") {
    PlayerLoadout sent;

    WeaponSlot slot;
    slot.weapon = 0x1B06D571;

    for (std::uint32_t part = 1; part <= kMaxWeaponComponents + 4; ++part) {
        slot.components.push_back(part);
    }

    sent.weapons.push_back(slot);

    const auto got = decode<PlayerLoadout>(encode(sent));

    REQUIRE(got);
    REQUIRE(got->weapons.size() == 1);
    CHECK(got->weapons[0].components.size() == kMaxWeaponComponents);
}

// Своя краска отличается от «краски нет», и отличается признаком, а не цветом:
// чёрная краска осмысленна, и по трём нулям её от снятой не отличить.
TEST_CASE("a custom paint keeps itself apart from no paint at all", "[messages]") {
    VehicleAppearance painted;
    painted.id = 3;
    painted.customPrimary = true;
    painted.customPrimaryRed = 0;
    painted.customPrimaryGreen = 0;
    painted.customPrimaryBlue = 0;
    painted.customSecondary = true;
    painted.customSecondaryRed = 10;
    painted.customSecondaryGreen = 20;
    painted.customSecondaryBlue = 30;

    const auto got = decode<VehicleAppearance>(encode(painted));

    REQUIRE(got);
    CHECK(got->customPrimary);
    CHECK(got->customPrimaryRed == 0);
    CHECK(got->customSecondary);
    CHECK(got->customSecondaryGreen == 20);

    VehicleAppearance bare;
    bare.id = 3;

    const auto plain = decode<VehicleAppearance>(encode(bare));

    REQUIRE(plain);
    CHECK_FALSE(plain->customPrimary);
    CHECK_FALSE(plain->customSecondary);

    // И сравнение их различает: внешность уходит только при изменении, и
    // неразличённая краска не уехала бы вовсе.
    CHECK_FALSE(*got == *plain);
}

// Точка взгляда едет не точкой, а двумя углами, и собирается обратно из головы
// отправителя. Проверяется поэтому не совпадение чисел, а то, что взгляд
// смотрит туда же: направление то же, удаление то же.
TEST_CASE("a look point survives the round trip as two angles", "[messages]") {
    PlayerState sent;
    sent.position = Vec3{.x = 100.0F, .y = -200.0F, .z = 30.0F};

    // Голова смотрит на северо-восток и слегка вверх.
    const Vec3 head{sent.position.x, sent.position.y, sent.position.z + kLookHeight};
    sent.aimAt = Vec3{head.x + 14.0F, head.y + 14.0F, head.z + 3.0F};

    const auto got = decode<PlayerState>(encode(sent));

    REQUIRE(got);

    const float dx = got->aimAt.x - head.x;
    const float dy = got->aimAt.y - head.y;
    const float dz = got->aimAt.z - head.z;

    // Удаление приводится к общему: отправитель волен ставить точку где угодно,
    // а по сети едет только направление.
    CHECK(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) == Catch::Approx(kLookRange).margin(0.1));

    // Направление то же, что и было: сравниваются доли, а не метры.
    const float length = std::sqrt(14.0F * 14.0F + 14.0F * 14.0F + 3.0F * 3.0F);

    CHECK(dx / kLookRange == Catch::Approx(14.0F / length).margin(0.01));
    CHECK(dy / kLookRange == Catch::Approx(14.0F / length).margin(0.01));
    CHECK(dz / kLookRange == Catch::Approx(3.0F / length).margin(0.01));
}

// Целящийся возит точку целиком: она в мире, а не в двадцати метрах от головы.
TEST_CASE("an aiming player still sends the whole point", "[messages]") {
    PlayerState sent;
    sent.position = Vec3{.x = 10.0F, .y = 20.0F, .z = 30.0F};
    sent.flags = static_cast<std::uint32_t>(PlayerFlag::Aiming);
    sent.aimAt = Vec3{.x = 110.0F, .y = 20.0F, .z = 31.0F};

    const auto got = decode<PlayerState>(encode(sent));

    REQUIRE(got);
    CHECK(got->aimAt.x == Catch::Approx(sent.aimAt.x));
    CHECK(got->aimAt.y == Catch::Approx(sent.aimAt.y));
    CHECK(got->aimAt.z == Catch::Approx(sent.aimAt.z));
}

// Клиент, точку не заполняющий вовсе, платить за неё не должен — и не платит:
// нулевая точка признака не поднимает.
TEST_CASE("a player who looks nowhere pays nothing for it", "[messages]") {
    PlayerState blind;
    blind.position = Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F};

    PlayerState looking = blind;
    looking.aimAt = Vec3{.x = 1.0F, .y = 22.0F, .z = 3.65F};

    CHECK(encode(looking).size() == encode(blind).size() + 4);

    const auto got = decode<PlayerState>(encode(blind));

    REQUIRE(got);
    CHECK(got->aimAt == Vec3{});
}
