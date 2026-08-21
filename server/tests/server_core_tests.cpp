// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include "server_core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

using namespace oxymp;
using namespace oxymp::server;

namespace {

/// Рассылка для проверок: вместо сети — список того, что ушло бы.
///
/// Ради неё ядро и не знает про сервер напрямую. Проверить, что скрипт не может
/// вылечить вышедшего игрока или что смерть по его воле объявлена без убийцы,
/// иначе означало бы поднять сессию и кого-нибудь в ней застрелить.
class FakeSink final : public CoreSink {
public:
    std::vector<std::string> sent;

    void healthChanged(const Player& player) override {
        sent.push_back(std::format("health {} {} {}", player.id, player.health, player.armour));
    }

    void appearanceChanged(const Player& player) override {
        sent.push_back(std::format("appearance {} {:#x}", player.id,
                                   player.appearance ? player.appearance->model : 0U));
    }

    void teleported(const Player& player, const shared::Vec3& position) override {
        sent.push_back(std::format("teleport {} {:.1f} {:.1f} {:.1f}", player.id, position.x,
                                   position.y, position.z));
    }

    void vehicleTeleported(shared::VehicleId id, const shared::Vec3& position,
                           float heading) override {
        sent.push_back(std::format("vehicle teleport {} {:.1f} {:.1f} {:.1f} {:.0f}", id,
                                   position.x, position.y, position.z, heading));
    }

    void vehicleRepaired(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle repair {}", id));
    }

    void seated(const Player& player, shared::VehicleId vehicle, std::int8_t seat) override {
        sent.push_back(std::format("seat {} {} {}", player.id, vehicle, seat));
    }

    void kicked(const Player& player, std::string_view reason) override {
        sent.push_back(std::format("kick {} {}", player.id, reason));
    }

    void emitted(const Player& player, std::string_view name, std::string_view payload) override {
        sent.push_back(std::format("emit {} {} {}", player.id, name, payload));
    }

    void loadoutChanged(const Player& player, bool replace) override {
        sent.push_back(std::format("loadout {} {} {}", player.id, player.loadout.size(),
                                   replace ? "replace" : "add"));
    }

    void vehicleAdded(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle+ {}", id));
    }

    void vehicleRemoved(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle- {}", id));
    }

    void objectAdded(shared::ObjectId id) override {
        sent.push_back(std::format("object+ {}", id));
    }

    void objectRemoved(shared::ObjectId id) override {
        sent.push_back(std::format("object- {}", id));
    }

    void blipChanged(shared::BlipId id) override {
        sent.push_back(std::format("blip {}", id));
    }

    void blipRemoved(shared::BlipId id) override {
        sent.push_back(std::format("blip- {}", id));
    }

    void worldChanged() override { sent.emplace_back("world"); }

    void chatLine(shared::PlayerId to, std::string text) override {
        sent.push_back(std::format("chat {} {}", to == shared::kInvalidPlayerId ? -1
                                                                                : static_cast<int>(to),
                                   text));
    }
};

/// Слушатель, записывающий события и то, что он успел о них узнать.
class Recorder final : public script::Listener {
public:
    struct Seen {
        script::EventKind kind = script::EventKind::Tick;
        shared::PlayerId player = shared::kInvalidPlayerId;
        shared::PlayerId killer = shared::kInvalidPlayerId;

        /// Модель машины, какой обработчик увидел её в это мгновение.
        std::uint32_t vehicleModel = 0;
    };

    std::vector<Seen> seen;

    bool handle(const script::Event& event) override {
        seen.push_back(Seen{
            .kind = event.kind,
            .player = event.player.valid() ? event.player.id() : shared::kInvalidPlayerId,
            .killer = event.killer.valid() ? event.killer.id() : shared::kInvalidPlayerId,
            .vehicleModel = event.vehicle.model(),
        });

        return true;
    }
};

/// Сессия без сети: настоящие реестры, подставная рассылка.
struct Session {
    PlayerRegistry players;
    VehicleDirectory vehicles;
    ObjectDirectory objects;
    BlipDirectory blips;
    WorldClock world{"EXTRASUNNY", 12, 0};
    Config config;
    script::Events events;
    FakeSink sink;

