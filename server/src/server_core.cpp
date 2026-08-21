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

/// Метка, какой её знает скрипт.
[[nodiscard]] script::BlipInfo describe(const BlipDirectory::Entry& blip) {
    return script::BlipInfo{
        .id = blip.state.id,
        .position = blip.state.position,
        .sprite = blip.state.sprite,
        .colour = blip.state.colour,
        .alpha = blip.state.alpha,
        .display = blip.state.display,
        .shortRange = blip.state.shortRange,
        .scale = blip.state.scale,
        .name = blip.state.name,
        .dimension = blip.dimension,
    };
}

/// Она же, какой её понимает протокол.
///
/// Номер сюда не переносится нарочно: назначает его сервер, и позволить скрипту
/// его подставить значило бы разрешить метке стать другой меткой.
[[nodiscard]] shared::BlipState describe(const script::BlipInfo& blip) {
    return shared::BlipState{
        .position = blip.position,
        .sprite = blip.sprite,
        .colour = blip.colour,
        .alpha = blip.alpha,
        .display = blip.display,
        .shortRange = blip.shortRange,
        .scale = blip.scale,
        .name = blip.name,
    };
}

/// Маркер, каким его знает скрипт.
[[nodiscard]] script::MarkerInfo describe(const MarkerDirectory::Entry& marker) {
    return script::MarkerInfo{
        .id = marker.state.id,
        .type = marker.state.type,
        .position = marker.state.position,
        .rotation = marker.state.rotation,
        .direction = marker.state.direction,
        .scale = marker.state.scale,
        .red = marker.state.red,
        .green = marker.state.green,
        .blue = marker.state.blue,
        .alpha = marker.state.alpha,
        .visible = marker.state.visible,
        .bobUpAndDown = marker.state.bobUpAndDown,
        .faceCamera = marker.state.faceCamera,
        .rotate = marker.state.rotate,
        .streamingDistance = marker.state.streamingDistance,
        .dimension = marker.dimension,
    };
}

/// Он же, каким его понимает протокол. Номер не переносится — назначает его
/// сервер, как и метке.
[[nodiscard]] shared::MarkerState describe(const script::MarkerInfo& marker) {
    return shared::MarkerState{
        .type = marker.type,
        .position = marker.position,
        .rotation = marker.rotation,
        .direction = marker.direction,
        .scale = marker.scale,
        .red = marker.red,
        .green = marker.green,
        .blue = marker.blue,
        .alpha = marker.alpha,
        .visible = marker.visible,
        .bobUpAndDown = marker.bobUpAndDown,
        .faceCamera = marker.faceCamera,
        .rotate = marker.rotate,
        .streamingDistance = marker.streamingDistance,
    };
}

/// Контрольная точка, какой её знает скрипт.
[[nodiscard]] script::CheckpointInfo describe(const CheckpointDirectory::Entry& checkpoint) {
    return script::CheckpointInfo{
        .id = checkpoint.state.id,
        .type = checkpoint.state.type,
        .position = checkpoint.state.position,
        .nextPosition = checkpoint.state.nextPosition,
        .radius = checkpoint.state.radius,
        .height = checkpoint.state.height,
        .red = checkpoint.state.red,
        .green = checkpoint.state.green,
        .blue = checkpoint.state.blue,
        .alpha = checkpoint.state.alpha,
        .iconRed = checkpoint.state.iconRed,
        .iconGreen = checkpoint.state.iconGreen,
        .iconBlue = checkpoint.state.iconBlue,
        .iconAlpha = checkpoint.state.iconAlpha,
        .visible = checkpoint.state.visible,
        .streamingDistance = checkpoint.state.streamingDistance,
        .dimension = checkpoint.dimension,
    };
}

/// Она же, какой её понимает протокол.
[[nodiscard]] shared::CheckpointState describe(const script::CheckpointInfo& checkpoint) {
    return shared::CheckpointState{
        .type = checkpoint.type,
        .position = checkpoint.position,
        .nextPosition = checkpoint.nextPosition,
        .radius = checkpoint.radius,
        .height = checkpoint.height,
        .red = checkpoint.red,
        .green = checkpoint.green,
        .blue = checkpoint.blue,
        .alpha = checkpoint.alpha,
        .iconRed = checkpoint.iconRed,
        .iconGreen = checkpoint.iconGreen,
        .iconBlue = checkpoint.iconBlue,
        .iconAlpha = checkpoint.iconAlpha,
        .visible = checkpoint.visible,
        .streamingDistance = checkpoint.streamingDistance,
    };
}

} // namespace

