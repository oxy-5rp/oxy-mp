// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include "server_core.hpp"

#include <catch2/catch_approx.hpp>
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

    void vehicleAppearanceChanged(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle look {}", id));
    }

    void attachmentChanged(AttachmentDirectory::Ref entity) override {
        sent.push_back(std::format("attach {} {}", static_cast<int>(entity.kind), entity.id));
    }

    void pedChanged(shared::PedId id) override { sent.push_back(std::format("ped {}", id)); }
    void pedRemoved(shared::PedId id) override { sent.push_back(std::format("ped- {}", id)); }

    void seated(const Player& player, shared::VehicleId vehicle, std::int8_t seat) override {
        sent.push_back(std::format("seat {} {} {}", player.id, vehicle, seat));
    }

    void kicked(const Player& player, std::string_view reason) override {
        sent.push_back(std::format("kick {} {}", player.id, reason));
    }

    void emitted(const Player& player, std::string_view name, std::string_view payload) override {
        sent.push_back(std::format("emit {} {} {}", player.id, name, payload));
    }

    void loadoutChanged(const Player& player, bool replace, std::uint32_t equip) override {
        equipped = equip;
        sent.push_back(std::format("loadout {} {} {}", player.id, player.loadout.size(),
                                   replace ? "replace" : "add"));
    }

    /// Какое оружие последний список велел вложить в руки. Ноль — никакое.
    std::uint32_t equipped = 0;

    void vehicleAdded(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle+ {}", id));
    }

    void vehicleRemoved(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle- {}", id));
    }

    void vehicleDoorsChanged(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle doors {}", id));
    }

    void vehicleControlChanged(shared::VehicleId id) override {
        sent.push_back(std::format("vehicle lock {}", id));
    }

    void controlChanged(const Player& player) override {
        sent.push_back(std::format("control {} {}", player.id, player.control));
    }

    void objectAdded(shared::ObjectId id) override {
        sent.push_back(std::format("object+ {}", id));
    }

    void objectMoved(shared::ObjectId id) override {
        sent.push_back(std::format("object= {}", id));
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

    void markerChanged(shared::MarkerId id) override {
        sent.push_back(std::format("marker {}", id));
    }

    void markerRemoved(shared::MarkerId id) override {
        sent.push_back(std::format("marker- {}", id));
    }

    void checkpointChanged(shared::CheckpointId id) override {
        sent.push_back(std::format("checkpoint {}", id));
    }

    void checkpointRemoved(shared::CheckpointId id) override {
        sent.push_back(std::format("checkpoint- {}", id));
    }

    void animationPlayed(const Player& player,
                         const shared::PlayerAnimation& animation) override {
        sent.push_back(std::format("anim {} {}/{}", player.id, animation.dictionary,
                                   animation.name));
    }

    void exploded(const shared::Explosion& explosion, std::int32_t dimension) override {
        sent.push_back(std::format("boom {} at {} {} {} in {}", explosion.kind,
                                   explosion.position.x, explosion.position.y,
                                   explosion.position.z, dimension));
    }

    void dimensionChanged(const Player& player, std::int32_t previous) override {
        sent.push_back(std::format("dimension {} {}->{}", player.id, previous, player.dimension));
    }

    void worldChanged() override { sent.emplace_back("world"); }

    /// Адрес и задержка в проверках подставные и постоянные: они приходят от
    /// транспорта, а транспорта здесь нет вовсе — и не должно быть.
    [[nodiscard]] std::string addressOf(const Player& player) const override {
        return std::format("10.0.0.{}", player.id);
    }

    [[nodiscard]] std::uint32_t latencyOf(const Player& player) const override {
        return 40 + player.id;
    }

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
    MarkerDirectory markers;
    CheckpointDirectory checkpoints;
    WorldClock world{"EXTRASUNNY", 12, 0};
    Config config;
    script::Events events;
    FakeSink sink;

    PedDirectory peds;
    AttachmentDirectory attachments;
    ServerCore core{players,      vehicles,     objects, peds,    blips,  markers,
                    checkpoints,  attachments,  world,   config,  events, sink};

    Player& join(net::PeerId peer, std::string nickname) {
        players.add(peer, std::move(nickname), 0, Identity{});
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

TEST_CASE("an explosion reaches the wire with its own dimension", "[server][script]") {
    // Взрыв — единственное распоряжение ядра, у которого нет ни игрока, ни
    // сущности: его заводит ресурс, а не тот, у кого рвануло. Слой мира поэтому
    // приходит доводом, а не берётся из игрока, — и до рассылки он обязан
    // доехать в целости, иначе взрыв прогремит не в том слое.
    Session session;

    script::ExplosionInfo explosion;
    explosion.position = shared::Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F};
    explosion.kind = static_cast<std::int32_t>(shared::ExplosionKind::Rocket);
    explosion.dimension = 7;

    session.core.explode(explosion);

    REQUIRE(session.sink.sent.size() == 1);
    CHECK(session.sink.sent.front() == "boom 4 at 1 2 3 in 7");
}

TEST_CASE("an explosion does not need anyone to be online", "[server][script]") {
    // Пустая площадь — не отказ: взрыв заводит ресурс, и не показать его
    // некому значит ровно «рядом никого нет». Ответа у распоряжения поэтому
    // нет вовсе.
    Session session;

    session.core.explode(script::ExplosionInfo{});

    CHECK(session.sink.sent.size() == 1);
}

TEST_CASE("two players may share a nickname", "[server][script]") {
    // Имя в сессии ничего не решает: игрок называется номером, и по номеру его
    // находит и режим, и чат, и всякая рассылка. Требование разных имён было
    // при этом самым частым отказом на входе — имя приходит из oxymp.toml, и по
    // умолчанию оно у всех одно и то же.
    Session session;

    Player& first = session.join(1, "oxy");
    Player& second = session.join(2, "oxy");

    CHECK(first.id != second.id);

    const auto shownFirst = session.core.player(first.id);
    const auto shownSecond = session.core.player(second.id);

    REQUIRE(shownFirst);
    REQUIRE(shownSecond);
    CHECK(shownFirst->nickname == "oxy");
    CHECK(shownSecond->nickname == "oxy");

    // И распоряжение доходит до того, кому назначено, а не до однофамильца.
    REQUIRE(session.core.setHealth(second.id, 60, 0));

    const auto healed = session.core.player(second.id);
    REQUIRE(healed);
    CHECK(healed->health == 60);
    CHECK(session.core.player(first.id)->health != 60);
}

TEST_CASE("a player who left cannot be reached", "[server][script]") {
    Session session;
    session.join(1, "ушедший");

    REQUIRE(session.core.player(0));
    session.players.removeByPeer(1);

    CHECK_FALSE(session.core.player(0));
    CHECK_FALSE(session.core.setHealth(0, 200, 0));
    CHECK_FALSE(session.core.giveWeapon(0, 0x1B06D571, 100, false));
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

    REQUIRE(session.core.giveWeapon(player.id, 0x1B06D571, 100, false));
    REQUIRE(session.core.giveWeapon(player.id, 0x1B06D571, 250, false));

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

    CHECK_FALSE(session.core.giveWeapon(player.id, 0, 100, false));
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

    // Три события: завели, убрали своим именем и убрали общим. Своё имя идёт
    // первым, общее вторым — у alt:V `removeEntity` объявляется на всякую
    // сущность, а `vehicleDestroy` только на машину, и подписаны на них разные
    // места.
    REQUIRE(recorder.seen.size() == 3);
    CHECK(recorder.seen[1].kind == script::EventKind::VehicleDestroy);
    CHECK(recorder.seen[2].kind == script::EventKind::RemoveEntity);

    // Обработчик застаёт машину живой: он вправе спросить у неё модель — записать
    // в журнал, поставить на её место другую, — а после уборки ссылка на неё уже
    // ничего не расскажет.
    CHECK(recorder.seen[1].vehicleModel == 0xDEADBEEF);

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

TEST_CASE("moving a player between dimensions tells the others nothing",
          "[server][script]") {
    // Клиент про измерения не знает вовсе, и номера слоя не узнаёт никогда:
    // не рассказать и есть единственный способ не дать увидеть. Остальным о
    // переходе не говорится ничего — перемена скажется сама собой: тем, кто его
    // больше видеть не должен, снимки просто перестанут приходить.
    //
    // Единственное, чем перемена отзывается, — переспрос нарисованного у самого
    // ушедшего: метки и фигуры уходят к нему один раз, при входе, и сами собой
    // не разберутся.
    Session session;

    Player& player = session.join(1, "игрок");
    session.join(2, "сосед");
    session.sink.sent.clear();

    REQUIRE(session.core.setDimension(player.id, 3));

    CHECK(session.sink.sent == std::vector<std::string>{"dimension 0 0->3"});
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
    CHECK_FALSE(session.core.setVehicleAppearance(1, {}));
    CHECK_FALSE(session.core.vehicleAppearance(1).has_value());
}

TEST_CASE("a vehicle nobody has described looks stock", "[server][script]") {
    Session session;

    const shared::VehicleId id = session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    // Пустота здесь означала бы «нет такой машины», а машина есть. Что о её
    // внешности пока никто не говорил — не отсутствие ответа, а сам ответ.
    const std::optional<script::VehicleAppearanceInfo> look = session.core.vehicleAppearance(id);
    REQUIRE(look.has_value());

    CHECK(look->primaryColour == 0);
    CHECK(look->mods[11] == shared::kStockMod);
    CHECK(look->plate.empty());
}

TEST_CASE("a vehicle wears the mods the script names", "[server][script]") {
    Session session;

    const shared::VehicleId id = session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    script::VehicleAppearanceInfo look;
    look.primaryColour = 12;
    look.mods[11] = 3;
    look.neonSides = shared::NeonSide::Left | shared::NeonSide::Right;

    REQUIRE(session.core.setVehicleAppearance(id, look));

    // Рассказать о ней обязаны всем, кто машину видит: показывает её каждый у
    // себя сам, и умолчи сервер — перекрашенной она осталась бы только на бумаге.
    CHECK(std::ranges::count(session.sink.sent, std::format("vehicle look {}", id)) == 1);

    const std::optional<script::VehicleAppearanceInfo> worn = session.core.vehicleAppearance(id);
    REQUIRE(worn.has_value());

    CHECK(worn->primaryColour == 12);
    CHECK(worn->mods[11] == 3);
    CHECK(worn->neonSides == (shared::NeonSide::Left | shared::NeonSide::Right));
}

TEST_CASE("a number plate longer than the game shows is trimmed at once",
          "[server][script]") {
    Session session;

    const shared::VehicleId id = session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    REQUIRE(id != shared::kInvalidVehicleId);

    script::VehicleAppearanceInfo look;
    look.plate = "TOOLONGPLATE";

    REQUIRE(session.core.setVehicleAppearance(id, look));

    // Обрезается на месте, а не у читающего: иначе у сервера номер остался бы
    // длинным, а у игроков — коротким, и скрипт, спросивший внешность обратно,
    // получил бы не то, что видно в игре.
    CHECK(session.core.vehicleAppearance(id)->plate.size() == shared::kMaxPlateLength);
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

TEST_CASE("clearing one clothing slot leaves the rest on", "[server][script]") {
    // У alt:V объявлено `clearClothes(component)` — снять одну вещь. Довод
    // здесь однажды не читался вовсе, и всякий `clearClothes(11)` раздевал
    // человека целиком: и штаны, и обувь, и маску. Ошибка молчала — вызов
    // удавался, ответ был `true`.
    Session session;
    const Player& player = session.join(1, "oxy");

    REQUIRE(session.core.setClothes(player.id, 11, 15, 0, 0));
    REQUIRE(session.core.setClothes(player.id, 4, 7, 0, 0));

    REQUIRE(session.core.setClothes(player.id, 11, 0, 0, 0));

    const auto look = session.core.appearance(player.id);

    REQUIRE(look);
    CHECK(look->components[11].drawable == 0);
    CHECK(look->components[4].drawable == 7);
}

TEST_CASE("a vehicle tells the scripts how far its wheel is turned", "[server][script]") {
    // Руль ехал в каждом снимке машины и не был виден скрипту — ровно как
    // прочность до него. Это ввод водителя, а не угол колёс в градусах: у alt:V
    // `steeringAngle` тех же долей.
    Session session;
    const Player& player = session.join(1, "oxy");

    // Заводит машину сам игрок — ведущим она достаётся ему сразу, и снимок от
    // него будет принят. Чужой снимок реестр отбрасывает целиком.
    const shared::VehicleId id = session.vehicles.add(0xDEADBEEF, {}, 0.0F, player.id, 64);
    REQUIRE(id != shared::kInvalidVehicleId);

    shared::VehicleState state;
    state.id = id;
    state.steer = -0.75F;

    REQUIRE(session.vehicles.applyState(player.id, state));

    const auto got = session.core.vehicle(id);

    REQUIRE(got);
    CHECK(got->steer == -0.75F);
}

TEST_CASE("moving a player to another world layer is announced", "[server][script]") {
    // Слой мира едет своим полем, а не общим `seat`: слой это число со знаком в
    // четыре байта, а место в машине — байт, и урезанный слой указал бы не туда.
    Session session;
    Recorder recorder;

    Player& player = session.join(1, "oxy");
    session.events.subscribe(&recorder);

    REQUIRE(session.core.setDimension(player.id, 100000));

    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().kind == script::EventKind::PlayerDimensionChange);
}

TEST_CASE("moving a player to the layer it is already in says nothing",
          "[server][script]") {
    Session session;
    Recorder recorder;

    Player& player = session.join(1, "oxy");
    REQUIRE(session.core.setDimension(player.id, 7));

    session.events.subscribe(&recorder);

    REQUIRE(session.core.setDimension(player.id, 7));

    CHECK(recorder.seen.empty());
}

TEST_CASE("healing is told apart from taking damage", "[server][script]") {
    // Не всякая перемена здоровья — лечение: убыль это урон, и о нём говорит
    // своё событие там, где известен ударивший. Сюда попадает только рост.
    Session session;
    Recorder recorder;

    Player& player = session.join(1, "oxy");
    REQUIRE(session.core.setHealth(player.id, 100, 0));

    session.events.subscribe(&recorder);

    REQUIRE(session.core.setHealth(player.id, 180, 20));

    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().kind == script::EventKind::PlayerHeal);
    CHECK(recorder.seen.front().player == player.id);
}

TEST_CASE("losing health is not called healing", "[server][script]") {
    Session session;
    Recorder recorder;

    Player& player = session.join(1, "oxy");
    REQUIRE(session.core.setHealth(player.id, 180, 0));

    session.events.subscribe(&recorder);

    REQUIRE(session.core.setHealth(player.id, 100, 0));

    CHECK(recorder.seen.empty());
}

TEST_CASE("armour is capped by the limit the script set, not the game default",
          "[server][script]") {
    // Тяжёлый бронежилет в режимах — это поднятый предел, а не броня сверх
    // него. Обрезай мы по общей сотне, поднятый предел не значил бы ничего.
    Session session;
    const Player& player = session.join(1, "oxy");

    REQUIRE(session.core.setMaxArmour(player.id, 200));
    REQUIRE(session.core.setHealth(player.id, 200, 200));

    CHECK(player.armour == 200);
}

TEST_CASE("lowering the armour limit takes off what is above it", "[server][script]") {
    // Иначе игрок остался бы в броне выше собственного предела, и снять её было
    // бы нечем: следующая же выдача обрезалась бы по новому пределу, а нынешняя
    // так и стояла бы.
    Session session;
    const Player& player = session.join(1, "oxy");

    REQUIRE(session.core.setMaxArmour(player.id, 200));
    REQUIRE(session.core.setHealth(player.id, 200, 200));
    REQUIRE(session.core.setMaxArmour(player.id, 50));

    CHECK(player.armour == 50);
}

TEST_CASE("ammo of a weapon already carried can be changed without giving it again",
          "[server][script]") {
    // Без замены списка: замена отобрала бы у игрока оружие из рук на
    // мгновение — игра выдаёт его заново, — и держащий ствол опустил бы руки
    // посреди перестрелки.
    Session session;
    const Player& player = session.join(1, "oxy");

    REQUIRE(session.core.giveWeapon(player.id, 0x1B06D571, 100, false));
    REQUIRE(session.core.setWeaponAmmo(player.id, 0x1B06D571, 7));

    const std::vector<shared::WeaponSlot> carried = session.core.loadout(player.id);

    REQUIRE(carried.size() == 1);
    CHECK(carried.front().ammo == 7);
}

TEST_CASE("ammo of a weapon nobody carries is refused", "[server][script]") {
    // Отказ, а не молчаливая выдача: режим, доложивший патроны в
    // несуществующее оружие, узнает об этом сразу, а не когда игрок полезет
    // стрелять.
    Session session;
    const Player& player = session.join(1, "oxy");

    CHECK_FALSE(session.core.setWeaponAmmo(player.id, 0x1B06D571, 7));
}

TEST_CASE("a weapon given to be held says so to the client", "[server][script]") {
    // Просьба вложить в руки уезжает вместе со списком, а не отдельным
    // распоряжением: список и так придёт, и второе сообщение о том же оружии
    // разъехалось бы с ним по дороге.
    Session session;
    const Player& player = session.join(1, "oxy");

    REQUIRE(session.core.giveWeapon(player.id, 0x1B06D571, 100, true));

    CHECK(session.sink.equipped == 0x1B06D571);
}

TEST_CASE("a weapon given without asking to hold it changes nothing in the hands",
          "[server][script]") {
    // Ноль означает «не трогать того, что он держит». Подменять человеку оружие
    // посреди перестрелки оттого, что сервер прислал список, — не то, о чём его
    // просили.
    Session session;
    const Player& player = session.join(1, "oxy");

    REQUIRE(session.core.giveWeapon(player.id, 0x1B06D571, 100, false));

    CHECK(session.sink.equipped == 0);
}

TEST_CASE("a refused connection tells the scripts who was refused",
          "[server][script]") {
    // Игрока в этом событии нет и быть не может: отказ случается раньше, чем
    // игрок заводится. Поэтому имя и адрес едут строками — взять их обработчику
    // больше неоткуда.
    script::Event denied;
    denied.kind = script::EventKind::PlayerConnectDenied;
    denied.reason = static_cast<std::uint8_t>(shared::RejectReason::WrongPassword);
    denied.name = "oxy";
    denied.text = "127.0.0.1";

    CHECK(denied.player.id() == shared::kInvalidPlayerId);
    CHECK(denied.reason == static_cast<std::uint8_t>(shared::RejectReason::WrongPassword));
    CHECK(denied.name == "oxy");
}

TEST_CASE("a blip remembers who it was meant for", "[server][script]") {
    Session session;

    script::BlipInfo wanted;
    wanted.name = "Только своим";
    wanted.targets = {7, 9};

    const shared::BlipId id = session.core.createBlip(wanted);
    REQUIRE(id != shared::kInvalidBlipId);

    const auto got = session.core.blip(id);

    REQUIRE(got);
    CHECK(got->targets == std::vector<shared::PlayerId>{7, 9});
}

TEST_CASE("a blip without targets is meant for everyone", "[server][script]") {
    // Метка, поставленная без списка, обязана быть видна всей сессии: так же
    // читает её alt:V, и режим, назвавший только положение, ждёт обычную метку.
    Session session;

    const shared::BlipId id = session.core.createBlip(script::BlipInfo{});
    REQUIRE(id != shared::kInvalidBlipId);

    const auto got = session.core.blip(id);

    REQUIRE(got);
    CHECK(got->global);
    CHECK(got->targets.empty());
}

TEST_CASE("a blip that is not global and names nobody is meant for nobody",
          "[server][script]") {
    // Разница между «общая» и «пустой список» — не отвлечённая. Прежде общность
    // выводилась из пустоты списка, и метка, заведённая с `global = false`,
    // успевала мигнуть у всех до того, как ей назовут получателей; а не
    // назвавшая их ни разу оставалась общей навсегда.
    Session session;

    script::BlipInfo wanted;
    wanted.global = false;

    const shared::BlipId id = session.core.createBlip(wanted);
    REQUIRE(id != shared::kInvalidBlipId);

    const auto got = session.core.blip(id);

    REQUIRE(got);
    CHECK_FALSE(got->global);
    CHECK(got->targets.empty());
}

TEST_CASE("naming a target does not make a global blip private", "[server][script]") {
    // Обратное тоже верно: общность называется при заведении и списком не
    // меняется. У alt:V `isGlobal` объявлено `readonly` ровно поэтому.
    Session session;

    script::BlipInfo wanted;
    wanted.targets = {3};

    const shared::BlipId id = session.core.createBlip(wanted);
    REQUIRE(id != shared::kInvalidBlipId);

    const auto got = session.core.blip(id);

    REQUIRE(got);
    CHECK(got->global);
}

TEST_CASE("editing a blip carries its targets, not just its looks",
          "[server][script]") {
    // Правка метки идёт целиком, и список тех, кому она видна, — её часть.
    // Не наложи мы его при правке, `addTarget` не сделал бы ничего: список
    // менялся бы у ресурса и не доезжал до сервера ни разу.
    Session session;

    script::BlipInfo wanted;
    wanted.targets = {4};

    const shared::BlipId id = session.core.createBlip(wanted);
    REQUIRE(id != shared::kInvalidBlipId);

    script::BlipInfo changed = wanted;
    changed.targets = {4, 5};

    REQUIRE(session.core.updateBlip(id, changed));

    const auto got = session.core.blip(id);

    REQUIRE(got);
    CHECK(got->targets == std::vector<shared::PlayerId>{4, 5});
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

TEST_CASE("a marker keeps the id the server gave it", "[server][script]") {
    Session session;

    script::MarkerInfo marker;
    marker.type = 27;
    marker.position = shared::Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F};
    marker.red = 200;

    const shared::MarkerId id = session.core.createMarker(marker);
    REQUIRE(id != shared::kInvalidMarkerId);

    // Номер назначает сервер, и попытка скрипта назвать свой ничего не меняет:
    // иначе маркер мог бы стать другим маркером.
    script::MarkerInfo pretender;
    pretender.id = id + 100;
    pretender.type = 1;
    REQUIRE(session.core.updateMarker(id, pretender));

    const auto shown = session.core.marker(id);
    REQUIRE(shown);
    CHECK(shown->id == id);
    CHECK(shown->type == 1);

    CHECK(session.core.removeMarker(id));
    CHECK_FALSE(session.core.marker(id));
    CHECK_FALSE(session.core.removeMarker(id));
}

TEST_CASE("a checkpoint keeps the id the server gave it", "[server][script]") {
    Session session;

    script::CheckpointInfo point;
    point.type = 4;
    point.radius = 5.0F;

    const shared::CheckpointId id = session.core.createCheckpoint(point);
    REQUIRE(id != shared::kInvalidCheckpointId);

    const auto shown = session.core.checkpoint(id);
    REQUIRE(shown);
    CHECK(shown->radius == Catch::Approx(5.0F));

    CHECK(session.core.removeCheckpoint(id));
    CHECK_FALSE(session.core.checkpoint(id));
}

TEST_CASE("the session refuses more markers than it allows", "[server][script]") {
    Session session;
    session.config.maxMarkers = 1;

    CHECK(session.core.createMarker({}) != shared::kInvalidMarkerId);
    CHECK(session.core.createMarker({}) == shared::kInvalidMarkerId);
}

// Тот же слой — не перемена, и переспрашивать нарисованное незачем: режим,
// ставящий измерение в цикле, иначе слал бы полную карту меток на каждый оборот.
TEST_CASE("staying in the same dimension is not a move", "[server][script]") {
    Session session;

    Player& player = session.join(1, "игрок");
    REQUIRE(session.core.setDimension(player.id, 7));

    session.sink.sent.clear();
    REQUIRE(session.core.setDimension(player.id, 7));

    CHECK(session.sink.sent.empty());
}

// Движение уходит всем, кто игрока видит, а не одному хозяину: у остальных
// персонаж показан куклой, и молчащая кукла осталась бы стоять столбом.
TEST_CASE("an animation is told about, not just performed", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");
    session.sink.sent.clear();

    script::AnimationInfo animation;
    animation.dictionary = "amb@world_human_smoking@male@male_a@base";
    animation.name = "base";

    REQUIRE(session.core.playAnimation(player.id, animation));
    CHECK(session.sink.sent ==
          std::vector<std::string>{"anim 0 amb@world_human_smoking@male@male_a@base/base"});

    // Снятие задач — то же распоряжение с пустым набором: по сети это одно и то
    // же «перестань делать то, что делаешь».
    session.sink.sent.clear();
    REQUIRE(session.core.clearTasks(player.id));
    CHECK(session.sink.sent == std::vector<std::string>{"anim 0 /"});
}

TEST_CASE("an animation for nobody changes nothing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.playAnimation(7, {}));
    CHECK_FALSE(session.core.clearTasks(7));
}

// --- Привязка сущностей ------------------------------------------------------

namespace {

script::EntityRef entity(shared::EntityKind kind, std::uint32_t id) {
    return script::EntityRef{.kind = kind, .id = id};
}

} // namespace

TEST_CASE("a door opens to the level it was told", "[server][script]") {
    Session session;
    session.join(1, "игрок");

    const shared::VehicleId car = session.core.createVehicle(0xDEADBEEF, {}, 0.0F);
    REQUIRE(car != shared::kInvalidVehicleId);

    // Приоткрытая, а не настежь: восемь ступеней у alt:V заведены ровно затем,
    // чтобы отличать одно от другого.
    REQUIRE(session.core.setVehicleDoor(car, 4, 3));
    CHECK(std::ranges::count(session.sink.sent, std::format("vehicle doors {}", car)) == 1);

    auto shown = session.core.vehicle(car);
    REQUIRE(shown.has_value());
    CHECK(shared::doorLevel(shown->doorLevels, 4) == 3);

    // Соседние двери не тронуты: степени лежат в одном числе, и запись одной не
    // вправе стереть остальные.
    CHECK(shared::doorLevel(shown->doorLevels, 0) == 0);
    CHECK(shared::doorLevel(shown->doorLevels, 5) == 0);

    REQUIRE(session.core.setVehicleDoor(car, 0, shared::kDoorFullyOpen));

    shown = session.core.vehicle(car);
    REQUIRE(shown.has_value());
    CHECK(shared::doorLevel(shown->doorLevels, 0) == shared::kDoorFullyOpen);
    CHECK(shared::doorLevel(shown->doorLevels, 4) == 3);
}

TEST_CASE("a door that does not exist is refused", "[server][script]") {
    Session session;
    const shared::VehicleId car = session.core.createVehicle(0xDEADBEEF, {}, 0.0F);

    // Молча проглоченные, они выглядели бы как сделанное и невидимое.
    CHECK_FALSE(session.core.setVehicleDoor(car, 6, 1));
    CHECK_FALSE(session.core.setVehicleDoor(car, 0, 8));
    CHECK_FALSE(session.core.setVehicleDoor(99, 0, 1));
}

TEST_CASE("a locked vehicle stays locked and says so once", "[server][script]") {
    Session session;
    session.join(1, "игрок");

    const shared::VehicleId car = session.core.createVehicle(0xDEADBEEF, {}, 0.0F);
    REQUIRE(car != shared::kInvalidVehicleId);

    REQUIRE(session.core.setVehicleLock(car, 2));
    CHECK(std::ranges::count(session.sink.sent, std::format("vehicle lock {}", car)) == 1);

    // Замок помнит сервер: без этого запертая машина открывалась бы всякому,
    // кто подошёл к ней позже, — он о замке не слышал.
    const auto shown = session.core.vehicle(car);
    REQUIRE(shown.has_value());
    CHECK(shown->lockState == 2);
}

TEST_CASE("locking a vehicle that is gone is refused", "[server][script]") {
    Session session;
    CHECK_FALSE(session.core.setVehicleLock(99, 2));
}

TEST_CASE("freezing a player is told once, not every time", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setFrozen(player.id, true));
    CHECK(std::ranges::count(session.sink.sent, std::format("control {} 1", player.id)) == 1);

    // Второй раз то же самое не рассылается: признаки ставят из обработчиков,
    // которые идут каждый такт, и слать одно и то же тридцать раз в секунду
    // значило бы платить за ничто.
    REQUIRE(session.core.setFrozen(player.id, true));
    CHECK(std::ranges::count(session.sink.sent, std::format("control {} 1", player.id)) == 1);
}

