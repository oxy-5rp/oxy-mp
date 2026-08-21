#include "server_core.hpp"

#include <algorithm>
#include <string>

namespace oxymp::server {
namespace {

/// Что о нём знает скрипт.
[[nodiscard]] script::PlayerInfo describe(const Player& player, const VehicleDirectory& vehicles,
                                          const Config& config) {
    const VehicleDirectory::Seat seat = vehicles.seatOf(player.id);

    return script::PlayerInfo{
        .id = player.id,
        .nickname = player.nickname,
        .position = player.position,
        .heading = player.heading,
        .health = player.health,
        .armour = player.armour,
        .model = player.appearance ? player.appearance->model : 0,
        .vehicle = seat.vehicle,
        .seat = seat.index,
        .dimension = player.dimension,

        // Право живёт в настройках сервера, а не у скрипта: назначает
        // распорядителей хозяин сессии, и подменять его решение ресурсу
        // непозволительно. Видеть же ответ ресурс обязан — иначе ему нельзя
        // доверить ничего, что меняет мир.
        .admin = std::ranges::find(config.admins, player.id) != config.admins.end(),
    };
}

[[nodiscard]] script::VehicleInfo describe(shared::VehicleId id,
                                           const VehicleDirectory::Vehicle& vehicle) {
    return script::VehicleInfo{
        .id = id,
        .model = vehicle.state.model,
        .position = vehicle.state.position,
        .rotation = vehicle.state.rotation,
        .owner = vehicle.owner,
        .dimension = vehicle.dimension,
    };
}

[[nodiscard]] script::ObjectInfo describe(shared::ObjectId id,
                                          const ObjectDirectory::Object& object) {
    return script::ObjectInfo{
        .id = id,
        .model = object.model,
        .position = object.position,
        .rotation = object.rotation,
        .dimension = object.dimension,
    };
}

} // namespace

ServerCore::ServerCore(PlayerRegistry& players, VehicleDirectory& vehicles, ObjectDirectory& objects,
                       WorldClock& world, const Config& config, script::Events& events,
                       CoreSink& sink) noexcept
    : players_(&players), vehicles_(&vehicles), objects_(&objects), world_(&world),
      config_(&config), events_(&events), sink_(&sink) {}

std::vector<script::PlayerInfo> ServerCore::players() const {
    std::vector<script::PlayerInfo> everyone;
    everyone.reserve(players_->size());

    for (const auto& [peer, player] : *players_) {
        everyone.push_back(describe(player, *vehicles_, *config_));
    }

    return everyone;
}

std::optional<script::PlayerInfo> ServerCore::player(shared::PlayerId id) const {
    const Player* found = players_->findById(id);
    return found == nullptr ? std::nullopt : std::optional{describe(*found, *vehicles_, *config_)};
}

bool ServerCore::setHealth(shared::PlayerId id, std::uint16_t health, std::uint16_t armour) {
    Player* player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    const bool wasAlive = player->health != 0;

    // Обрезается, а не принимается как есть. Скрипт вправе ошибиться в числе, и
    // ошибка эта тихая: игра больше двухсот единиц не показывает, и человек с
    // шестьюдесятью тысячами выглядит здоровым ровно так же, как целый, — просто
    // не умирает.
    player->health = std::min(health, kFullHealth);
    player->armour = std::min(armour, kFullArmour);

    sink_->healthChanged(*player);

    // Смерть по воле скрипта — тоже смерть, и объявить её нужно. Убийцы у неё
    // нет: тот, кто убил чужой рукой, объявляет её сам и называет стрелявшего —
    // здесь же известно только, что здоровья не осталось.
    if (wasAlive && player->health == 0) {
        script::Event death;
        death.kind = script::EventKind::PlayerDeath;
        death.player = script::Player{*this, id};

        events_->dispatch(death);
    }

    return true;
}

bool ServerCore::giveWeapon(shared::PlayerId id, std::uint32_t weapon, std::uint16_t ammo) {
    if (weapon == 0) {
        return false;
    }

    Player* player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Уже выданное оружие не заводится вторым: игроку меняются патроны. Иначе
    // список рос бы на каждую выдачу, а игра всё равно держит по одному стволу
    // каждого вида.
    const auto known = std::ranges::find(player->loadout, weapon, &shared::WeaponSlot::weapon);

    if (known != player->loadout.end()) {
        known->ammo = ammo;
    } else if (player->loadout.size() < shared::kMaxWeaponSlots) {
        player->loadout.push_back(shared::WeaponSlot{.weapon = weapon, .ammo = ammo});
    } else {
        return false;
    }

    sink_->loadoutChanged(*player, false);
    return true;
}

bool ServerCore::clearWeapons(shared::PlayerId id) {
    Player* player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    player->loadout.clear();

    // С заменой: пустой список без этого признака означал бы «добавить ничего»,
    // и оружие осталось бы у игрока в руках.
    sink_->loadoutChanged(*player, true);
    return true;
}

/// Внешность игрока, заведённая при надобности.
///
/// Её может не быть вовсе: игрок объявляет свою не сразу, а сервер вправе
/// одеть его хоть в обработчике входа — то есть раньше. Тогда она заводится
/// здесь пустой, а остальное игрок допишет своим объявлением.
namespace {

[[nodiscard]] shared::PlayerAppearance& appearanceOf(Player& player) {
    if (!player.appearance) {
        player.appearance.emplace();
        player.appearance->playerId = player.id;
    }

    return *player.appearance;
}

} // namespace

bool ServerCore::setClothes(shared::PlayerId id, std::uint8_t component, std::uint8_t drawable,
                            std::uint8_t texture, std::uint8_t palette) {
    Player* const player = players_->findById(id);
    if (player == nullptr || component >= shared::kPedComponentCount) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);