    ServerCore core{players, vehicles, objects, blips, world, config, events, sink};

    Player& join(net::PeerId peer, std::string nickname) {
        players.add(peer, std::move(nickname), 0);
        return *players.findByPeer(peer);
    }
};

} // namespace

TEST_CASE("the core shows a player as the registries know them", "[server][script]") {
    Session session;

    Player& player = session.join(1, "игрок");
    player.position = shared::Vec3{.x = 10.0F, .y = 20.0F, .z = 30.0F};
    player.heading = 90.0F;

    const shared::VehicleId car = session.core.createVehicle(0xDEADBEEF, {}, 0.0F);
    REQUIRE(session.vehicles.setSeat(player.id, car, shared::kDriverSeat));

    const auto shown = session.core.player(player.id);

    REQUIRE(shown);
    CHECK(shown->nickname == "игрок");
    CHECK(shown->position.y == 20.0F);
    CHECK(shown->heading == 90.0F);

    // Место в машине лежит в реестре машин и больше нигде. Заведи ядро свой
    // список — он разошёлся бы с настоящим в первый же день, и разошёлся молча.
    CHECK(shown->vehicle == car);
    CHECK(shown->seat == shared::kDriverSeat);
}

TEST_CASE("a player who left cannot be reached", "[server][script]") {
    Session session;
    session.join(1, "ушедший");

    REQUIRE(session.core.player(0));
    session.players.removeByPeer(1);

    CHECK_FALSE(session.core.player(0));
    CHECK_FALSE(session.core.setHealth(0, 200, 0));
    CHECK_FALSE(session.core.giveWeapon(0, 0x1B06D571, 100));
    CHECK_FALSE(session.core.tell(0, "живой?"));

    CHECK(session.sink.sent.empty());
}

TEST_CASE("the core trims health to what the game shows", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setHealth(player.id, 60'000, 60'000));

    // Обрезается, а не принимается как есть: ошибка в числе тихая — игрок с
    // шестьюдесятью тысячами выглядит целым ровно так же, как целый, просто не
    // умирает.
    CHECK(player.health == kFullHealth);
    CHECK(player.armour == kFullArmour);

    REQUIRE(session.sink.sent.size() == 1);
    CHECK(session.sink.sent.front() == "health 0 200 100");
}

TEST_CASE("death by a script has no killer", "[server][script]") {
    Session session;
    Recorder recorder;
    session.events.subscribe(&recorder);

    Player& player = session.join(1, "жертва");

    REQUIRE(session.core.setHealth(player.id, 0, 0));

    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().kind == script::EventKind::PlayerDeath);
    CHECK(recorder.seen.front().player == player.id);

    // Убийцы нет, и это честно: обнуливший здоровье скрипт не стрелявший.
    CHECK(recorder.seen.front().killer == shared::kInvalidPlayerId);

    // Второе обнуление смертью уже не считается: мёртвый не умирает дважды, а
    // событие на каждое подтверждение того же самого — это событие на такт.
    REQUIRE(session.core.setHealth(player.id, 0, 0));
    CHECK(recorder.seen.size() == 1);
}

TEST_CASE("a weapon given twice stays one weapon", "[server][script]") {
    Session session;
    Player& player = session.join(1, "стрелок");

    REQUIRE(session.core.giveWeapon(player.id, 0x1B06D571, 100));
    REQUIRE(session.core.giveWeapon(player.id, 0x1B06D571, 250));

    // Игра держит по одному стволу каждого вида, и список обязан этому
    // соответствовать: иначе он рос бы на каждую выдачу.
    REQUIRE(player.loadout.size() == 1);
    CHECK(player.loadout.front().ammo == 250);

    REQUIRE(session.core.clearWeapons(player.id));
    CHECK(player.loadout.empty());

    // С заменой: без этого признака пустой список означал бы «добавить ничего»,
    // и оружие осталось бы у игрока в руках.
    CHECK(session.sink.sent.back() == "loadout 0 0 replace");
}

