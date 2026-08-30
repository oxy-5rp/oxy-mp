#include "server_core.hpp"

#include <algorithm>
#include <string>

namespace oxymp::server {
namespace {

/// Что о нём знает скрипт.
[[nodiscard]] script::PlayerInfo describe(const Player& player, const VehicleDirectory& vehicles,
                                          const Config& config, const CoreSink& sink) {
    const VehicleDirectory::Seat seat = vehicles.seatOf(player.id);

    return script::PlayerInfo{
        .id = player.id,
        .nickname = player.nickname,
        .hwidHash = player.hwidHash,
        .socialId = player.socialId,
        .socialName = player.socialName,
        .position = player.position,
        .heading = player.heading,
        .velocity = player.state.velocity,
        .health = player.health,
        .armour = player.armour,
        .maxArmour = player.maxArmour,
        .model = player.appearance ? player.appearance->model : 0,
        .vehicle = seat.vehicle,
        .seat = seat.index,

        // Признаки, прицел и оружие в руках берутся из последнего снимка, а не
        // ведутся отдельно. Снимок и есть то, что игрок о себе рассказал: вести
        // рядом с ним вторую запись значило бы завести второй источник правды,
        // который разойдётся с первым на первом же пропущенном пакете.
        .flags = player.state.flags,
        .aimAt = player.state.aimAt,

        // Распоряжение о теле помнит сервер, а не снимок: клиент о нём не
        // рассказывает — он его исполняет.
        .control = player.control,
        .weapon = player.state.weapon,
        .ammo = player.state.ammo,

        // Движение помнит сервер: он его и велел. Снимок говорит лишь, идёт ли
        // оно ещё, — по этому признаку сервер и стирает свою память.
        .animationDictionary = player.animationDictionary,
        .animationName = player.animationName,

        .dimension = player.dimension,

        // Адрес и задержка спрашиваются у рассылки: знает их транспорт, а до
        // него дотягивается только сервер. Спрашиваются на каждый снимок, а не
        // запоминаются: задержка меняется каждую секунду, а запомненная врала
        // бы тем убедительнее, чем дольше её не трогали.
        .ip = sink.addressOf(player),
        .ping = sink.latencyOf(player),

        // Право живёт в настройках сервера, а не у скрипта: назначает
        // распорядителей хозяин сессии, и подменять его решение ресурсу
        // непозволительно. Видеть же ответ ресурс обязан — иначе ему нельзя
        // доверить ничего, что меняет мир.
        .admin = std::ranges::find(config.admins, player.id) != config.admins.end(),
    };
}

[[nodiscard]] script::VehicleInfo describe(shared::VehicleId id,
                                           const VehicleDirectory::Vehicle& vehicle,
                                           const VehicleDirectory& vehicles) {
    return script::VehicleInfo{
        .id = id,
        .model = vehicle.state.model,
        .position = vehicle.state.position,
        .rotation = vehicle.state.rotation,
        .owner = vehicle.owner,

        // Двери, скорость, прочность и признаки берутся из последнего снимка.
        // Ехали они в нём с самого начала — недоставало дороги наружу, ровно
        // как у признаков состояния игрока.
        .doorLevels = vehicle.state.doorLevels,
        .steer = vehicle.state.steer,
        .velocity = vehicle.state.velocity,
        .bodyHealth = vehicle.state.bodyHealth,
        .engineHealth = vehicle.state.engineHealth,
        .tankHealth = vehicle.state.tankHealth,
        .flags = vehicle.state.flags,

        // Замки помнит сервер, а не снимок: ведущий о них не рассказывает — он
        // их исполняет, как и всякий, кто машину видит.
        .lockState = vehicle.lockState,
        .windowsOpen = vehicle.windowsOpen,
        .roofState = vehicle.state.roofState,
        .passengers = vehicles.seatedIn(id),

        .dimension = vehicle.dimension,
    };
}

/// Внешность машины, какой её знает скрипт.
[[nodiscard]] script::VehicleAppearanceInfo describe(const shared::VehicleAppearance& look) {
    return script::VehicleAppearanceInfo{
        .primaryColour = look.primaryColour,
        .secondaryColour = look.secondaryColour,
        .pearlescentColour = look.pearlescentColour,
        .wheelColour = look.wheelColour,
        .customPrimary = look.customPrimary,
        .customPrimaryRed = look.customPrimaryRed,
        .customPrimaryGreen = look.customPrimaryGreen,
        .customPrimaryBlue = look.customPrimaryBlue,
        .customSecondary = look.customSecondary,
        .customSecondaryRed = look.customSecondaryRed,
        .customSecondaryGreen = look.customSecondaryGreen,
        .customSecondaryBlue = look.customSecondaryBlue,
        .plate = look.plate,
        .plateStyle = look.plateStyle,
        .livery = look.livery,
        .wheelType = look.wheelType,
        .windowTint = look.windowTint,
        .dirtLevel = look.dirtLevel,
        .mods = look.mods,
        .toggleMods = look.toggleMods,
        .customTyres = look.customTyres,
        .tyreSmokeRed = look.tyreSmokeRed,
        .tyreSmokeGreen = look.tyreSmokeGreen,
        .tyreSmokeBlue = look.tyreSmokeBlue,
        .neonSides = look.neonSides,
        .neonRed = look.neonRed,
        .neonGreen = look.neonGreen,
        .neonBlue = look.neonBlue,
        .extras = look.extras,
    };
}

/// Она же, какой её понимает протокол.
///
/// Номер сюда не переносится, потому что его в описании и нет: машину называет
/// довод. См. script::VehicleAppearanceInfo.
[[nodiscard]] shared::VehicleAppearance describe(const script::VehicleAppearanceInfo& look) {
    shared::VehicleAppearance appearance;

    appearance.primaryColour = look.primaryColour;
    appearance.secondaryColour = look.secondaryColour;
    appearance.pearlescentColour = look.pearlescentColour;
    appearance.wheelColour = look.wheelColour;
    appearance.plate = look.plate;
    appearance.plateStyle = look.plateStyle;
    appearance.livery = look.livery;
    appearance.wheelType = look.wheelType;
    appearance.windowTint = look.windowTint;
    appearance.dirtLevel = look.dirtLevel;
    appearance.mods = look.mods;
    appearance.toggleMods = look.toggleMods;
    appearance.customTyres = look.customTyres;
    appearance.tyreSmokeRed = look.tyreSmokeRed;
    appearance.tyreSmokeGreen = look.tyreSmokeGreen;
    appearance.tyreSmokeBlue = look.tyreSmokeBlue;
    appearance.neonSides = look.neonSides;
    appearance.neonRed = look.neonRed;
    appearance.neonGreen = look.neonGreen;
    appearance.neonBlue = look.neonBlue;
    appearance.extras = look.extras;

    appearance.customPrimary = look.customPrimary;
    appearance.customPrimaryRed = look.customPrimaryRed;
    appearance.customPrimaryGreen = look.customPrimaryGreen;
    appearance.customPrimaryBlue = look.customPrimaryBlue;

    appearance.customSecondary = look.customSecondary;
    appearance.customSecondaryRed = look.customSecondaryRed;
    appearance.customSecondaryGreen = look.customSecondaryGreen;
    appearance.customSecondaryBlue = look.customSecondaryBlue;

    // Номер обрезается здесь, хотя его обрежет и читающий: длинный, он остался
    // бы у сервера длинным, а у всех остальных — коротким, и скрипт, спросивший
    // внешность обратно, получил бы не то, что видят игроки.
    if (appearance.plate.size() > shared::kMaxPlateLength) {
        appearance.plate.resize(shared::kMaxPlateLength);
    }

    return appearance;
}

/// Прохожий, каким его знает скрипт.
[[nodiscard]] script::PedInfo describe(const PedDirectory::Ped& ped) {
    return script::PedInfo{
        .id = ped.state.id,
        .model = ped.state.model,
        .position = ped.state.position,
        .rotation = ped.state.rotation,
        .health = ped.state.health,
        .maxHealth = ped.state.maxHealth,
        .armour = ped.state.armour,
        .weapon = ped.state.weapon,
        .dimension = ped.dimension,
    };
}

/// Он же, каким его понимает протокол.
///
/// Номер сюда не переносится нарочно: назначает его сервер, и позволить скрипту
/// его подставить значило бы разрешить кукле стать другой куклой.
[[nodiscard]] shared::PedState describe(const script::PedInfo& ped) {
    return shared::PedState{
        .model = ped.model,
        .position = ped.position,
        .rotation = ped.rotation,
        .health = ped.health,
        .maxHealth = ped.maxHealth,
        .armour = ped.armour,
        .weapon = ped.weapon,
    };
}

/// Привязка, какой её знает скрипт.
[[nodiscard]] script::AttachmentInfo describe(const shared::EntityAttachment& attachment) {
    return script::AttachmentInfo{
        .target = script::EntityRef{.kind = attachment.targetKind, .id = attachment.target},
        .bone = attachment.bone,
        .boneName = attachment.boneName,
        .position = attachment.position,
        .rotation = attachment.rotation,
        .collision = attachment.collision,
        .fixedRotation = attachment.fixedRotation,
    };
}

/// Она же, какой её понимает протокол. Кого привязывают — довод, а не описание.
[[nodiscard]] shared::EntityAttachment describe(script::EntityRef entity,
                                                const script::AttachmentInfo& attachment) {
    return shared::EntityAttachment{
        .kind = entity.kind,
        .id = entity.id,
        .targetKind = attachment.target.kind,
        .target = attachment.target.id,
        .bone = attachment.bone,
        .boneName = attachment.boneName,
        .position = attachment.position,
        .rotation = attachment.rotation,
        .collision = attachment.collision,
        .fixedRotation = attachment.fixedRotation,
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
        .priority = blip.state.priority,
        .name = blip.state.name,
        .flags = blip.state.flags,
        .flashInterval = blip.state.flashInterval,
        .flashTimer = blip.state.flashTimer,
        .number = blip.state.number,
        .hasSecondaryColour = blip.state.hasSecondaryColour,
        .secondaryRed = blip.state.secondaryRed,
        .secondaryGreen = blip.state.secondaryGreen,
        .secondaryBlue = blip.state.secondaryBlue,
        .gxtName = blip.state.gxtName,
        .dimension = blip.dimension,
        .global = blip.global,
        .targets = blip.targets,
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
        .priority = blip.priority,
        .name = blip.name,
        .flags = blip.flags,
        .flashInterval = blip.flashInterval,
        .flashTimer = blip.flashTimer,
        .number = blip.number,
        .hasSecondaryColour = blip.hasSecondaryColour,
        .secondaryRed = blip.secondaryRed,
        .secondaryGreen = blip.secondaryGreen,
        .secondaryBlue = blip.secondaryBlue,
        .gxtName = blip.gxtName,
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
                       ObjectDirectory& objects, PedDirectory& peds, BlipDirectory& blips,
                       MarkerDirectory& markers, CheckpointDirectory& checkpoints,
                       AttachmentDirectory& attachments, WorldClock& world, const Config& config,
                       const GameData& models, script::Events& events, CoreSink& sink) noexcept
    : players_(&players), vehicles_(&vehicles), objects_(&objects), peds_(&peds), blips_(&blips),
      markers_(&markers), checkpoints_(&checkpoints), attachments_(&attachments), world_(&world),
      config_(&config), models_(&models), events_(&events), sink_(&sink) {}

bool ServerCore::knowsModels() const {
    return models_ != nullptr && models_->loaded();
}

const script::VehicleModelInfo* ServerCore::vehicleModel(std::uint32_t hash) const {
    return models_ == nullptr ? nullptr : models_->vehicle(hash);
}

const script::PedModelInfo* ServerCore::pedModel(std::uint32_t hash) const {
    return models_ == nullptr ? nullptr : models_->ped(hash);
}

const script::WeaponModelInfo* ServerCore::weaponModel(std::uint32_t hash) const {
    return models_ == nullptr ? nullptr : models_->weapon(hash);
}

std::int32_t ServerCore::vehicleModsCount(shared::VehicleId id, std::uint8_t slot) const {
    if (models_ == nullptr) {
        return -1;
    }

    // Модель спрашивается у реестра, а не у скрипта: тот назвал номер машины в
    // сессии, а справочник знает модели. Ушедшей машины в реестре уже нет, и
    // ответ на неё — «не знаю», а не ноль: ноль означал бы пустое меню тюнинга
    // у машины, которой не существует.
    const VehicleDirectory::Vehicle* const one = vehicles_->find(id);

    if (one == nullptr) {
        return -1;
    }

    return models_->modsCount(one->state.model, slot);
}

std::vector<script::PlayerInfo> ServerCore::players() const {
    std::vector<script::PlayerInfo> everyone;
    everyone.reserve(players_->size());

    for (const auto& [peer, player] : *players_) {
        everyone.push_back(describe(player, *vehicles_, *config_, *sink_));
    }

    return everyone;
}

std::optional<script::PlayerInfo> ServerCore::player(shared::PlayerId id) const {
    const Player* found = players_->findById(id);

    return found == nullptr ? std::nullopt
                            : std::optional{describe(*found, *vehicles_, *config_, *sink_)};
}

bool ServerCore::setMaxArmour(shared::PlayerId id, std::uint16_t maxArmour) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    player->maxArmour = maxArmour;

    // Уже надетая броня обрезается по новому пределу: опустивший его увидел бы
    // иначе игрока в броне выше собственного предела, и снять её было бы нечем.
    player->armour = std::min(player->armour, maxArmour);

    sink_->healthChanged(*player);
    return true;
}

bool ServerCore::setHealth(shared::PlayerId id, std::uint16_t health, std::uint16_t armour) {
    Player* player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    const bool wasAlive = player->health != 0;
    const std::uint16_t healthWas = player->health;
    const std::uint16_t armourWas = player->armour;

    // Обрезается, а не принимается как есть. Скрипт вправе ошибиться в числе, и
    // ошибка эта тихая: игра больше двухсот единиц не показывает, и человек с
    // шестьюдесятью тысячами выглядит здоровым ровно так же, как целый, — просто
    // не умирает.
    player->health = std::min(health, kFullHealth);
    player->armour = std::min(armour, player->maxArmour);

    sink_->healthChanged(*player);

    // Лечение объявляется отдельным событием, как у alt:V. Не всякая перемена
    // здоровья — лечение: убыль это урон, и о нём говорит своё событие там, где
    // известен ударивший. Здесь же известно только, что стало больше.
    if (player->health > healthWas || player->armour > armourWas) {
        script::Event healed;
        healed.kind = script::EventKind::PlayerHeal;
        healed.player = script::Player{*this, id};
        healed.healthHarm = healthWas;
        healed.armourHarm = armourWas;
        healed.health = player->health;
        healed.armour = player->armour;

        events_->dispatch(healed);
    }

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

std::vector<shared::WeaponSlot> ServerCore::loadout(shared::PlayerId id) const {
    const Player* const player = players_->findById(id);
    return player == nullptr ? std::vector<shared::WeaponSlot>{} : player->loadout;
}

bool ServerCore::removeWeapon(shared::PlayerId id, std::uint32_t weapon) {
    if (weapon == 0) {
        return false;
    }

    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    if (std::erase_if(player->loadout, [weapon](const shared::WeaponSlot& slot) {
            return slot.weapon == weapon;
        }) == 0) {
        return false;
    }

    // Насадки и расцветка уходят вместе со стволом сами: лежат они внутри
    // самого слота (`WeaponSlot::components`), а не отдельным списком. Отдельный
    // пришлось бы чистить вручную, и забытая насадка досталась бы следующему
    // такому же стволу — игрок, которому вернули пистолет, получил бы его с
    // прежним глушителем, о котором никто не просил.

    // Снаряжение уходит игроку заменой, а не добавкой: иначе игра оставила бы у
    // него отобранный ствол — она о нём не забывала.
    sink_->loadoutChanged(*player, true, 0);
    return true;
}

bool ServerCore::giveWeapon(shared::PlayerId id, std::uint32_t weapon, std::uint16_t ammo,
                            bool equip) {
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

    sink_->loadoutChanged(*player, false, equip ? weapon : 0);
    return true;
}

namespace {

/// Ствол в снаряжении игрока. Пусто — такого у него нет.
[[nodiscard]] shared::WeaponSlot* weaponOf(Player& player, std::uint32_t weapon) {
    const auto found = std::ranges::find(player.loadout, weapon, &shared::WeaponSlot::weapon);
    return found == player.loadout.end() ? nullptr : &*found;
}

} // namespace

bool ServerCore::setWeaponAmmo(shared::PlayerId id, std::uint32_t weapon, std::uint16_t ammo) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::WeaponSlot* const slot = weaponOf(*player, weapon);
    if (slot == nullptr) {
        return false;
    }

    slot->ammo = ammo;

    // Без замены: список тот же, поменялось одно число. Замена отобрала бы у
    // игрока оружие из рук на мгновение — игра выдаёт его заново, и держащий
    // ствол опустил бы руки посреди перестрелки.
    sink_->loadoutChanged(*player, false, 0);
    return true;
}

bool ServerCore::addWeaponComponent(shared::PlayerId id, std::uint32_t weapon,
                                    std::uint32_t component) {
    if (weapon == 0 || component == 0) {
        return false;
    }

    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Ствол обязан быть у игрока. Насадка без ствола — насадка ни на чём, и
    // запомнить её молча значило бы пообещать то, чего не будет: выдай сервер
    // этот ствол потом, насадку он к нему уже не привяжет.
    shared::WeaponSlot* const slot = weaponOf(*player, weapon);
    if (slot == nullptr) {
        return false;
    }

    if (std::ranges::find(slot->components, component) != slot->components.end()) {
        return true;
    }

    if (slot->components.size() >= shared::kMaxWeaponComponents) {
        return false;
    }

    slot->components.push_back(component);

    // Снаряжение уходит целиком и без отбора имеющегося: список полный, и
    // отбирать нечего — то же оружие вернётся с той же насадкой.
    sink_->loadoutChanged(*player, false, 0);
    return true;
}

bool ServerCore::removeWeaponComponent(shared::PlayerId id, std::uint32_t weapon,
                                       std::uint32_t component) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::WeaponSlot* const slot = weaponOf(*player, weapon);
    if (slot == nullptr) {
        return false;
    }