    look.components[component] = shared::PedComponent{
        .drawable = drawable,
        .texture = texture,
        .palette = palette,
    };

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setProp(shared::PlayerId id, std::uint8_t index, std::int8_t drawable,
                         std::int8_t texture) {
    Player* const player = players_->findById(id);
    if (player == nullptr || index >= shared::kPedPropCount) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);

    look.props[index] = shared::PedProp{.drawable = drawable, .texture = texture};

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setDimension(shared::PlayerId id, std::int32_t dimension) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    player->dimension = dimension;

    // Рассказывать об этом клиентам нечем и незачем: они про измерения не знают
    // вовсе. Перемена скажется сама собой на ближайшей рассылке — тем, кто его
    // больше видеть не должен, снимки просто перестанут приходить.
    return true;
}

bool ServerCore::setModel(shared::PlayerId id, std::uint32_t model) {
    Player* player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Ноль — «оставить как есть»: так же его толкует и сама PlayerAppearance.
    // Отдельного распоряжения «сними модель» у игры нет, и выдумывать его,
    // подставляя ноль в поле, значило бы оставить игрока без тела.
    if (model == 0) {
        return false;
    }

    // Внешности может не быть вовсе: игрок объявляет свою не сразу, а сервер
    // вправе назначить модель хоть в обработчике входа — то есть раньше. Тогда
    // она заводится здесь, пустой, с одной лишь моделью, и остальное игрок
    // допишет своим объявлением.
    if (!player->appearance) {
        player->appearance.emplace();
    }

    player->appearance->playerId = id;
    player->appearance->model = model;

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::teleport(shared::PlayerId id, const shared::Vec3& position) {
    const Player* player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Положение у себя не меняется: правду о нём приносит снимок игрока, и
    // придумай мы её здесь — она разошлась бы с настоящей до первого снимка, а
    // по ней сервер решает, кому какую машину вести и кому о чём рассказывать.
    sink_->teleported(*player, position);
    return true;
}

bool ServerCore::kick(shared::PlayerId id, std::string_view reason) {
    const Player* player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Из реестра игрок здесь не убирается, и это существенно. Уборка — дело
    // одного места, обработчика разрыва соединения: она объявляет
    // playerDisconnect, передаёт машины другим ведущим и рассылает уход
    // остальным. Убери мы игрока здесь — всё это либо не случилось бы вовсе,
    // либо случилось дважды.
    sink_->kicked(*player, reason);
    return true;
}

bool ServerCore::emit(shared::PlayerId id, std::string_view name, std::string_view payload) {
    const Player* player = players_->findById(id);

    if (player == nullptr || name.empty()) {
        return false;
    }

    sink_->emitted(*player, name, payload);
    return true;
}

std::vector<script::VehicleInfo> ServerCore::vehicles() const {
    std::vector<script::VehicleInfo> everything;
    everything.reserve(vehicles_->size());

    for (const auto& [id, vehicle] : vehicles_->all()) {
        everything.push_back(describe(id, vehicle));
    }

    return everything;
}

std::optional<script::VehicleInfo> ServerCore::vehicle(shared::VehicleId id) const {
    const VehicleDirectory::Vehicle* found = vehicles_->find(id);
    return found == nullptr ? std::nullopt : std::optional{describe(id, *found)};
}

shared::VehicleId ServerCore::createVehicle(std::uint32_t model, const shared::Vec3& position,
                                            float heading) {
    // Без ведущего. Скрипт ставит машину куда угодно — хоть на другой конец
    // карты, где никого нет, — и назначить ведущим его самого было бы неверно:
    // ведущий обязан видеть машину по-настоящему, иначе она не поедет ни у кого.
    // Кому её вести, решит ближайший пересмотр по расстояниям.
    const shared::VehicleId id = vehicles_->add(model, position, heading,
                                                shared::kInvalidPlayerId, config_->maxVehicles);

    if (id == shared::kInvalidVehicleId) {
        return shared::kInvalidVehicleId;
    }

    sink_->vehicleAdded(id);

    script::Event created;
    created.kind = script::EventKind::VehicleCreate;
    created.vehicle = script::Vehicle{*this, id};

    events_->dispatch(created);

    return id;
}

bool ServerCore::setVehicleDimension(shared::VehicleId id, std::int32_t dimension) {
    return vehicles_->setDimension(id, dimension);
}

bool ServerCore::removeVehicle(shared::VehicleId id) {
    if (vehicles_->find(id) == nullptr) {
        return false;
    }

    // Объявляется до уборки, а не после. Обработчик вправе спросить у машины
    // модель или положение — записать в журнал, поставить на её место другую, —
    // а после уборки ссылка на неё уже ничего не расскажет.
    script::Event destroyed;
    destroyed.kind = script::EventKind::VehicleDestroy;
    destroyed.vehicle = script::Vehicle{*this, id};

    events_->dispatch(destroyed);

    // Обработчик мог убрать её сам — тогда убирать нечего, и это не ошибка.
    if (!vehicles_->remove(id)) {
        return false;
    }

    sink_->vehicleRemoved(id);
    return true;
}

std::vector<script::ObjectInfo> ServerCore::objects() const {
    std::vector<script::ObjectInfo> everything;
    everything.reserve(objects_->size());

    for (const auto& [id, object] : objects_->all()) {
        everything.push_back(describe(id, object));
    }

    return everything;
}

std::optional<script::ObjectInfo> ServerCore::object(shared::ObjectId id) const {
    const ObjectDirectory::Object* found = objects_->find(id);
    return found == nullptr ? std::nullopt : std::optional{describe(id, *found)};
}

shared::ObjectId ServerCore::createObject(std::uint32_t model, const shared::Vec3& position,
                                          const shared::Vec3& rotation) {
    const shared::ObjectId id = objects_->add(model, position, rotation, config_->maxObjects);

    if (id == shared::kInvalidObjectId) {
        return shared::kInvalidObjectId;
    }

    sink_->objectAdded(id);
    return id;
}

bool ServerCore::setObjectDimension(shared::ObjectId id, std::int32_t dimension) {
    return objects_->setDimension(id, dimension);
}

bool ServerCore::removeObject(shared::ObjectId id) {
    if (!objects_->remove(id)) {
        return false;
    }

    sink_->objectRemoved(id);
    return true;
}

void ServerCore::broadcast(std::string_view text) {
    if (text.empty()) {
        return;
    }

    sink_->chatLine(shared::kInvalidPlayerId, std::string{text});
}

bool ServerCore::tell(shared::PlayerId id, std::string_view text) {
    if (text.empty() || players_->findById(id) == nullptr) {
        return false;
    }

    sink_->chatLine(id, std::string{text});
    return true;
}

bool ServerCore::setWeather(std::string_view weather) {
    if (!world_->setWeather(std::string{weather})) {
        return false;
    }

    sink_->worldChanged();
    return true;
}

bool ServerCore::setTime(std::uint8_t hour, std::uint8_t minute) {
    // Секунды здесь нет намеренно: скрипту она не нужна — время суток ставят
    // ровным, — а часы сервера идут сами и отсчитают её сами.
    if (!world_->setTime(hour, minute, 0)) {
        return false;
    }

    sink_->worldChanged();
    return true;
}

} // namespace oxymp::server