TEST_CASE("both flags of the body live side by side", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setFrozen(player.id, true));
    REQUIRE(session.core.setInvincible(player.id, true));

    // Второй признак не стирает первый: уходят они целиком, и ставящий один
    // обязан сохранить другой.
    const auto shown = session.core.player(player.id);
    REQUIRE(shown.has_value());

    CHECK(shared::has(shown->control, shared::PlayerControlFlag::Frozen));
    CHECK(shared::has(shown->control, shared::PlayerControlFlag::Invincible));

    // И снятие одного не трогает второй.
    REQUIRE(session.core.setFrozen(player.id, false));

    const auto after = session.core.player(player.id);
    REQUIRE(after.has_value());

    CHECK_FALSE(shared::has(after->control, shared::PlayerControlFlag::Frozen));
    CHECK(shared::has(after->control, shared::PlayerControlFlag::Invincible));
}

TEST_CASE("a body of nobody is not commanded", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.setFrozen(99, true));
    CHECK_FALSE(session.core.setInvincible(99, true));
}

TEST_CASE("one weapon can be taken away without touching the rest",
          "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.giveWeapon(player.id, 0xAABB, 50, false));
    REQUIRE(session.core.giveWeapon(player.id, 0xCCDD, 120, false));

    REQUIRE(session.core.removeWeapon(player.id, 0xAABB));

    const auto left = session.core.loadout(player.id);

    REQUIRE(left.size() == 1);
    CHECK(left.front().weapon == 0xCCDD);
    CHECK(left.front().ammo == 120);

    // Уходит оно заменой, а не добавкой: иначе игра оставила бы отобранный ствол
    // у игрока — она о нём не забывала.
    CHECK(std::ranges::count(session.sink.sent,
                             std::format("loadout {} 1 replace", player.id)) == 1);
}