    if (std::erase(slot->components, component) == 0) {
        return false;
    }

    // Снятая насадка требует отбора: игра не снимает поставленное сама, и
    // список без насадки для неё выглядит просто как список без насадки.
    // Поэтому оружие выдаётся заново, поверх отобранного.
    sink_->loadoutChanged(*player, true, 0);
    return true;
}

bool ServerCore::setWeaponTint(shared::PlayerId id, std::uint32_t weapon, std::uint8_t tint) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::WeaponSlot* const slot = weaponOf(*player, weapon);
    if (slot == nullptr) {
        return false;
    }

    slot->tint = tint;

    sink_->loadoutChanged(*player, false, 0);
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
    sink_->loadoutChanged(*player, true, 0);
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

bool ServerCore::setClothes(shared::PlayerId id, std::uint8_t component, std::uint16_t drawable,
                            std::uint16_t texture, std::uint8_t palette) {
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

bool ServerCore::setProp(shared::PlayerId id, std::uint8_t index, std::int16_t drawable,
                         std::int16_t texture) {
    Player* const player = players_->findById(id);
    if (player == nullptr || index >= shared::kPedPropCount) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);

    look.props[index] = shared::PedProp{.drawable = drawable, .texture = texture};

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setFrozen(shared::PlayerId id, bool frozen) {
    return setControl(id, shared::PlayerControlFlag::Frozen, frozen);
}

bool ServerCore::setInvincible(shared::PlayerId id, bool invincible) {
    return setControl(id, shared::PlayerControlFlag::Invincible, invincible);
}

bool ServerCore::setControl(shared::PlayerId id, shared::PlayerControlFlag flag, bool on) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    const auto bit = static_cast<std::uint8_t>(flag);
    const std::uint8_t wanted = on ? static_cast<std::uint8_t>(player->control | bit)
                                   : static_cast<std::uint8_t>(player->control & ~bit);

    // Ничего не изменилось — ничего и не рассылаем. Признаки эти ставят из
    // обработчиков, которые идут каждый такт, и слать одно и то же тридцать раз
    // в секунду значило бы платить за ничто.
    if (wanted == player->control) {
        return true;
    }

    player->control = wanted;
    sink_->controlChanged(*player);
    return true;
}

bool ServerCore::setHeadBlend(shared::PlayerId id, std::uint8_t shapeFirst,
                              std::uint8_t shapeSecond, std::uint8_t shapeThird,
                              std::uint8_t skinFirst, std::uint8_t skinSecond,
                              std::uint8_t skinThird, float shapeMix, float skinMix,
                              float thirdMix) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Доли обрезаются здесь, а не у клиента. Обрежет их всё равно игра, но
    // помнит внешность сервер и пересказывает её вошедшим позже: сохрани он
    // число, которого игра не приняла, — вошедшие увидели бы одно лицо, а
    // хозяин у себя другое.
    const auto clamp = [](float value) {
        return value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
    };