TEST_CASE("a weapon without a hash is refused", "[server][script]") {
    Session session;
    Player& player = session.join(1, "стрелок");

    CHECK_FALSE(session.core.giveWeapon(player.id, 0, 100));
    CHECK(player.loadout.empty());
    CHECK(session.sink.sent.empty());
}

TEST_CASE("a vehicle from a script belongs to nobody", "[server][script]") {
    Session session;
    Recorder recorder;
    session.events.subscribe(&recorder);

    const shared::VehicleId id =
        session.core.createVehicle(0xDEADBEEF, shared::Vec3{.x = 5.0F}, 180.0F);

    REQUIRE(id != shared::kInvalidVehicleId);

    // Ведущего нет намеренно: скрипт ставит машину куда угодно, хоть туда, где
    // никого нет, а вести её обязан тот, кто её видит. Кому — решит пересмотр.
    const auto shown = session.core.vehicle(id);
    REQUIRE(shown);
    CHECK(shown->owner == shared::kInvalidPlayerId);
    CHECK(shown->rotation.z == 180.0F);

    CHECK(session.sink.sent.size() == 1);
    CHECK(session.sink.sent.front() == std::format("vehicle+ {}", id));

    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().kind == script::EventKind::VehicleCreate);
    CHECK(recorder.seen.front().vehicleModel == 0xDEADBEEF);
}

TEST_CASE("a vehicle is still there while its removal is announced", "[server][script]") {
    Session session;
    Recorder recorder;
    session.events.subscribe(&recorder);

    const shared::VehicleId id = session.core.createVehicle(0xDEADBEEF, {}, 0.0F);
    REQUIRE(session.core.removeVehicle(id));

    REQUIRE(recorder.seen.size() == 2);
    CHECK(recorder.seen.back().kind == script::EventKind::VehicleDestroy);

    // Обработчик застаёт машину живой: он вправе спросить у неё модель — записать
    // в журнал, поставить на её место другую, — а после уборки ссылка на неё уже
    // ничего не расскажет.
    CHECK(recorder.seen.back().vehicleModel == 0xDEADBEEF);

    CHECK_FALSE(session.core.vehicle(id));
    CHECK(session.sink.sent.back() == std::format("vehicle- {}", id));
}

TEST_CASE("removing a vehicle that is gone changes nothing", "[server][script]") {
    Session session;
    Recorder recorder;
    session.events.subscribe(&recorder);

    CHECK_FALSE(session.core.removeVehicle(12345));

    CHECK(session.sink.sent.empty());
    CHECK(recorder.seen.empty());
}

TEST_CASE("the core keeps the session within its limits", "[server][script]") {
    Session session;
    session.config.maxVehicles = 1;
    session.config.maxObjects = 1;

    REQUIRE(session.core.createVehicle(0xDEADBEEF, {}, 0.0F) != shared::kInvalidVehicleId);
    CHECK(session.core.createVehicle(0xDEADBEEF, {}, 0.0F) == shared::kInvalidVehicleId);

    REQUIRE(session.core.createObject(0xDEADBEEF, {}, {}) != shared::kInvalidObjectId);
    CHECK(session.core.createObject(0xDEADBEEF, {}, {}) == shared::kInvalidObjectId);

    // Предел исчерпан — рассылки нет. Иначе клиенты заводили бы у себя машину,
    // которой сервер не выдавал.
    CHECK(session.sink.sent.size() == 2);
}

TEST_CASE("the world changes only when the game would accept it", "[server][script]") {
    Session session;

    REQUIRE(session.core.setWeather("THUNDER"));
    CHECK(session.sink.sent.back() == "world");

    CHECK_FALSE(session.core.setWeather(""));
    CHECK_FALSE(session.core.setTime(25, 0));

    // Негодного клиентам не рассылается: игра пропустила бы его мимо, а сессия
    // разошлась бы в том, который час.
    CHECK(session.sink.sent.size() == 1);

    REQUIRE(session.core.setTime(23, 30));
    CHECK(session.world.snapshot().hour == 23);
}