TEST_CASE("taking away a weapon nobody had is refused", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    // Молчаливое согласие здесь было бы ложью: просивший решил бы, что ствол
    // отобран, — а его и не было.
    CHECK_FALSE(session.core.removeWeapon(player.id, 0xAABB));
    CHECK_FALSE(session.core.removeWeapon(player.id, 0));
    CHECK_FALSE(session.core.removeWeapon(99, 0xAABB));
}

TEST_CASE("a face assembled by the server comes back the same", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setHeadBlend(player.id, 21, 33, 0, 14, 27, 0, 0.75F, 0.25F, 0.0F));

    const auto look = session.core.appearance(player.id);
    REQUIRE(look.has_value());

    // Порядок доводов здесь перепутать легче всего: шесть чисел подряд, и все
    // законные. Перепутанные, они собрали бы другое лицо без единой жалобы.
    CHECK(look->shapeFirst == 21);
    CHECK(look->shapeSecond == 33);
    CHECK(look->skinFirst == 14);
    CHECK(look->skinSecond == 27);
    CHECK(look->shapeMix == 0.75F);
    CHECK(look->skinMix == 0.25F);
}

TEST_CASE("a tattoo goes on once and comes off by name", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.addDecoration(player.id, 0xAAAA, 0xBBBB));

    // Вторая такая же не заводится: игра держит по одной каждого вида, и список
    // рос бы на каждую выдачу — как у насадок на оружие.
    REQUIRE(session.core.addDecoration(player.id, 0xAAAA, 0xBBBB));

    auto look = session.core.appearance(player.id);
    REQUIRE(look.has_value());
    CHECK(look->decorations.size() == 1);

    REQUIRE(session.core.addDecoration(player.id, 0xAAAA, 0xCCCC));

    look = session.core.appearance(player.id);
    REQUIRE(look.has_value());
    CHECK(look->decorations.size() == 2);

    REQUIRE(session.core.removeDecoration(player.id, 0xAAAA, 0xBBBB));

    look = session.core.appearance(player.id);
    REQUIRE(look.has_value());
    REQUIRE(look->decorations.size() == 1);
    CHECK(look->decorations.front().overlay == 0xCCCC);

    REQUIRE(session.core.clearDecorations(player.id));

    look = session.core.appearance(player.id);
    REQUIRE(look.has_value());
    CHECK(look->decorations.empty());
}