ServerCore::ServerCore(PlayerRegistry& players, VehicleDirectory& vehicles,
                       ObjectDirectory& objects, BlipDirectory& blips, MarkerDirectory& markers,
                       CheckpointDirectory& checkpoints, WorldClock& world, const Config& config,
                       script::Events& events, CoreSink& sink) noexcept
    : players_(&players), vehicles_(&vehicles), objects_(&objects), blips_(&blips),
      markers_(&markers), checkpoints_(&checkpoints), world_(&world), config_(&config),
      events_(&events), sink_(&sink) {}

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

bool ServerCore::setIntoVehicle(shared::PlayerId id, shared::VehicleId vehicle,
                                std::int8_t seat) {
    const Player* const player = players_->findById(id);
    if (player == nullptr || vehicles_->find(vehicle) == nullptr) {
        return false;
    }

    // Место в реестре здесь не проставляется, и это не забывчивость. Кто где
    // сидит, сервер узнаёт из снимков: пока игра на той стороне не посадила
    // персонажа, он не сидит нигде, и записать обратное значило бы разойтись с
    // правдой до первого же снимка — а по ней сервер решает, кому вести машину.
    sink_->seated(*player, vehicle, seat);
    return true;
}

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

bool ServerCore::playAnimation(shared::PlayerId id, const script::AnimationInfo& animation) {
    const Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::PlayerAnimation message;
    message.playerId = id;
    message.dictionary = animation.dictionary;
    message.name = animation.name;
    message.blendIn = animation.blendIn;
    message.blendOut = animation.blendOut;
    message.duration = animation.duration;
    message.flags = animation.flags;
    message.playbackRate = animation.playbackRate;

    message.locks |= animation.lockX ? static_cast<std::uint8_t>(shared::AnimationLock::X) : 0;
    message.locks |= animation.lockY ? static_cast<std::uint8_t>(shared::AnimationLock::Y) : 0;
    message.locks |= animation.lockZ ? static_cast<std::uint8_t>(shared::AnimationLock::Z) : 0;

    sink_->animationPlayed(*player, message);
    return true;
}

bool ServerCore::clearTasks(shared::PlayerId id) {
    const Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Пустой набор означает «снять задачи»: по сети это то же самое
    // распоряжение — «перестань делать то, что делаешь».
    shared::PlayerAnimation message;
    message.playerId = id;

    sink_->animationPlayed(*player, message);
    return true;
}

bool ServerCore::setDimension(shared::PlayerId id, std::int32_t dimension) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    const std::int32_t previous = player->dimension;
    if (previous == dimension) {
        return true;
    }

    player->dimension = dimension;

    // Про сами измерения клиенту по-прежнему не рассказывают: он о них не знает
    // вовсе, и это решение (см. docs/altv-next.md). Но нарисованное — метки,
    // маркеры, точки — уходит к нему один раз, при входе, и само собой не
    // разберётся: без этого игрок унёс бы карту прежнего слоя с собой.
    sink_->dimensionChanged(*player, previous);
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

bool ServerCore::teleportVehicle(shared::VehicleId id, const shared::Vec3& position,
                                 float heading) {
    const VehicleDirectory::Vehicle* const vehicle = vehicles_->find(id);
    if (vehicle == nullptr) {
        return false;
    }

    // У себя — всегда: этим состоянием сервер отвечает вошедшим и оживляет
    // машину без ведущего. Не поставь мы его, вошедший увидел бы машину на
    // прежнем месте до первого снимка.
    const bool led = vehicle->owner != shared::kInvalidPlayerId;

    (void)vehicles_->place(id, position, heading);

    // Ведущему — просьбой. Машина живёт в игре у него, и переставить её может
    // только она; поставленное у себя он вернул бы обратно ближайшим снимком.
    if (led) {
        sink_->vehicleTeleported(id, position, heading);
    }

    return true;
}