    shared::PlayerAppearance& look = appearanceOf(*player);

    look.shapeFirst = shapeFirst;
    look.shapeSecond = shapeSecond;
    look.shapeThird = shapeThird;
    look.skinFirst = skinFirst;
    look.skinSecond = skinSecond;
    look.skinThird = skinThird;
    look.shapeMix = clamp(shapeMix);
    look.skinMix = clamp(skinMix);
    look.thirdMix = clamp(thirdMix);

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setHeadOverlay(shared::PlayerId id, std::uint8_t slot, std::uint8_t index,
                                float opacity) {
    Player* const player = players_->findById(id);
    if (player == nullptr || slot >= shared::kPedOverlayCount) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);

    // Цвет слоя здесь не трогается: ставят его отдельно, и обнулять его при
    // всякой смене самого слоя значило бы стирать сделанное соседним вызовом.
    look.overlays[slot].index = index;
    look.overlays[slot].opacity = opacity < 0.0F ? 0.0F : (opacity > 1.0F ? 1.0F : opacity);

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setHeadOverlayColour(shared::PlayerId id, std::uint8_t slot,
                                      std::uint8_t colourType, std::uint8_t colour,
                                      std::uint8_t secondColour) {
    Player* const player = players_->findById(id);
    if (player == nullptr || slot >= shared::kPedOverlayCount) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);