TEST_CASE("a tattoo out of nothing is refused", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    // Пустой хеш — не татуировка. Молча приняв его, мы поставили бы персонажу
    // ничто и объявили бы это сделанным.
    CHECK_FALSE(session.core.addDecoration(player.id, 0, 0xBBBB));
    CHECK_FALSE(session.core.addDecoration(player.id, 0xAAAA, 0));
    CHECK_FALSE(session.core.addDecoration(99, 0xAAAA, 0xBBBB));

    // И снять ту, которой не было, тоже нельзя молча.
    CHECK_FALSE(session.core.removeDecoration(player.id, 0xAAAA, 0xBBBB));
}

TEST_CASE("a face feature comes back the way it was set", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setFaceFeature(player.id, 3, 0.5F));

    const auto look = session.core.appearance(player.id);
    REQUIRE(look.has_value());

    // По сети черта идёт байтом в сто двадцать семь ступеней; половина от
    // единицы это шестьдесят три с половиной, то есть шестьдесят три.
    CHECK(look->faceFeatures[3] == 63);

    // Остальные девятнадцать не тронуты: черты двигают по одной.
    CHECK(look->faceFeatures[0] == 0);
    CHECK(look->faceFeatures[19] == 0);
}

TEST_CASE("a face feature outside its range is cut to it", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setFaceFeature(player.id, 0, 5.0F));
    REQUIRE(session.core.setFaceFeature(player.id, 1, -5.0F));

    const auto look = session.core.appearance(player.id);
    REQUIRE(look.has_value());

    // Обрезается здесь, а не только у игры: внешность сервер помнит и
    // пересказывает вошедшим позже.
    CHECK(look->faceFeatures[0] == 127);
    CHECK(look->faceFeatures[1] == -127);
}