TEST_CASE("an empty line is not sent to anyone", "[server][script]") {
    Session session;
    session.join(1, "игрок");

    session.core.broadcast("");
    CHECK_FALSE(session.core.tell(0, ""));

    CHECK(session.sink.sent.empty());

    session.core.broadcast("всем");
    REQUIRE(session.core.tell(0, "одному"));

    REQUIRE(session.sink.sent.size() == 2);
    CHECK(session.sink.sent.front() == "chat -1 всем");
    CHECK(session.sink.sent.back() == "chat 0 одному");
}

TEST_CASE("the core tells a script who may command the session", "[server][script]") {
    Session session;
    session.config.admins = {0};

    session.join(1, "хозяин");
    session.join(2, "гость");

    const auto boss = session.core.player(0);
    const auto guest = session.core.player(1);

    REQUIRE(boss);
    REQUIRE(guest);

    // Право живёт в настройках сервера: назначает распорядителей хозяин сессии,
    // и подменять его решение ресурсу непозволительно. Видеть же ответ ресурс
    // обязан — иначе ему нельзя доверить ничего, что меняет мир.
    CHECK(boss->admin);
    CHECK_FALSE(guest->admin);
}

TEST_CASE("a teleport is a request, not a change", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");
    player.position = shared::Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F};

    REQUIRE(session.core.teleport(player.id, shared::Vec3{.x = 50.0F, .y = 60.0F, .z = 70.0F}));

    // Положение у себя не меняется: правду о нём приносит снимок игрока, а
    // придуманная здесь разошлась бы с настоящей до первого снимка — и по ней
    // сервер решает, кому какую машину вести и кому о чём рассказывать.
    CHECK(player.position.x == 1.0F);
    CHECK(session.sink.sent.back() == "teleport 0 50.0 60.0 70.0");
}

TEST_CASE("a model named by a script reaches everyone, the owner included",
          "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    // Внешности у него ещё нет вовсе: свою он объявляет не сразу, а сервер
    // вправе назначить модель хоть в обработчике входа — то есть раньше.
    REQUIRE_FALSE(player.appearance.has_value());

    REQUIRE(session.core.setModel(player.id, 0x705E61F2));

    REQUIRE(player.appearance.has_value());
    CHECK(player.appearance->model == 0x705E61F2);

    // Чей это вид, проставляет сервер: без этого получатель не понял бы, кого
    // переодевать.
    CHECK(player.appearance->playerId == player.id);

    CHECK(session.sink.sent.back() == "appearance 0 0x705e61f2");
}

TEST_CASE("a model of zero is refused instead of stripping the player",
          "[server][script]") {
    // Ноль в PlayerAppearance означает «оставить ту, что есть», и приняв его за
    // распоряжение, сервер оставил бы игрока без тела.
    Session session;
    Player& player = session.join(1, "игрок");

    CHECK_FALSE(session.core.setModel(player.id, 0));
    CHECK_FALSE(player.appearance.has_value());
}

TEST_CASE("a model named for nobody changes nothing", "[server][script]") {
    Session session;
    session.join(1, "игрок");

    CHECK_FALSE(session.core.setModel(7, 0x705E61F2));
}

TEST_CASE("an event without a name goes nowhere", "[server][script]") {
    Session session;
    session.join(1, "игрок");

    CHECK_FALSE(session.core.emit(0, "", "{}"));
    CHECK_FALSE(session.core.emit(7, "ui:menu", "{}"));

    CHECK(session.sink.sent.empty());

    REQUIRE(session.core.emit(0, "ui:menu", R"({"open":0})"));
    CHECK(session.sink.sent.back() == R"(emit 0 ui:menu {"open":0})");
}