    look.overlays[slot].colourType = colourType;
    look.overlays[slot].colour = colour;
    look.overlays[slot].secondColour = secondColour;

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setHairColour(shared::PlayerId id, std::uint8_t colour, std::uint8_t highlight) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);

    look.hairColour = colour;
    look.hairHighlight = highlight;

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setEyeColour(shared::PlayerId id, std::uint8_t colour) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    appearanceOf(*player).eyeColour = colour;

    sink_->appearanceChanged(*player);
    return true;
}

std::optional<shared::PlayerAppearance> ServerCore::appearance(shared::PlayerId id) const {
    const Player* const player = players_->findById(id);
    if (player == nullptr) {
        return std::nullopt;
    }

    // Пусто, пока игрок не объявил внешности сам и скрипт её не назначил.
    // Выдумывать её сервер не вправе: отданная по умолчанию, она разошлась бы с
    // тем, что игрок видит у себя, — а спросивший принял бы её за правду.
    return player->appearance;
}

bool ServerCore::addDecoration(shared::PlayerId id, std::uint32_t collection,
                               std::uint32_t overlay) {
    Player* const player = players_->findById(id);

    // Пустой хеш — не татуировка. Молча приняв его, мы поставили бы персонажу
    // ничто и объявили бы это сделанным.
    if (player == nullptr || collection == 0 || overlay == 0) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);
    const shared::PedDecoration wanted{.collection = collection, .overlay = overlay};

    // Уже стоящая вторично не заводится: игра держит по одной каждого вида.
    if (std::ranges::find(look.decorations, wanted) != look.decorations.end()) {
        return true;
    }

    if (look.decorations.size() >= shared::kMaxPedDecorations) {
        return false;
    }

    look.decorations.push_back(wanted);

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::removeDecoration(shared::PlayerId id, std::uint32_t collection,
                                  std::uint32_t overlay) {
    Player* const player = players_->findById(id);
    if (player == nullptr || !player->appearance) {
        return false;
    }

    const shared::PedDecoration wanted{.collection = collection, .overlay = overlay};

    if (std::erase(player->appearance->decorations, wanted) == 0) {
        return false;
    }

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::clearDecorations(shared::PlayerId id) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::PlayerAppearance& look = appearanceOf(*player);

    if (look.decorations.empty()) {
        return true;
    }

    look.decorations.clear();

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::setFaceFeature(shared::PlayerId id, std::uint8_t index, float scale) {
    Player* const player = players_->findById(id);
    if (player == nullptr || index >= shared::kPedFaceFeatureCount) {
        return false;
    }

    // Обрезается и здесь, а не только у игры: внешность сервер помнит и
    // пересказывает вошедшим позже, и сохранённое число, которого игра не
    // приняла, дало бы им одно лицо, а хозяину другое.
    const float held = scale < -1.0F ? -1.0F : (scale > 1.0F ? 1.0F : scale);

    // В байт — по сто двадцать семь ступеней в каждую сторону, как в протоколе.
    appearanceOf(*player).faceFeatures[index] =
        static_cast<std::int8_t>(held * 127.0F);

    sink_->appearanceChanged(*player);
    return true;
}

bool ServerCore::playAnimation(shared::PlayerId id, const script::AnimationInfo& animation) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Сценарий у alt:V своим движением не считается: его имя не пара «набор и
    // движение», а одно слово, и в `currentAnimationDict` ему места нет.
    rememberAnimation(*player, animation.scenario.empty() ? animation.dictionary : std::string{},
                      animation.scenario.empty() ? animation.name : std::string{});

    shared::PlayerAnimation message;
    message.playerId = id;
    message.dictionary = animation.dictionary;
    message.name = animation.name;
    message.scenario = animation.scenario;
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

bool ServerCore::playSpeech(shared::PlayerId id, const std::string& speech,
                            const std::string& params, const std::string& voice) {
    const Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::PlayerSpeech message;
    message.playerId = id;
    message.speech = speech;
    message.params = params;
    message.voice = voice;

    sink_->speechPlayed(*player, message);
    return true;
}

void ServerCore::explode(const script::ExplosionInfo& explosion) {
    shared::Explosion message;
    message.position = explosion.position;
    message.kind = explosion.kind;
    message.scale = explosion.scale;
    message.audible = explosion.audible;
    message.invisible = explosion.invisible;
    message.shake = explosion.shake;

    sink_->exploded(message, explosion.dimension);
}

void ServerCore::rememberAnimation(Player& player, std::string dictionary, std::string name) {
    if (player.animationDictionary == dictionary && player.animationName == name) {
        return;
    }

    // Смена объявляется отсюда, а не из разбора снимков, и в этом весь порядок:
    // начало движения известно нам самим — мы его и велели, — а конец известен
    // только хозяину, и приходит он признаком в снимке.
    script::Event event;
    event.kind = script::EventKind::PlayerAnimationChange;
    event.player = script::Player{*this, player.id};
    event.animationDictionaryWas = player.animationDictionary;
    event.animationNameWas = player.animationName;
    event.animationDictionary = dictionary;
    event.animationName = name;

    player.animationDictionary = std::move(dictionary);
    player.animationName = std::move(name);
    player.animationAt = std::chrono::steady_clock::now();
    player.animationSeen = false;

    events_->dispatch(event);
}

bool ServerCore::clearBlood(shared::PlayerId id) {
    const Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    shared::PlayerBodyOrder message;
    message.playerId = id;
    message.order = static_cast<std::uint8_t>(shared::BodyOrder::ClearBlood);

    sink_->bodyOrdered(*player, message);
    return true;
}

bool ServerCore::clearTasks(shared::PlayerId id) {
    Player* const player = players_->findById(id);
    if (player == nullptr) {
        return false;
    }

    // Зачистка снимает и заданное движение — то же самое сделает у себя клиент.
    // Не объяви мы этого здесь, сервер до срока считал бы движение идущим.
    rememberAnimation(*player, {}, {});

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

    script::Event moved;
    moved.kind = script::EventKind::PlayerDimensionChange;
    moved.player = script::Player{*this, id};
    moved.dimensionWas = previous;
    moved.dimension = dimension;

    events_->dispatch(moved);
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
        everything.push_back(describe(id, vehicle, *vehicles_));
    }

    return everything;
}