TEST_CASE("a face feature that does not exist is refused", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    CHECK_FALSE(session.core.setFaceFeature(player.id, 20, 0.5F));
    CHECK_FALSE(session.core.setFaceFeature(99, 0, 0.5F));
}

TEST_CASE("blend shares outside their range are cut to it", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setHeadBlend(player.id, 0, 0, 0, 0, 0, 0, 3.5F, -1.0F, 0.5F));

    const auto look = session.core.appearance(player.id);
    REQUIRE(look.has_value());

    // Обрезаются здесь, а не у клиента, и это существенно: внешность сервер
    // помнит и пересказывает вошедшим позже. Сохрани он долю, которой игра не
    // приняла, — вошедшие увидели бы одно лицо, а хозяин у себя другое.
    CHECK(look->shapeMix == 1.0F);
    CHECK(look->skinMix == 0.0F);
    CHECK(look->thirdMix == 0.5F);
}

TEST_CASE("a layer of the face keeps its colour when the layer changes",
          "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setHeadOverlay(player.id, 1, 5, 0.8F));
    REQUIRE(session.core.setHeadOverlayColour(player.id, 1, 1, 4, 4));

    // Смена самого слоя цвет не трогает: ставят их разными вызовами, и стирать
    // сделанное соседним было бы неожиданностью.
    REQUIRE(session.core.setHeadOverlay(player.id, 1, 6, 0.5F));

    const auto look = session.core.appearance(player.id);
    REQUIRE(look.has_value());

    CHECK(look->overlays[1].index == 6);
    CHECK(look->overlays[1].opacity == 0.5F);
    CHECK(look->overlays[1].colourType == 1);
    CHECK(look->overlays[1].colour == 4);
}

