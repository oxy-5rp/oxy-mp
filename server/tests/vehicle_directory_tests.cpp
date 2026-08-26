#include "vehicle_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace oxymp;
using namespace oxymp::server;

namespace {

/// Предел числа машин, не мешающий проверке.
constexpr std::size_t kPlenty = 64;

/// Игрок, стоящий в указанной точке.
VehicleDirectory::PlayerPlacement at(shared::PlayerId id, float x, float y = 0.0F) {
    return VehicleDirectory::PlayerPlacement{
        .id = id,
        .position = shared::Vec3{.x = x, .y = y, .z = 0.0F},
    };
}

/// Заводит машину в указанной точке и сразу отдаёт её номер.
shared::VehicleId spawn(VehicleDirectory& directory, shared::PlayerId owner, float x,
                        float y = 0.0F) {
    return directory.add(0xDEADBEEF, shared::Vec3{.x = x, .y = y, .z = 0.0F}, 0.0F, owner, kPlenty);
}

} // namespace

TEST_CASE("moving a vehicle marks it for the next broadcast", "[server][vehicles]") {
    // Рассылка решает по этой отметке, кого пересылать в такте. Без неё
    // переставленная скриптом машина доезжала только до вошедших позже: тем,
    // кто уже на неё смотрит, её не показывали.
    //
    // Ведущего это не касается — ему уходит отдельная просьба, и снимок он
    // пришлёт сам. А у брошенной машины ведущего нет, и прислать снимок о ней
    // некому.
    VehicleDirectory vehicles;

    const shared::VehicleId id = spawn(vehicles, shared::kInvalidPlayerId, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    const VehicleDirectory::Vehicle* const parked = vehicles.find(id);
    REQUIRE(parked != nullptr);

    const auto before = parked->stateAt;

    REQUIRE(vehicles.place(id, shared::Vec3{.x = 100.0F, .y = 200.0F, .z = 30.0F}, 90.0F));

    const VehicleDirectory::Vehicle* const moved = vehicles.find(id);
    REQUIRE(moved != nullptr);

    CHECK(moved->stateAt > before);
    CHECK(moved->state.position.x == 100.0F);

    // И скорость обнулилась: переставленная на ходу машина, сохранив её,
    // поехала бы на новом месте сама.
    CHECK(moved->state.velocity.x == 0.0F);
}

TEST_CASE("repairing a vehicle marks it for the next broadcast", "[server][vehicles]") {
    // По той же причине: прочности изменились, а рассылка узнаёт об этом только
    // по отметке.
    VehicleDirectory vehicles;

    const shared::VehicleId id = spawn(vehicles, shared::kInvalidPlayerId, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    const auto before = vehicles.find(id)->stateAt;

    REQUIRE(vehicles.repair(id));

    const VehicleDirectory::Vehicle* const fixed = vehicles.find(id);
    REQUIRE(fixed != nullptr);

    CHECK(fixed->stateAt > before);
    CHECK(fixed->state.bodyHealth == shared::kFullVehicleHealth);
    CHECK(fixed->state.tyresBurst == 0);
}


TEST_CASE("vehicle numbers are issued by the server and never repeat", "[vehicles]") {
    VehicleDirectory directory;

    const shared::VehicleId first = spawn(directory, 0, 0.0F);
    const shared::VehicleId second = spawn(directory, 0, 0.0F);

    CHECK(first != shared::kInvalidVehicleId);
    CHECK(second != shared::kInvalidVehicleId);
    CHECK(first != second);

    // Номер убранной машины не выдаётся повторно. Иначе снимок, отставший от
    // своей машины на пару кадров, попал бы в другую, занявшую её номер.
    REQUIRE(directory.remove(second));

    const shared::VehicleId third = spawn(directory, 0, 0.0F);
    CHECK(third != second);
}

TEST_CASE("the vehicle limit is honoured", "[vehicles]") {
    VehicleDirectory directory;

    CHECK(directory.add(0xDEADBEEF, {}, 0.0F, 0, 1) != shared::kInvalidVehicleId);

    // Предел не от жадности: каждая машина заводится по-настоящему у каждого
    // клиента, а игра держит на свете ограниченное их число.
    CHECK(directory.add(0xDEADBEEF, {}, 0.0F, 0, 1) == shared::kInvalidVehicleId);

    // Модель нулём — не машина. Отказать честнее, чем завести пустышку, которую
    // никто не сможет показать.
    VehicleDirectory empty;
    CHECK(empty.add(0, {}, 0.0F, 0, kPlenty) == shared::kInvalidVehicleId);
}

TEST_CASE("only the owner may report a vehicle", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 3, 0.0F);

    shared::VehicleState state;
    state.id = id;
    state.position = shared::Vec3{.x = 50.0F, .y = 0.0F, .z = 0.0F};

    // Чужой снимок отбрасывается целиком: это либо отставший снимок того, у кого
    // машину уже забрали, либо ошибка в клиенте. Принятый, он дёрнул бы машину.
    CHECK_FALSE(directory.applyState(4, state));
    CHECK(directory.find(id)->state.position.x == 0.0F);

    CHECK(directory.applyState(3, state));
    CHECK(directory.find(id)->state.position.x == 50.0F);
}

TEST_CASE("a snapshot cannot change the model of a vehicle", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 0, 0.0F);

    shared::VehicleState state;
    state.id = id;
    state.model = 0x11111111;

    REQUIRE(directory.applyState(0, state));

    // Машина заводится один раз и с одной моделью. Позволь снимку менять её — и
    // ведущий превратил бы чужую машину во что угодно, а ведущим побывает каждый.
    CHECK(directory.find(id)->state.model == 0xDEADBEEF);
}