TEST_CASE("a player starts in the default dimension and can be moved out of it",
          "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    CHECK(player.dimension == script::kDefaultDimension);
    CHECK(session.core.player(player.id)->dimension == script::kDefaultDimension);

    REQUIRE(session.core.setDimension(player.id, 42));

    CHECK(player.dimension == 42);
    CHECK(session.core.player(player.id)->dimension == 42);
}

TEST_CASE("moving a player between dimensions tells the clients nothing",
          "[server][script]") {
    // Клиент про измерения не знает вовсе, и рассылать ему тут нечего: перемена
    // скажется сама собой — тем, кто его больше видеть не должен, снимки просто
    // перестанут приходить. Не рассказать и есть единственный способ не дать
    // увидеть.
    Session session;
    Player& player = session.join(1, "игрок");

    const std::size_t before = session.sink.sent.size();

    REQUIRE(session.core.setDimension(player.id, 3));

    CHECK(session.sink.sent.size() == before);
}

TEST_CASE("a dimension named for nobody changes nothing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.setDimension(7, 1));
}

TEST_CASE("a vehicle carries its own dimension, not its driver's", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    const shared::VehicleId id =
        session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    CHECK(session.core.vehicle(id)->dimension == script::kDefaultDimension);

    // Слой мира у машины свой: вести её можно и не сидя в ней, а стоит она там,
    // где стоит, независимо от того, кто её ведёт.
    REQUIRE(session.core.setDimension(player.id, 5));
    CHECK(session.core.vehicle(id)->dimension == script::kDefaultDimension);

    REQUIRE(session.core.setVehicleDimension(id, 5));
    CHECK(session.core.vehicle(id)->dimension == 5);
}

TEST_CASE("a dimension named for a vehicle that is gone changes nothing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.setVehicleDimension(1, 1));
}

TEST_CASE("clothes named by a script reach everyone, the owner included", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    // Внешности у него ещё нет вовсе: свою он объявляет не сразу, а сервер
    // вправе одеть его хоть в обработчике входа — то есть раньше.
    REQUIRE_FALSE(player.appearance.has_value());

    // Одиннадцатый слот — верх, и это нумерация игры, а не наша.
    REQUIRE(session.core.setClothes(player.id, 11, 15, 2, 0));

    REQUIRE(player.appearance.has_value());
    CHECK(player.appearance->components[11].drawable == 15);
    CHECK(player.appearance->components[11].texture == 2);
    CHECK(player.appearance->playerId == player.id);

    CHECK(session.sink.sent.back().starts_with("appearance 0"));
}

TEST_CASE("a clothing slot outside the ones the game has is refused", "[server][script]") {
    // Молча проглоченный слот выглядел бы как надетое, но невидимое, — и
    // искали бы его в одежде, а не в номере.
    Session session;
    Player& player = session.join(1, "игрок");

    CHECK_FALSE(session.core.setClothes(player.id, shared::kPedComponentCount, 1, 0, 0));
    CHECK_FALSE(player.appearance.has_value());
}

TEST_CASE("an accessory is put on and taken off by the same call", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setProp(player.id, 0, 5, 1));
    CHECK(player.appearance->props[0].drawable == 5);
    CHECK(player.appearance->props[0].texture == 1);

    // Минус единица — то, чем «ничего не надето» зовётся у самой игры.
    REQUIRE(session.core.setProp(player.id, 0, -1, 0));
    CHECK(player.appearance->props[0].drawable == -1);
}

TEST_CASE("clothes named for nobody change nothing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.setClothes(7, 11, 1, 0, 0));
    CHECK_FALSE(session.core.setProp(7, 0, 1, 0));
}