TEST_CASE("a layer outside the thirteen is refused", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    // Молча проглоченный, такой слой выглядел бы поставленным и невидимым — то
    // же самое правило, что и у слотов одежды.
    CHECK_FALSE(session.core.setHeadOverlay(player.id, 99, 1, 1.0F));
    CHECK_FALSE(session.core.setHeadOverlayColour(player.id, 99, 1, 1, 1));
}

TEST_CASE("hair and eye colours are told and remembered", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    REQUIRE(session.core.setHairColour(player.id, 12, 3));
    REQUIRE(session.core.setEyeColour(player.id, 7));

    const auto look = session.core.appearance(player.id);
    REQUIRE(look.has_value());

    CHECK(look->hairColour == 12);
    CHECK(look->hairHighlight == 3);
    CHECK(look->eyeColour == 7);
}

TEST_CASE("a player who never said how they look has no appearance",
          "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    // Пустота, а не описание из нулей. Нули означали бы лысого голого человека
    // с белыми глазами, и спросивший принял бы это за правду; сервер же
    // внешности не выдумывает.
    CHECK_FALSE(session.core.appearance(player.id).has_value());
    CHECK_FALSE(session.core.appearance(99).has_value());
}

TEST_CASE("an object that moved says so", "[server][script]") {
    Session session;
    session.join(1, "игрок");

    const shared::ObjectId box = session.core.createObject(0xBADF00D, shared::Vec3{}, {});
    REQUIRE(box != shared::kInvalidObjectId);

    const shared::Vec3 where{.x = 10.0F, .y = -20.0F, .z = 30.0F};
    const shared::Vec3 turn{.x = 0.0F, .y = 0.0F, .z = 90.0F};

    REQUIRE(session.core.moveObject(box, where, turn));

    // Повод объявляется, и объявляется отдельно от появления: у тех, кто предмет
    // уже видит, раздача его больше не тронет, и не скажи им никто — предмет
    // остался бы стоять на старом месте до конца сессии.
    CHECK(std::ranges::count(session.sink.sent, std::format("object= {}", box)) == 1);

    const auto moved = session.core.object(box);
    REQUIRE(moved.has_value());

    CHECK(moved->position == where);
    CHECK(moved->rotation == turn);
}

TEST_CASE("moving an object that is gone is refused", "[server][script]") {
    Session session;

    // Молчаливое согласие здесь было бы ложью: просивший решил бы, что предмет
    // стоит там, куда он его послал.
    CHECK_FALSE(session.core.moveObject(99, shared::Vec3{}, shared::Vec3{}));
}

