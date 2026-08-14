// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include "fake_core.hpp"

#include <oxymp/script/entity.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace oxymp;
using namespace oxymp::script;
using oxymp::script::testing::FakeCore;

TEST_CASE("a reference resolves the entity every time", "[script]") {
    FakeCore core;
    core.join(0, "player");

    const Player player{core, 0};

    REQUIRE(player.valid());
    CHECK(player.nickname() == "player");
    CHECK(player.health() == 200);

    // Ссылка держит номер, а не запись: изменение за её спиной видно сразу, без
    // всякого обновления. Держи она копию — скрипт судил бы о живом игроке по
    // тому, каким тот был на момент подписки.
    REQUIRE(core.setHealth(0, 50, 25));

    CHECK(player.health() == 50);
    CHECK(player.armour() == 25);
}

TEST_CASE("a reference to someone gone answers honestly", "[script]") {
    FakeCore core;
    core.join(3, "гость");

    const Player player{core, 3};
    REQUIRE(player.valid());

    // Игрок волен выйти посреди обработчика события: скрипт, откликнувшийся на
    // его выстрел, вправе решить показать строку в чат — а к этому мгновению
    // отправителя уже нет.
    core.leave(3);

    CHECK_FALSE(player.valid());
    CHECK(player.nickname().empty());
    CHECK(player.health() == 0);

    // И распоряжения ему больше не проходят — молча и без падения.
    CHECK_FALSE(player.setHealth(200, 0));
    CHECK_FALSE(player.tell("живой?"));
}

TEST_CASE("a default reference is harmless", "[script]") {
    // Такая встречается там, где событие не называет второго участника: смерть
    // от падения обходится без убийцы. Скрипт, забывший спросить valid(),
    // должен получить нули, а не крах сервера.
    const Player nobody;

    CHECK_FALSE(nobody.valid());
    CHECK(nobody.id() == shared::kInvalidPlayerId);
    CHECK(nobody.nickname().empty());
    CHECK_FALSE(nobody.giveWeapon(0x1B06D571, 100));

    const Vehicle nothing;

    CHECK_FALSE(nothing.valid());
    CHECK(nothing.model() == 0);
    CHECK_FALSE(nothing.remove());
}

TEST_CASE("a vehicle names the player who leads it", "[script]") {
    FakeCore core;
    core.join(1, "водитель");

    const shared::VehicleId id =
        core.createVehicle(0xDEADBEEF, shared::Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F}, 90.0F);

    REQUIRE(id != shared::kInvalidVehicleId);

    const Vehicle vehicle{core, id};
    REQUIRE(vehicle.valid());
    CHECK(vehicle.model() == 0xDEADBEEF);
    CHECK(vehicle.rotation().z == 90.0F);

    // Ведущего нет — машина стоит. Это обычное положение вещей, а не поломка, и
    // ссылка на «никого» обязана быть безобидной.
    CHECK_FALSE(vehicle.owner().valid());

    core.vehicleList.front().owner = 1;

    REQUIRE(vehicle.owner().valid());
    CHECK(vehicle.owner().nickname() == "водитель");
}

TEST_CASE("a player names the vehicle they sit in", "[script]") {
    FakeCore core;
    PlayerInfo& info = core.join(2, "пассажир");

    CHECK_FALSE(Player{core, 2}.vehicle().valid());

    const shared::VehicleId id = core.createVehicle(0xDEADBEEF, {}, 0.0F);
    info.vehicle = id;

    const Vehicle riding = Player{core, 2}.vehicle();

    REQUIRE(riding.valid());
    CHECK(riding.id() == id);
}

TEST_CASE("removing an entity invalidates references to it", "[script]") {
    FakeCore core;

    const shared::ObjectId id = core.createObject(0xDEADBEEF, {}, {});
    const Object object{core, id};

    REQUIRE(object.valid());
    REQUIRE(object.remove());

    CHECK_FALSE(object.valid());

    // Второй раз убрать уже нечего — и это отказ, а не молчаливый успех.
    CHECK_FALSE(object.remove());
}

TEST_CASE("the core refuses an entity without a model", "[script]") {
    FakeCore core;

    CHECK(core.createVehicle(0, {}, 0.0F) == shared::kInvalidVehicleId);
    CHECK(core.createObject(0, {}, {}) == shared::kInvalidObjectId);
}