std::optional<script::VehicleInfo> ServerCore::vehicle(shared::VehicleId id) const {
    const VehicleDirectory::Vehicle* found = vehicles_->find(id);
    return found == nullptr ? std::nullopt : std::optional{describe(id, *found, *vehicles_)};
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

std::optional<script::VehicleAppearanceInfo> ServerCore::vehicleAppearance(
    shared::VehicleId id) const {
    const VehicleDirectory::Vehicle* const vehicle = vehicles_->find(id);
    if (vehicle == nullptr) {
        return std::nullopt;
    }

    // Заводская, если о ней ещё никто не говорил. Пустота здесь означала бы «нет
    // такой машины», а машина есть — просто выглядит она пока никак, и это
    // законный ответ, а не отсутствие ответа.
    return vehicle->appearance ? describe(*vehicle->appearance)
                               : script::VehicleAppearanceInfo{};
}

bool ServerCore::setVehicleAppearance(shared::VehicleId id,
                                      const script::VehicleAppearanceInfo& appearance) {
    if (!vehicles_->setAppearance(id, describe(appearance))) {
        return false;
    }

    sink_->vehicleAppearanceChanged(id);
    return true;
}

bool ServerCore::setVehicleDimension(shared::VehicleId id, std::int32_t dimension) {
    return vehicles_->setDimension(id, dimension);
}

bool ServerCore::setVehicleOwner(shared::VehicleId id, shared::PlayerId owner, bool sticky) {
    // Игрок проверяется здесь, а не в реестре машин: реестру нет дела до списка
    // игроков, и знать о нём он не должен (см. PlayerPlacement).
    if (players_->findById(owner) == nullptr) {
        return false;
    }

    return vehicles_->pinOwner(id, owner, sticky);
}

bool ServerCore::clearVehicleOwner(shared::VehicleId id) {
    return vehicles_->pinOwner(id, shared::kInvalidPlayerId, false);
}

/// Объявляет уход сущности — до того, как её уберут.
///
/// Общее на три рода: у alt:V это одно событие `removeEntity`, а не три, и
/// различает роды оно самой сущностью. Игрока сюда не заводят — он уходит своим
/// событием, и считать его уход дважды незачем.
script::ServerConfigInfo ServerCore::config() const {
    if (config_ == nullptr) {
        return {};
    }

    return script::ServerConfigInfo{
        .name = config_->name,
        .port = config_->port,
        .maxPlayers = config_->maxPlayers,
        // Признак, а не сам пароль: режим, положивший его в свой журнал или
        // отправивший в своё окно, раздал бы его игрокам.
        .passworded = !config_->password.empty(),
        .tickRate = config_->tickRate,
        .streamDistance = config_->streamDistance,
        .verbose = config_->verbose,
        .resources = config_->resources,
    };
}

std::vector<std::pair<shared::EntityKind, std::uint32_t>> ServerCore::streamedTo(
    shared::PlayerId id) const {
    std::vector<std::pair<shared::EntityKind, std::uint32_t>> found;

    const Player* const player = players_->findById(id);

    if (player == nullptr) {
        return found;
    }

    found.reserve(player->streamed.size() + player->streamedObjects.size() +
                  player->streamedPeds.size());

    for (const shared::VehicleId vehicle : player->streamed) {
        found.emplace_back(shared::EntityKind::Vehicle, vehicle);
    }

    for (const shared::ObjectId object : player->streamedObjects) {
        found.emplace_back(shared::EntityKind::Object, object);
    }

    for (const shared::PedId ped : player->streamedPeds) {
        found.emplace_back(shared::EntityKind::Ped, ped);
    }

    return found;
}

bool ServerCore::askResource(std::string_view name, script::ResourceAction action) {
    return sink_->resourceAsked(name, action);
}

void ServerCore::tellEntityGone(shared::EntityKind kind, std::uint32_t id) {
    script::Event gone;
    gone.kind = script::EventKind::RemoveEntity;
    gone.reason = static_cast<std::uint8_t>(kind);
    gone.weapon = id;

    events_->dispatch(gone);
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
    tellEntityGone(shared::EntityKind::Vehicle, id);

    // Обработчик мог убрать её сам — тогда убирать нечего, и это не ошибка.
    if (!vehicles_->remove(id)) {
        return false;
    }

    forgetAttachments(
        AttachmentDirectory::Ref{.kind = shared::EntityKind::Vehicle, .id = id});

    sink_->vehicleRemoved(id);
    return true;
}

// --- Привязка сущностей -----------------------------------------------------

/// Есть ли такая сущность в сессии.
///
/// Проверять приходится здесь: реестр привязок знает только рода и номера, а
/// списки лежат по трём разным местам. Без проверки скрипт повесил бы предмет на
/// машину, которой нет, и предмет этот навсегда остался бы висеть в никуда —
/// отвязать его было бы уже некому.
bool ServerCore::exists(script::EntityRef entity) const {
    switch (entity.kind) {
    case shared::EntityKind::Player:
        return players_->findById(entity.id) != nullptr;
    case shared::EntityKind::Vehicle:
        return vehicles_->find(entity.id) != nullptr;
    case shared::EntityKind::Object:
        return objects_->find(entity.id) != nullptr;
    case shared::EntityKind::Ped:
        return peds_->find(entity.id) != nullptr;
    case shared::EntityKind::None:
        break;
    }

    return false;
}


bool ServerCore::attachEntity(script::EntityRef entity,
                              const script::AttachmentInfo& attachment) {
    if (!exists(entity) || !exists(attachment.target)) {
        return false;
    }

    if (!attachments_->attach(describe(entity, attachment))) {
        return false;
    }

    sink_->attachmentChanged(AttachmentDirectory::Ref{.kind = entity.kind, .id = entity.id});
    return true;
}

bool ServerCore::detachEntity(script::EntityRef entity) {
    const AttachmentDirectory::Ref self{.kind = entity.kind, .id = entity.id};

    if (!attachments_->detach(self)) {
        return false;
    }

    sink_->attachmentChanged(self);
    return true;
}

std::optional<script::AttachmentInfo> ServerCore::attachment(script::EntityRef entity) const {
    const shared::EntityAttachment* const found =
        attachments_->find(AttachmentDirectory::Ref{.kind = entity.kind, .id = entity.id});

    return found == nullptr ? std::nullopt : std::optional{describe(*found)};
}

void ServerCore::forgetAttachments(AttachmentDirectory::Ref entity) {
    for (const AttachmentDirectory::Ref& loosened : attachments_->forget(entity)) {
        sink_->attachmentChanged(loosened);
    }
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

bool ServerCore::setVehicleDoor(shared::VehicleId id, std::uint8_t door, std::uint8_t level) {
    if (door >= shared::kVehicleDoorCount || level > shared::kDoorFullyOpen) {
        return false;
    }

    if (!vehicles_->setDoorLevel(id, door, level)) {
        return false;
    }

    sink_->vehicleDoorsChanged(id);
    return true;
}

bool ServerCore::setVehicleLock(shared::VehicleId id, std::uint8_t lockState) {
    if (!vehicles_->setLockState(id, lockState)) {
        return false;
    }

    sink_->vehicleControlChanged(id);
    return true;
}

bool ServerCore::setVehicleWindows(shared::VehicleId id, std::uint8_t windowsOpen) {
    if (!vehicles_->setWindows(id, windowsOpen)) {
        return false;
    }

    sink_->vehicleControlChanged(id);
    return true;
}

bool ServerCore::moveObject(shared::ObjectId id, const shared::Vec3& position,
                            const shared::Vec3& rotation) {
    if (!objects_->move(id, position, rotation)) {
        return false;
    }

    sink_->objectMoved(id);
    return true;
}

bool ServerCore::setObjectDimension(shared::ObjectId id, std::int32_t dimension) {
    return objects_->setDimension(id, dimension);
}

bool ServerCore::removeObject(shared::ObjectId id) {
    if (objects_->find(id) == nullptr) {
        return false;
    }

    tellEntityGone(shared::EntityKind::Object, id);

    if (!objects_->remove(id)) {
        return false;
    }

    forgetAttachments(AttachmentDirectory::Ref{.kind = shared::EntityKind::Object, .id = id});

    sink_->objectRemoved(id);
    return true;
}

// --- Прохожие ---------------------------------------------------------------

std::vector<script::PedInfo> ServerCore::peds() const {
    std::vector<script::PedInfo> everyone;
    everyone.reserve(peds_->size());

    for (const auto& [id, ped] : peds_->all()) {
        everyone.push_back(describe(ped));
    }

    return everyone;
}

std::optional<script::PedInfo> ServerCore::ped(shared::PedId id) const {
    const PedDirectory::Ped* const found = peds_->find(id);
    return found == nullptr ? std::nullopt : std::optional{describe(*found)};
}

shared::PedId ServerCore::createPed(const script::PedInfo& ped) {
    const shared::PedId id = peds_->add(describe(ped), config_->maxPeds);
    if (id == shared::kInvalidPedId) {
        return shared::kInvalidPedId;
    }

    (void)peds_->setDimension(id, ped.dimension);
    sink_->pedChanged(id);

    return id;
}

bool ServerCore::updatePed(shared::PedId id, const script::PedInfo& ped) {
    const PedDirectory::Ped* const before = peds_->find(id);

    if (before == nullptr) {
        return false;
    }

    const std::uint16_t healthWas = before->state.health;
    const std::uint16_t armourWas = before->state.armour;

    if (!peds_->update(id, describe(ped))) {
        return false;
    }

    (void)peds_->setDimension(id, ped.dimension);
    sink_->pedChanged(id);

    // Смерть и лечение — по тем же правилам, что и у игрока: смерть это
    // обнуление здоровья у живого, лечение — его рост.
    //
    // Ударивший здесь не назван, и это верно: правкой куклы через ядро
    // распоряжается скрипт, а не чужая пуля. Убитый попаданием приходит другим
    // путём — `hurtPed`, — и там ударивший известен.
    const PedDirectory::Ped* const after = peds_->find(id);

    if (after == nullptr) {
        return true;
    }

    if (healthWas != 0 && after->state.health == 0) {
        announcePedDeath(id, shared::kInvalidPlayerId);
        return true;
    }

    if (after->state.health > healthWas || after->state.armour > armourWas) {
        script::Event healed;
        healed.kind = script::EventKind::PedHeal;
        healed.ped = id;
        healed.healthHarm = healthWas;
        healed.armourHarm = armourWas;
        healed.health = after->state.health;
        healed.armour = after->state.armour;

        events_->dispatch(healed);
    }

    return true;
}

void ServerCore::announcePedDeath(shared::PedId id, shared::PlayerId killer) {
    const PedDirectory::Ped* const dead = peds_->find(id);

    script::Event death;
    death.kind = script::EventKind::PedDeath;
    death.ped = id;
    death.weapon = dead != nullptr ? dead->state.weapon : 0U;

    // Ударивший — живой ссылкой, как и везде: он мог выйти между попаданием и
    // обработчиком, и тогда обработчик честно увидит, что его больше нет.
    if (killer != shared::kInvalidPlayerId) {
        death.killer = script::Player{*this, killer};
    }

    events_->dispatch(death);
}

bool ServerCore::hurtPed(shared::PedId id, std::uint16_t health, std::uint16_t armour,
                         shared::PlayerId killer) {
    const PedDirectory::Ped* const before = peds_->find(id);

    if (before == nullptr) {
        return false;
    }

    const std::uint16_t healthWas = before->state.health;

    shared::PedState fresh = before->state;
    fresh.health = health;
    fresh.armour = armour;

    if (!peds_->update(id, fresh)) {
        return false;
    }

    sink_->pedChanged(id);

    // Смерть объявляется здесь же и с ударившим: он известен только на этом
    // пути. Оживление сюда попасть не может — попадание здоровья не прибавляет.
    if (healthWas != 0 && health == 0) {
        announcePedDeath(id, killer);
    }

    return true;
}

bool ServerCore::removePed(shared::PedId id) {
    if (peds_->find(id) == nullptr) {
        return false;
    }

    tellEntityGone(shared::EntityKind::Ped, id);

    if (!peds_->remove(id)) {
        return false;
    }

    forgetAttachments(AttachmentDirectory::Ref{.kind = shared::EntityKind::Ped, .id = id});

    sink_->pedRemoved(id);
    return true;
}

bool ServerCore::setPedDimension(shared::PedId id, std::int32_t dimension) {
    if (!peds_->setDimension(id, dimension)) {
        return false;
    }

    // Сказать нужно и здесь: слой мира решает, кому прохожего видно, а раздача
    // сверяет слои на своём такте. Без повода она сделает это не раньше, чем
    // игрок пройдёт полметра, — и всё это время кукла будет видна не тем.
    sink_->pedChanged(id);
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
    (void)blips_->setTargets(id, blip.global, blip.targets);

    sink_->blipChanged(id);
    return id;
}

bool ServerCore::updateBlip(shared::BlipId id, const script::BlipInfo& blip) {
    if (!blips_->update(id, describe(blip))) {
        return false;
    }

    (void)blips_->setDimension(id, blip.dimension);

    // Список тех, кому метка видна, накладывается тем же путём, что и слой:
    // скрипт правит метку целиком, и не наложи мы его — `addTarget` не сделал
    // бы ничего, а метка осталась бы видна всем.
    (void)blips_->setTargets(id, blip.global, blip.targets);

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