TEST_CASE("an object hangs on a player and everyone hears about it", "[server][script]") {
    Session session;
    session.join(1, "игрок");

    const shared::ObjectId box = session.core.createObject(0xBADF00D, shared::Vec3{}, {});
    REQUIRE(box != shared::kInvalidObjectId);

    script::AttachmentInfo worn;
    worn.target = entity(shared::EntityKind::Player, 0);
    worn.boneName = "SKEL_R_Hand";
    worn.position = shared::Vec3{.x = 0.1F, .y = 0.0F, .z = 0.0F};

    REQUIRE(session.core.attachEntity(entity(shared::EntityKind::Object, box), worn));

    CHECK(std::ranges::count(session.sink.sent,
                             std::format("attach {} {}",
                                         static_cast<int>(shared::EntityKind::Object), box)) == 1);

    const auto got = session.core.attachment(entity(shared::EntityKind::Object, box));
    REQUIRE(got.has_value());

    CHECK(got->target == entity(shared::EntityKind::Player, 0));
    CHECK(got->boneName == "SKEL_R_Hand");
}

TEST_CASE("hanging on something that is not there is refused", "[server][script]") {
    Session session;

    const shared::ObjectId box = session.core.createObject(0xBADF00D, shared::Vec3{}, {});
    REQUIRE(box != shared::kInvalidObjectId);

    script::AttachmentInfo worn;
    worn.target = entity(shared::EntityKind::Vehicle, 42);

    // Молчаливое согласие здесь было бы хуже отказа: предмет остался бы висеть в
    // никуда, и отвязать его было бы уже некому.
    CHECK_FALSE(session.core.attachEntity(entity(shared::EntityKind::Object, box), worn));

    // И наоборот: вешать то, чего нет, тоже не на что.
    worn.target = entity(shared::EntityKind::Object, box);
    CHECK_FALSE(session.core.attachEntity(entity(shared::EntityKind::Object, 99), worn));
}

TEST_CASE("removing the thing it hangs on loosens what hung", "[server][script]") {
    Session session;

    const shared::VehicleId car = session.core.createVehicle(0xB779A091, shared::Vec3{}, 0.0F);
    const shared::ObjectId box = session.core.createObject(0xBADF00D, shared::Vec3{}, {});

    REQUIRE(car != shared::kInvalidVehicleId);
    REQUIRE(box != shared::kInvalidObjectId);

    script::AttachmentInfo worn;
    worn.target = entity(shared::EntityKind::Vehicle, car);

    REQUIRE(session.core.attachEntity(entity(shared::EntityKind::Object, box), worn));
    REQUIRE(session.core.removeVehicle(car));

    // Клиентам сказано дважды: первый раз о привязке, второй — о её снятии.
    CHECK(std::ranges::count(session.sink.sent,
                             std::format("attach {} {}",
                                         static_cast<int>(shared::EntityKind::Object), box)) == 2);

    CHECK_FALSE(session.core.attachment(entity(shared::EntityKind::Object, box)).has_value());
}

TEST_CASE("detaching what hangs on nothing changes nothing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.detachEntity(entity(shared::EntityKind::Object, 1)));
    CHECK(std::ranges::none_of(session.sink.sent, [](const std::string& line) {
        return line.starts_with("attach");
    }));
}

// --- Прохожие ----------------------------------------------------------------

namespace {

/// Кукла у начала координат: модель и здоровье, остального проверкам не нужно.
script::PedInfo standing(std::uint32_t model) {
    script::PedInfo ped;
    ped.model = model;
    ped.position = shared::Vec3{.x = 1.0F, .y = 2.0F, .z = 3.0F};
    return ped;
}

} // namespace

TEST_CASE("a ped taken away is announced as gone before it is gone",
          "[server][script]") {
    // До уборки, а не после: обработчик вправе спросить у уходящего модель или
    // положение, а после уборки ссылка на него уже ничего не расскажет.
    Session session;
    Recorder recorder;

    const shared::PedId id = session.core.createPed(standing(0x9C9EFFD8));
    REQUIRE(id != shared::kInvalidPedId);

    session.events.subscribe(&recorder);

    REQUIRE(session.core.removePed(id));

    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().kind == script::EventKind::RemoveEntity);
}

TEST_CASE("a ped that ran out of health is announced dead", "[server][script]") {
    Session session;
    Recorder recorder;

    const shared::PedId id = session.core.createPed(standing(0x9C9EFFD8));
    REQUIRE(id != shared::kInvalidPedId);

    session.events.subscribe(&recorder);

    script::PedInfo dead = standing(0x9C9EFFD8);
    dead.health = 0;

    REQUIRE(session.core.updatePed(id, dead));

    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().kind == script::EventKind::PedDeath);
}

TEST_CASE("a ped given health back is announced healed, not killed",
          "[server][script]") {
    // Смерть и лечение разводятся так же, как у игрока: смерть это обнуление
    // здоровья у живого, лечение — его рост. Урона у прохожего нет вовсе: у
    // урона есть ударивший, а о попаданиях по прохожим клиент не сообщает.
    Session session;
    Recorder recorder;

    script::PedInfo hurt = standing(0x9C9EFFD8);
    hurt.health = 40;

    const shared::PedId id = session.core.createPed(hurt);
    REQUIRE(id != shared::kInvalidPedId);

    session.events.subscribe(&recorder);

    script::PedInfo whole = standing(0x9C9EFFD8);
    whole.health = 190;

    REQUIRE(session.core.updatePed(id, whole));

    REQUIRE(recorder.seen.size() == 1);
    CHECK(recorder.seen.front().kind == script::EventKind::PedHeal);
}

TEST_CASE("a ped keeps the id the server gave it", "[server][script]") {
    Session session;

    const shared::PedId id = session.core.createPed(standing(0x9C9EFFD8));
    REQUIRE(id != shared::kInvalidPedId);

    const std::optional<script::PedInfo> got = session.core.ped(id);
    REQUIRE(got.has_value());

    CHECK(got->id == id);
    CHECK(got->model == 0x9C9EFFD8);
    CHECK(got->position.x == 1.0F);

    CHECK(std::ranges::count(session.sink.sent, std::format("ped {}", id)) == 1);
}

TEST_CASE("a ped without a model is refused", "[server][script]") {
    Session session;

    // Отказ, а не пустая кукла: модель нулём — это не прохожий, и показать его
    // будет нечем.
    CHECK(session.core.createPed(standing(0)) == shared::kInvalidPedId);
    CHECK(session.core.peds().empty());
}