bool ServerCore::repairVehicle(shared::VehicleId id) {
    const VehicleDirectory::Vehicle* const vehicle = vehicles_->find(id);
    if (vehicle == nullptr) {
        return false;
    }

    const bool led = vehicle->owner != shared::kInvalidPlayerId;

    (void)vehicles_->repair(id);

    if (led) {
        sink_->vehicleRepaired(id);
    }

    return true;
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

std::vector<script::BlipInfo> ServerCore::blips() const {
    std::vector<script::BlipInfo> everything;
    everything.reserve(blips_->size());

    for (const auto& [id, blip] : blips_->all()) {
        everything.push_back(describe(blip));
    }

    return everything;
}

std::optional<script::BlipInfo> ServerCore::blip(shared::BlipId id) const {
    const BlipDirectory::Entry* const found = blips_->find(id);
    return found == nullptr ? std::nullopt : std::optional{describe(*found)};
}

shared::BlipId ServerCore::createBlip(const script::BlipInfo& blip) {
    const shared::BlipId id = blips_->add(describe(blip), config_->maxBlips);

    if (id == shared::kInvalidBlipId) {
        return shared::kInvalidBlipId;
    }

    (void)blips_->setDimension(id, blip.dimension);

    sink_->blipChanged(id);
    return id;
}

bool ServerCore::updateBlip(shared::BlipId id, const script::BlipInfo& blip) {
    if (!blips_->update(id, describe(blip))) {
        return false;
    }

    (void)blips_->setDimension(id, blip.dimension);

    sink_->blipChanged(id);
    return true;
}

bool ServerCore::removeBlip(shared::BlipId id) {
    if (!blips_->remove(id)) {
        return false;
    }

    sink_->blipRemoved(id);
    return true;
}

std::vector<script::MarkerInfo> ServerCore::markers() const {
    std::vector<script::MarkerInfo> everything;
    everything.reserve(markers_->size());

    for (const auto& [id, marker] : markers_->all()) {
        everything.push_back(describe(marker));
    }

    return everything;
}

std::optional<script::MarkerInfo> ServerCore::marker(shared::MarkerId id) const {
    const MarkerDirectory::Entry* const found = markers_->find(id);
    return found == nullptr ? std::nullopt : std::optional{describe(*found)};
}

shared::MarkerId ServerCore::createMarker(const script::MarkerInfo& marker) {
    const shared::MarkerId id = markers_->add(describe(marker), config_->maxMarkers);

    if (id == shared::kInvalidMarkerId) {
        return shared::kInvalidMarkerId;
    }

    (void)markers_->setDimension(id, marker.dimension);

    sink_->markerChanged(id);
    return id;
}

bool ServerCore::updateMarker(shared::MarkerId id, const script::MarkerInfo& marker) {
    if (!markers_->update(id, describe(marker))) {
        return false;
    }

    (void)markers_->setDimension(id, marker.dimension);

    sink_->markerChanged(id);
    return true;
}

bool ServerCore::removeMarker(shared::MarkerId id) {
    if (!markers_->remove(id)) {
        return false;
    }

    sink_->markerRemoved(id);
    return true;
}

std::vector<script::CheckpointInfo> ServerCore::checkpoints() const {
    std::vector<script::CheckpointInfo> everything;
    everything.reserve(checkpoints_->size());

    for (const auto& [id, checkpoint] : checkpoints_->all()) {
        everything.push_back(describe(checkpoint));
    }

    return everything;
}

std::optional<script::CheckpointInfo> ServerCore::checkpoint(shared::CheckpointId id) const {
    const CheckpointDirectory::Entry* const found = checkpoints_->find(id);
    return found == nullptr ? std::nullopt : std::optional{describe(*found)};
}

shared::CheckpointId ServerCore::createCheckpoint(const script::CheckpointInfo& checkpoint) {
    const shared::CheckpointId id =
        checkpoints_->add(describe(checkpoint), config_->maxCheckpoints);

    if (id == shared::kInvalidCheckpointId) {
        return shared::kInvalidCheckpointId;
    }

    (void)checkpoints_->setDimension(id, checkpoint.dimension);

    sink_->checkpointChanged(id);
    return id;
}

bool ServerCore::updateCheckpoint(shared::CheckpointId id,
                                  const script::CheckpointInfo& checkpoint) {
    if (!checkpoints_->update(id, describe(checkpoint))) {
        return false;
    }

    (void)checkpoints_->setDimension(id, checkpoint.dimension);

    sink_->checkpointChanged(id);
    return true;
}

bool ServerCore::removeCheckpoint(shared::CheckpointId id) {
    if (!checkpoints_->remove(id)) {
        return false;
    }

    sink_->checkpointRemoved(id);
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