TEST_CASE("the driver always leads the vehicle", "[vehicles]") {
    VehicleDirectory directory;

    // Машина заведена далеко от водителя, а рядом с ней стоит другой игрок.
    const shared::VehicleId id = spawn(directory, 1, 100.0F);

    directory.setSeat(2, id, shared::kDriverSeat);

    const std::vector players{at(1, 105.0F), at(2, 100.0F)};
    const auto changes = directory.reassign(players);

    REQUIRE(changes.size() == 1);
    CHECK(changes.front().id == id);

    // Ехать будет водитель, а считать поездку — кто-то другой: разъедутся они
    // мгновенно. Поэтому расстояние здесь ничего не решает.
    CHECK(directory.find(id)->owner == 2);
}

TEST_CASE("a passenger does not take the vehicle from the driver", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 1, 0.0F);

    directory.setSeat(1, id, shared::kDriverSeat);
    directory.setSeat(2, id, 0);

    // Ничего не меняется: машина уже за водителем, и пассажир её не отбирает.
    const std::vector players{at(1, 0.0F), at(2, 0.0F)};
    CHECK(directory.reassign(players).empty());

    CHECK(directory.find(id)->owner == 1);
}

TEST_CASE("an abandoned vehicle goes to the nearest player", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 1, 0.0F);

    // Ведущий ушёл из сессии. Машина остаётся — в ней могли остаться пассажиры.
    directory.forgetPlayer(1);
    CHECK(directory.find(id)->owner == shared::kInvalidPlayerId);

    const std::vector players{at(5, 200.0F), at(6, 20.0F)};
    const auto changes = directory.reassign(players);

    REQUIRE(changes.size() == 1);
    CHECK(changes.front().owner == 6);
    CHECK(directory.find(id)->owner == 6);
}

TEST_CASE("a vehicle beyond reach is led by nobody", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 1, 0.0F);

    // Ведущий обязан видеть машину по-настоящему: физику ей считает игра, а игра
    // считает её только тому, что у неё загружено.
    const std::vector players{at(1, VehicleDirectory::kOwnershipRange + 10.0F)};
    const auto changes = directory.reassign(players);

    REQUIRE(changes.size() == 1);
    CHECK(changes.front().owner == shared::kInvalidPlayerId);
    CHECK(directory.find(id)->owner == shared::kInvalidPlayerId);

    // «Ведущего нет» — не поломка: машина стоит там, где её оставили, и её
    // состояние помнит сервер.
    const std::vector nobody = std::vector<VehicleDirectory::PlayerPlacement>{};
    CHECK(directory.reassign(nobody).empty());
}

TEST_CASE("the vehicle stays with its owner until someone is clearly closer", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 1, 0.0F);

    // Соперник ближе, но не настолько, чтобы отбирать: иначе двое, стоящие
    // почти вровень, отбирали бы машину друг у друга каждый пересмотр, а каждая
    // смена ведущего — это рассылка всем и рывок машины на экране.
    const std::vector close{at(1, 60.0F), at(2, 40.0F)};
    CHECK(directory.reassign(close).empty());
    CHECK(directory.find(id)->owner == 1);

    // А вот теперь соперник ближе с запасом.
    const std::vector clear{at(1, 200.0F), at(2, 10.0F)};
    const auto changes = directory.reassign(clear);

    REQUIRE(changes.size() == 1);
    CHECK(changes.front().owner == 2);
    CHECK(directory.find(id)->owner == 2);
}