TEST_CASE("a ped is healed and armed whole, not field by field", "[server][script]") {
    Session session;

    const shared::PedId id = session.core.createPed(standing(0x9C9EFFD8));
    REQUIRE(id != shared::kInvalidPedId);

    script::PedInfo changed = *session.core.ped(id);
    changed.health = 150;
    changed.armour = 50;
    changed.weapon = 0x1B06D571;

    REQUIRE(session.core.updatePed(id, changed));

    const std::optional<script::PedInfo> got = session.core.ped(id);
    REQUIRE(got.has_value());

    CHECK(got->health == 150);
    CHECK(got->armour == 50);
    CHECK(got->weapon == 0x1B06D571);

    // Второй раз: заведение и правка объявляются одинаково — получателю разницы
    // нет, а нам не нужно помнить, знает он уже об этой кукле или нет.
    CHECK(std::ranges::count(session.sink.sent, std::format("ped {}", id)) == 2);
}

// Смена модели — это не правка, а новое тело: получателю пришлось бы убрать
// куклу и завести заново, а он об этом не узнает. Поэтому модель не меняется.
TEST_CASE("a ped cannot be turned into a different model", "[server][script]") {
    Session session;

    const shared::PedId id = session.core.createPed(standing(0x9C9EFFD8));
    REQUIRE(id != shared::kInvalidPedId);

    script::PedInfo changed = *session.core.ped(id);
    changed.model = 0xDEADBEEF;

    REQUIRE(session.core.updatePed(id, changed));
    CHECK(session.core.ped(id)->model == 0x9C9EFFD8);
}

TEST_CASE("a ped that is gone refuses both editing and removing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.updatePed(1, standing(0x9C9EFFD8)));
    CHECK_FALSE(session.core.removePed(1));
    CHECK_FALSE(session.core.setPedDimension(1, 5));
    CHECK_FALSE(session.core.ped(1).has_value());
}

TEST_CASE("removing a ped loosens what hung on it", "[server][script]") {
    Session session;

    const shared::PedId guard = session.core.createPed(standing(0x9C9EFFD8));
    const shared::ObjectId box = session.core.createObject(0xBADF00D, shared::Vec3{}, {});

    REQUIRE(guard != shared::kInvalidPedId);
    REQUIRE(box != shared::kInvalidObjectId);

    script::AttachmentInfo worn;
    worn.target = script::EntityRef{.kind = shared::EntityKind::Ped, .id = guard};

    REQUIRE(session.core.attachEntity(
        script::EntityRef{.kind = shared::EntityKind::Object, .id = box}, worn));

    REQUIRE(session.core.removePed(guard));

    CHECK_FALSE(session.core
                    .attachment(script::EntityRef{.kind = shared::EntityKind::Object, .id = box})
                    .has_value());
}

TEST_CASE("the session refuses more peds than it allows", "[server][script]") {
    Session session;
    session.config.maxPeds = 1;

    CHECK(session.core.createPed(standing(0x9C9EFFD8)) != shared::kInvalidPedId);
    CHECK(session.core.createPed(standing(0x9C9EFFD8)) == shared::kInvalidPedId);
}

// Насадка без ствола — насадка ни на чём: запомнить её молча значило бы
// пообещать то, чего не будет.
TEST_CASE("a weapon component needs the weapon to be there", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    constexpr std::uint32_t kRifle = 0xBFEFFF6D;
    constexpr std::uint32_t kScope = 0xA0D89C42;

    CHECK_FALSE(session.core.addWeaponComponent(player.id, kRifle, kScope));

    REQUIRE(session.core.giveWeapon(player.id, kRifle, 120, false));
    REQUIRE(session.core.addWeaponComponent(player.id, kRifle, kScope));

    REQUIRE(player.loadout.size() == 1);
    CHECK(player.loadout[0].components == std::vector<std::uint32_t>{kScope});

    // Та же насадка второй раз списка не растит: игра держит по одной каждого
    // вида, и повторная выдача ничего не меняет.
    REQUIRE(session.core.addWeaponComponent(player.id, kRifle, kScope));
    CHECK(player.loadout[0].components.size() == 1);

    CHECK(session.core.removeWeaponComponent(player.id, kRifle, kScope));
    CHECK(player.loadout[0].components.empty());

    // Снимать нечего — отказ, а не тишина.
    CHECK_FALSE(session.core.removeWeaponComponent(player.id, kRifle, kScope));
}

TEST_CASE("a weapon tint needs the weapon too", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    constexpr std::uint32_t kPistol = 0x1B06D571;

    CHECK_FALSE(session.core.setWeaponTint(player.id, kPistol, 3));

    REQUIRE(session.core.giveWeapon(player.id, kPistol, 50, false));
    REQUIRE(session.core.setWeaponTint(player.id, kPistol, 3));

    REQUIRE(player.loadout.size() == 1);
    CHECK(player.loadout[0].tint == 3);
}

// Снятая насадка требует отбора оружия: игра не снимает поставленное сама, и
// список без насадки для неё неотличим от списка без насадки.
TEST_CASE("removing a component asks for the weapon to be given anew",
          "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    constexpr std::uint32_t kRifle = 0xBFEFFF6D;
    constexpr std::uint32_t kScope = 0xA0D89C42;

    REQUIRE(session.core.giveWeapon(player.id, kRifle, 120, false));
    REQUIRE(session.core.addWeaponComponent(player.id, kRifle, kScope));

    session.sink.sent.clear();
    REQUIRE(session.core.removeWeaponComponent(player.id, kRifle, kScope));

    CHECK(session.sink.sent == std::vector<std::string>{"loadout 0 1 replace"});
}

TEST_CASE("weapon components for nobody change nothing", "[server][script]") {
    Session session;

    CHECK_FALSE(session.core.addWeaponComponent(7, 1, 2));
    CHECK_FALSE(session.core.removeWeaponComponent(7, 1, 2));
    CHECK_FALSE(session.core.setWeaponTint(7, 1, 2));
}

// Адрес и задержка — свойства игрока, а не отдельный вопрос к серверу: режим
// спрашивает их в обработчике входа, вместе с именем.
TEST_CASE("a player carries their address and latency", "[server][script]") {
    Session session;
    Player& player = session.join(1, "игрок");

    const auto shown = session.core.player(player.id);

    REQUIRE(shown);
    CHECK(shown->ip == "10.0.0.0");
    CHECK(shown->ping == 40);
}