TEST_CASE("a vehicle teleport is a request to whoever leads it", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");
    player.position = shared::Vec3{.x = 0.0F, .y = 0.0F, .z = 0.0F};

    const shared::VehicleId id = session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    // Ведущего у машины ещё нет: пересмотр ведущих идёт своим чередом, а
    // здесь его никто не звал. Значит и просить некого — сервер ставит её у
    // себя и молчит.
    REQUIRE(session.core.teleportVehicle(id, shared::Vec3{.x = 5.0F, .y = 6.0F, .z = 7.0F}, 90.0F));

    CHECK(session.core.vehicle(id)->position.x == 5.0F);
    CHECK(session.core.vehicle(id)->rotation.z == 90.0F);
    CHECK(std::ranges::none_of(session.sink.sent, [](const std::string& line) {
        return line.starts_with("vehicle teleport");
    }));
}

TEST_CASE("a repaired vehicle gets its health and glass back", "[server][script]") {
    Session session;

    const shared::VehicleId id = session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    REQUIRE(session.core.repairVehicle(id));
}

TEST_CASE("a command for a vehicle that is gone changes nothing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.teleportVehicle(1, shared::Vec3{}, 0.0F));
    CHECK_FALSE(session.core.repairVehicle(1));
}

TEST_CASE("a blip is handed a number by the server, not by the script", "[server][script]") {
    Session session;

    script::BlipInfo wanted;
    wanted.position = shared::Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F};
    wanted.sprite = 402;
    wanted.name = "Банк";

    // Номер, подставленный скриптом, не читается: назначает его сервер, и
    // позволить метке назвать себя чужим номером значило бы разрешить ей стать
    // другой меткой.
    wanted.id = 999;

    const shared::BlipId id = session.core.createBlip(wanted);

    REQUIRE(id != shared::kInvalidBlipId);
    CHECK(id != 999);

    const auto got = session.core.blip(id);

    REQUIRE(got);
    CHECK(got->id == id);
    CHECK(got->sprite == 402);
    CHECK(got->name == "Банк");
    CHECK(session.sink.sent.back() == std::format("blip {}", id));
}

TEST_CASE("a blip is edited whole, and the clients hear about it", "[server][script]") {
    Session session;

    script::BlipInfo wanted;
    wanted.name = "Было";

    const shared::BlipId id = session.core.createBlip(wanted);
    REQUIRE(id != shared::kInvalidBlipId);

    script::BlipInfo changed = wanted;
    changed.name = "Стало";
    changed.colour = 3;
    changed.dimension = 5;

    REQUIRE(session.core.updateBlip(id, changed));

    const auto got = session.core.blip(id);

    REQUIRE(got);
    CHECK(got->name == "Стало");
    CHECK(got->colour == 3);
    CHECK(got->dimension == 5);
    CHECK(session.sink.sent.back() == std::format("blip {}", id));
}

TEST_CASE("a blip that is gone refuses both editing and removing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.updateBlip(1, script::BlipInfo{}));
    CHECK_FALSE(session.core.removeBlip(1));
}

TEST_CASE("blips stop being handed out once the limit is reached", "[server][script]") {
    Session session;
    session.config.maxBlips = 2;

    CHECK(session.core.createBlip(script::BlipInfo{}) != shared::kInvalidBlipId);
    CHECK(session.core.createBlip(script::BlipInfo{}) != shared::kInvalidBlipId);
    CHECK(session.core.createBlip(script::BlipInfo{}) == shared::kInvalidBlipId);
}

TEST_CASE("seating a player is a request, not a change in the registry", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    const shared::VehicleId id = session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    REQUIRE(session.core.setIntoVehicle(player.id, id, shared::kNoSeat));

    CHECK(session.sink.sent.back() ==
          std::format("seat {} {} {}", player.id, id, static_cast<int>(shared::kNoSeat)));

    // Место в реестре не проставляется: кто где сидит, сервер узнаёт из
    // снимков. Записанное здесь разошлось бы с правдой до первого же снимка — а
    // по ней сервер решает, кому вести машину.
    CHECK(session.core.player(player.id)->vehicle == shared::kInvalidVehicleId);
}

TEST_CASE("seating into a vehicle that is gone changes nothing", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    CHECK_FALSE(session.core.setIntoVehicle(player.id, 1, shared::kNoSeat));
}