TEST_CASE("reassign reports only what changed", "[vehicles]") {
    VehicleDirectory directory;
    spawn(directory, 1, 0.0F);
    spawn(directory, 1, 10.0F);

    const std::vector players{at(1, 0.0F)};

    // Первый пересмотр ничего не меняет: обе машины уже за тем, кто их завёл.
    CHECK(directory.reassign(players).empty());

    // Рассылать всем полный список ведущих несколько раз в секунду незачем.
    const std::vector gone = std::vector<VehicleDirectory::PlayerPlacement>{};
    CHECK(directory.reassign(gone).size() == 2);
    CHECK(directory.reassign(gone).empty());
}

TEST_CASE("removing a vehicle clears the seats inside it", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 1, 0.0F);

    directory.setSeat(1, id, shared::kDriverSeat);
    CHECK(directory.isSeatedIn(1, id));

    REQUIRE(directory.remove(id));

    // Без этой уборки за игроком осталось бы место в машине, которой нет, и он
    // навсегда получил бы преимущество водителя над машиной-призраком.
    CHECK_FALSE(directory.isSeatedIn(1, id));
    CHECK(directory.size() == 0);
}

TEST_CASE("appearance is remembered for those who join later", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 2, 0.0F);

    shared::VehicleAppearance appearance;
    appearance.id = id;
    appearance.primaryColour = 77;

    CHECK_FALSE(directory.applyAppearance(3, appearance));
    CHECK_FALSE(directory.find(id)->appearance.has_value());

    REQUIRE(directory.applyAppearance(2, appearance));

    // Внешность рассылается при изменении, а вошедший позже этого сообщения уже
    // не услышит. Не помни её сервер — новичок до конца сессии видел бы чужую
    // машину заводского вида.
    REQUIRE(directory.find(id)->appearance.has_value());
    CHECK(directory.find(id)->appearance->primaryColour == 77);
}

TEST_CASE("the appearance the server gave beats the one the owner reports", "[vehicles]") {
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 2, 0.0F);

    shared::VehicleAppearance assigned;
    assigned.primaryColour = 12;
    assigned.mods[11] = 3;

    REQUIRE(directory.setAppearance(id, assigned));

    // Номер берётся у машины, а не у описания: скрипт его не называет вовсе.
    CHECK(directory.find(id)->appearance->id == id);
    CHECK(directory.find(id)->appearance->mods[11] == 3);

    // А теперь ведущий присылает снятое со своей игры — то есть ещё без тюнинга.
    // Прими мы это, назначенное пропало бы, не успев доехать.
    shared::VehicleAppearance reported;
    reported.id = id;
    reported.primaryColour = 77;

    CHECK_FALSE(directory.applyAppearance(2, reported));
    CHECK(directory.find(id)->appearance->primaryColour == 12);
    CHECK(directory.find(id)->appearance->mods[11] == 3);
}

TEST_CASE("a vehicle that is gone has no appearance to set", "[vehicles]") {
    VehicleDirectory directory;

    CHECK_FALSE(directory.setAppearance(shared::kInvalidVehicleId, {}));

    const shared::VehicleId id = spawn(directory, 2, 0.0F);
    REQUIRE(directory.remove(id));

    CHECK_FALSE(directory.setAppearance(id, {}));
}

TEST_CASE("a repaired vehicle stops calling itself wrecked", "[server][vehicles]") {
    // Прочности без признака — починка наполовину. Вошедший позже получил бы
    // машину разбитой и взорвал бы её у себя сразу, а тем, кто уже смотрит, она
    // осталась бы остовом: чинят они её по переходу из разбитой в целую, и без
    // снятого признака перехода не случается вовсе.
    VehicleDirectory directory;
    const shared::VehicleId id = spawn(directory, 0, 0.0F);

    shared::VehicleState wrecked = directory.find(id)->state;
    wrecked.bodyHealth = 0;
    wrecked.flags = static_cast<std::uint16_t>(shared::VehicleFlag::Destroyed);
    REQUIRE(directory.applyState(0, wrecked));

    REQUIRE(shared::has(directory.find(id)->state.flags, shared::VehicleFlag::Destroyed));

    REQUIRE(directory.repair(id));

    CHECK_FALSE(shared::has(directory.find(id)->state.flags, shared::VehicleFlag::Destroyed));
}
