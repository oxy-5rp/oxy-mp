#pragma once

#include <oxymp/script/core.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>

namespace oxymp::script::testing {

/// Ядро для проверок: обычные списки в памяти.
///
/// Ради него слой и объявлен интерфейсом. Настоящее ядро сидит внутри сервера и
/// тянет за собой сокеты, реестры и поток обслуживания; проверить на нём, что
/// ссылка на вышедшего игрока честно отвечает «его больше нет», означало бы
/// поднять сессию и кого-нибудь из неё выгнать.
///
/// Здесь то же самое делается стиранием записи из вектора.
class FakeCore final : public Core {
public:
    std::vector<PlayerInfo> playerList;
    std::vector<VehicleInfo> vehicleList;
    std::vector<ObjectInfo> objectList;
    std::vector<PedInfo> pedList;
    std::vector<BlipInfo> blipList;
    std::vector<MarkerInfo> markerList;
    std::vector<CheckpointInfo> checkpointList;

    /// Внешности машин. Отдельно от списка машин, как и у настоящего ядра.
    std::unordered_map<shared::VehicleId, VehicleAppearanceInfo> appearances;

    /// Кто к кому привязан. Ключ — род и номер одной строкой.
    std::unordered_map<std::string, AttachmentInfo> attachments;

    /// Что ядру велели сделать. Проверки смотрят сюда вместо сети.
    std::vector<std::string> said;

    /// Внешность лица, каким её назначил скрипт.
    ///
    /// Своими наборами, а не одним снимком: проверяют здесь не то, как игра
    /// нарисует лицо, — игры нет вовсе, — а то, что распоряжение дошло и легло
    /// туда, куда просили. Порядок доводов в этих трёх вызовах перепутать легче
    /// всего, и увидеть это можно только так.
    struct HeadBlend {
        std::uint8_t shapeFirst = 0;
        std::uint8_t shapeSecond = 0;
        std::uint8_t shapeThird = 0;
        std::uint8_t skinFirst = 0;
        std::uint8_t skinSecond = 0;
        std::uint8_t skinThird = 0;
        float shapeMix = 0.0F;
        float skinMix = 0.0F;
        float thirdMix = 0.0F;

        [[nodiscard]] friend bool operator==(const HeadBlend&, const HeadBlend&) = default;
    };

    struct Overlay {
        std::uint8_t slot = 0;
        std::uint8_t index = 0;
        float opacity = 0.0F;

        [[nodiscard]] friend bool operator==(const Overlay&, const Overlay&) = default;
    };

    struct OverlayColour {
        std::uint8_t slot = 0;
        std::uint8_t colourType = 0;
        std::uint8_t colour = 0;
        std::uint8_t secondColour = 0;

        [[nodiscard]] friend bool operator==(const OverlayColour&, const OverlayColour&) = default;
    };

    struct HairColour {
        std::uint8_t colour = 0;
        std::uint8_t highlight = 0;

        [[nodiscard]] friend bool operator==(const HairColour&, const HairColour&) = default;
    };

    std::unordered_map<shared::PlayerId, HeadBlend> headBlend;
    std::unordered_map<shared::PlayerId, std::vector<Overlay>> overlays;
    std::unordered_map<shared::PlayerId, std::vector<OverlayColour>> overlayColours;
    std::unordered_map<shared::PlayerId, HairColour> hairColour;
    std::unordered_map<shared::PlayerId, std::uint8_t> eyeColour;

    /// Черты лица, какими их двигал скрипт: номер и доля.
    std::unordered_map<shared::PlayerId, std::vector<std::pair<std::uint8_t, float>>> features;

    /// Внешность целиком — то, что отдаётся на вопрос о ней.
    std::unordered_map<shared::PlayerId, shared::PlayerAppearance> looks;

    /// Чем игроки вооружены.
    std::unordered_map<shared::PlayerId, std::vector<shared::WeaponSlot>> carried;

    /// Предел брони, поставленный скриптом.
    std::unordered_map<shared::PlayerId, std::uint16_t> armourLimits;

    shared::VehicleId nextVehicleId = 1;
    shared::ObjectId nextObjectId = 1;
    shared::PedId nextPedId = 1;
    shared::BlipId nextBlipId = 1;
    shared::MarkerId nextMarkerId = 1;
    shared::CheckpointId nextCheckpointId = 1;

    /// Род и номер одной строкой — ключ для списка привязок.
    [[nodiscard]] static std::string nameOf(EntityRef entity) {
        return std::format("{}:{}", static_cast<int>(entity.kind), entity.id);
    }

    /// Есть ли такая сущность в подставной сессии.
    [[nodiscard]] bool alive(EntityRef entity) const {
        switch (entity.kind) {
        case shared::EntityKind::Player:
            return player(entity.id).has_value();
        case shared::EntityKind::Vehicle:
            return std::ranges::find(vehicleList, entity.id, &VehicleInfo::id) !=
                   vehicleList.end();
        case shared::EntityKind::Object:
            return std::ranges::find(objectList, entity.id, &ObjectInfo::id) != objectList.end();
        case shared::EntityKind::Ped:
            return std::ranges::find(pedList, entity.id, &PedInfo::id) != pedList.end();
        case shared::EntityKind::None:
            break;
        }

        return false;
    }

    [[nodiscard]] std::vector<PlayerInfo> players() const override { return playerList; }

    [[nodiscard]] std::optional<PlayerInfo> player(shared::PlayerId id) const override {
        const auto it = std::ranges::find(playerList, id, &PlayerInfo::id);
        return it == playerList.end() ? std::nullopt : std::optional{*it};
    }

    bool setFrozen(shared::PlayerId id, bool frozen) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("frozen {} {}", id, frozen ? 1 : 0));
        return true;
    }

    bool setInvincible(shared::PlayerId id, bool invincible) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("invincible {} {}", id, invincible ? 1 : 0));
        return true;
    }

    bool removeWeapon(shared::PlayerId id, std::uint32_t weapon) override {
        if (!player(id) || weapon == 0) {
            return false;
        }

        said.push_back(std::format("weapon- {} {:#x}", id, weapon));
        return std::erase_if(carried[id], [weapon](const shared::WeaponSlot& slot) {
                   return slot.weapon == weapon;
               }) != 0;
    }

    bool setWeaponAmmo(shared::PlayerId id, std::uint32_t weapon,
                       std::uint16_t ammo) override {
        for (shared::WeaponSlot& slot : carried[id]) {
            if (slot.weapon == weapon) {
                slot.ammo = ammo;
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] std::vector<shared::WeaponSlot> loadout(
        shared::PlayerId id) const override {
        const auto found = carried.find(id);
        return found == carried.end() ? std::vector<shared::WeaponSlot>{} : found->second;
    }

    bool addWeaponComponent(shared::PlayerId id, std::uint32_t weapon,
                            std::uint32_t component) override {
        if (!player(id) || weapon == 0 || component == 0) {
            return false;
        }

        said.push_back(std::format("component+ {} {:#x} {:#x}", id, weapon, component));
        return true;
    }

    bool removeWeaponComponent(shared::PlayerId id, std::uint32_t weapon,
                               std::uint32_t component) override {
        if (!player(id) || weapon == 0 || component == 0) {
            return false;
        }

        said.push_back(std::format("component- {} {:#x} {:#x}", id, weapon, component));
        return true;
    }

    bool setWeaponTint(shared::PlayerId id, std::uint32_t weapon, std::uint8_t tint) override {
        if (!player(id) || weapon == 0) {
            return false;
        }

        said.push_back(std::format("tint {} {:#x} {}", id, weapon, tint));
        return true;
    }

    /// Внешность лица. Складывается в снимок, как и одежда: набор здесь
    /// проверяет, что распоряжение дошло и легло куда надо, а не то, как игра
    /// его нарисует.
    bool setHeadBlend(shared::PlayerId id, std::uint8_t shapeFirst, std::uint8_t shapeSecond,
                      std::uint8_t shapeThird, std::uint8_t skinFirst, std::uint8_t skinSecond,
                      std::uint8_t skinThird, float shapeMix, float skinMix,
                      float thirdMix) override {
        headBlend[id] = {shapeFirst, shapeSecond, shapeThird, skinFirst,
                         skinSecond, skinThird, shapeMix,     skinMix,
                         thirdMix};
        return std::ranges::find(playerList, id, &PlayerInfo::id) != playerList.end();
    }

    bool setHeadOverlay(shared::PlayerId id, std::uint8_t slot, std::uint8_t index,
                        float opacity) override {
        overlays[id].push_back({slot, index, opacity});
        return std::ranges::find(playerList, id, &PlayerInfo::id) != playerList.end();
    }

    bool setHeadOverlayColour(shared::PlayerId id, std::uint8_t slot, std::uint8_t colourType,
                              std::uint8_t colour, std::uint8_t secondColour) override {
        overlayColours[id].push_back({slot, colourType, colour, secondColour});
        return std::ranges::find(playerList, id, &PlayerInfo::id) != playerList.end();
    }

    bool setHairColour(shared::PlayerId id, std::uint8_t colour, std::uint8_t highlight) override {
        hairColour[id] = {colour, highlight};
        return std::ranges::find(playerList, id, &PlayerInfo::id) != playerList.end();
    }

    bool addDecoration(shared::PlayerId id, std::uint32_t collection,
                       std::uint32_t overlay) override {
        if (!player(id) || collection == 0 || overlay == 0) {
            return false;
        }

        said.push_back(std::format("tattoo+ {} {:#x} {:#x}", id, collection, overlay));
        return true;
    }

    bool removeDecoration(shared::PlayerId id, std::uint32_t collection,
                          std::uint32_t overlay) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("tattoo- {} {:#x} {:#x}", id, collection, overlay));
        return true;
    }

    bool clearDecorations(shared::PlayerId id) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("tattoo clear {}", id));
        return true;
    }

    bool setFaceFeature(shared::PlayerId id, std::uint8_t index, float scale) override {
        if (!player(id) || index >= shared::kPedFaceFeatureCount) {
            return false;
        }

        features[id].push_back({index, scale});
        return true;
    }

    bool setEyeColour(shared::PlayerId id, std::uint8_t colour) override {
        eyeColour[id] = colour;
        return std::ranges::find(playerList, id, &PlayerInfo::id) != playerList.end();
    }

    [[nodiscard]] std::optional<shared::PlayerAppearance> appearance(
        shared::PlayerId id) const override {
        const auto found = looks.find(id);
        return found == looks.end() ? std::nullopt
                                    : std::optional<shared::PlayerAppearance>{found->second};
    }

    bool playAnimation(shared::PlayerId id, const AnimationInfo& animation) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("anim {} {}/{}", id, animation.dictionary, animation.name));
        return true;
    }

    void explode(const ExplosionInfo& explosion) override {
        said.push_back(std::format("boom {} at {} {} {}", explosion.kind, explosion.position.x,
                                   explosion.position.y, explosion.position.z));
    }

    /// Настройки подставного сервера. Правятся проверкой напрямую: здесь это
    /// просто поле, а не файл.
    ServerConfigInfo settings;

    [[nodiscard]] ServerConfigInfo config() const override { return settings; }

    /// Имена ресурсов, которые подставное ядро согласно знать. Пусто — согласно
    /// на любое.
    std::vector<std::string> catalogued;

    bool askResource(std::string_view name, ResourceAction action) override {
        if (!catalogued.empty()
            && std::ranges::find(catalogued, name) == catalogued.end()) {
            return false;
        }

        said.push_back(std::format("resource {} {}", static_cast<int>(action), name));
        return true;
    }

    bool clearTasks(shared::PlayerId id) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("anim- {}", id));
        return true;
    }

    bool setHealth(shared::PlayerId id, std::uint16_t health, std::uint16_t armour) override {
        const auto it = std::ranges::find(playerList, id, &PlayerInfo::id);
        if (it == playerList.end()) {
            return false;
        }

        it->health = health;
        it->armour = armour;
        return true;
    }

    bool setMaxArmour(shared::PlayerId id, std::uint16_t maxArmour) override {
        armourLimits[id] = maxArmour;
        return true;
    }

    bool giveWeapon(shared::PlayerId id, std::uint32_t weapon, std::uint16_t ammo,
                    bool equip) override {
        if (!player(id)) {
            return false;
        }

        // Признак «вложить в руки» записывается вместе с остальным: проверка,
        // сверяющая выдачу, обязана видеть и его — иначе выданное запасом и
        // выданное в руки для неё одно и то же.
        said.push_back(std::format("weapon {} {} {}{}", id, weapon, ammo, equip ? " held" : ""));
        return true;
    }

    bool clearWeapons(shared::PlayerId id) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("disarm {}", id));
        return true;
    }

    bool setModel(shared::PlayerId id, std::uint32_t model) override {
        const auto it = std::ranges::find(playerList, id, &PlayerInfo::id);
        if (it == playerList.end() || model == 0) {
            return false;
        }

        it->model = model;
        said.push_back(std::format("model {} {:#x}", id, model));
        return true;
    }

    bool setIntoVehicle(shared::PlayerId id, shared::VehicleId vehicle,
                        std::int8_t seat) override {
        if (!player(id) ||
            std::ranges::find(vehicleList, vehicle, &VehicleInfo::id) == vehicleList.end()) {
            return false;
        }

        said.push_back(std::format("seat {} {} {}", id, vehicle, seat));
        return true;
    }

    bool setClothes(shared::PlayerId id, std::uint8_t component, std::uint16_t drawable,
                    std::uint16_t texture, std::uint8_t palette) override {
        if (!player(id) || component >= shared::kPedComponentCount) {
            return false;
        }

        said.push_back(
            std::format("clothes {} {} {} {} {}", id, component, drawable, texture, palette));
        return true;
    }

    bool setProp(shared::PlayerId id, std::uint8_t index, std::int16_t drawable,
                 std::int16_t texture) override {
        if (!player(id) || index >= shared::kPedPropCount) {
            return false;
        }

        said.push_back(std::format("prop {} {} {} {}", id, index, drawable, texture));
        return true;
    }

    bool setDimension(shared::PlayerId id, std::int32_t dimension) override {
        const auto it = std::ranges::find(playerList, id, &PlayerInfo::id);
        if (it == playerList.end()) {
            return false;
        }

        it->dimension = dimension;
        said.push_back(std::format("dimension {} {}", id, dimension));
        return true;
    }

    bool teleport(shared::PlayerId id, const shared::Vec3& position) override {
        const auto it = std::ranges::find(playerList, id, &PlayerInfo::id);
        if (it == playerList.end()) {
            return false;
        }

        // У настоящего ядра перенос — просьба к игре на той стороне, и здесь она
        // считается исполненной сразу. Разница для проверок несущественна: их
        // занимает, кого переносят и вправе ли просивший.
        it->position = position;

        said.push_back(std::format("teleport {} {:.1f} {:.1f} {:.1f}", id, position.x, position.y,
                                   position.z));
        return true;
    }

    bool kick(shared::PlayerId id, std::string_view reason) override {
        if (!player(id)) {
            return false;
        }

        // Игрок остаётся в списке, как и у настоящего ядра: разрыв соединения не
        // мгновенен, и до него выгнанный ещё числится в сессии.
        said.push_back(std::format("kick {} {}", id, reason));
        return true;
    }

    bool emit(shared::PlayerId id, std::string_view name, std::string_view payload) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("emit {} {} {}", id, name, payload));
        return true;
    }

    [[nodiscard]] std::vector<VehicleInfo> vehicles() const override { return vehicleList; }

    [[nodiscard]] std::optional<VehicleInfo> vehicle(shared::VehicleId id) const override {
        const auto it = std::ranges::find(vehicleList, id, &VehicleInfo::id);
        return it == vehicleList.end() ? std::nullopt : std::optional{*it};
    }

    [[nodiscard]] shared::VehicleId createVehicle(std::uint32_t model,
                                                   const shared::Vec3& position,
                                                   float heading) override {
        if (model == 0) {
            return shared::kInvalidVehicleId;
        }

        VehicleInfo info;
        info.id = nextVehicleId++;
        info.model = model;
        info.position = position;
        info.rotation = shared::Vec3{.x = 0.0F, .y = 0.0F, .z = heading};

        vehicleList.push_back(info);
        return info.id;
    }

    bool removeVehicle(shared::VehicleId id) override {
        return std::erase_if(vehicleList, [id](const VehicleInfo& info) {
                   return info.id == id;
               }) != 0;
    }

    bool teleportVehicle(shared::VehicleId id, const shared::Vec3& position,
                         float heading) override {
        const auto it = std::ranges::find(vehicleList, id, &VehicleInfo::id);
        if (it == vehicleList.end()) {
            return false;
        }

        it->position = position;
        it->rotation = shared::Vec3{.x = 0.0F, .y = 0.0F, .z = heading};

        said.push_back(std::format("vehicle teleport {} {:.1f} {:.1f} {:.1f}", id, position.x,
                                   position.y, position.z));
        return true;
    }

    bool repairVehicle(shared::VehicleId id) override {
        if (std::ranges::find(vehicleList, id, &VehicleInfo::id) == vehicleList.end()) {
            return false;
        }

        said.push_back(std::format("vehicle repair {}", id));
        return true;
    }

    [[nodiscard]] std::optional<VehicleAppearanceInfo> vehicleAppearance(
        shared::VehicleId id) const override {
        if (std::ranges::find(vehicleList, id, &VehicleInfo::id) == vehicleList.end()) {
            return std::nullopt;
        }

        const auto known = appearances.find(id);
        return known == appearances.end() ? VehicleAppearanceInfo{} : known->second;
    }

    bool setVehicleAppearance(shared::VehicleId id,
                              const VehicleAppearanceInfo& appearance) override {
        if (std::ranges::find(vehicleList, id, &VehicleInfo::id) == vehicleList.end()) {
            return false;
        }

        appearances[id] = appearance;
        said.push_back(std::format("vehicle look {}", id));
        return true;
    }

    bool setVehicleDimension(shared::VehicleId id, std::int32_t dimension) override {
        const auto it = std::ranges::find(vehicleList, id, &VehicleInfo::id);
        if (it == vehicleList.end()) {
            return false;
        }

        it->dimension = dimension;
        return true;
    }

    bool attachEntity(EntityRef entity, const AttachmentInfo& attachment) override {
        if (!alive(entity) || !alive(attachment.target) || entity == attachment.target) {
            return false;
        }

        attachments[nameOf(entity)] = attachment;
        said.push_back(std::format("attach {} to {}", nameOf(entity),
                                   nameOf(attachment.target)));
        return true;
    }

    bool detachEntity(EntityRef entity) override {
        if (attachments.erase(nameOf(entity)) == 0) {
            return false;
        }

        said.push_back(std::format("detach {}", nameOf(entity)));
        return true;
    }

    [[nodiscard]] std::optional<AttachmentInfo> attachment(EntityRef entity) const override {
        const auto found = attachments.find(nameOf(entity));
        return found == attachments.end() ? std::nullopt : std::optional{found->second};
    }

    [[nodiscard]] std::vector<PedInfo> peds() const override { return pedList; }

    [[nodiscard]] std::optional<PedInfo> ped(shared::PedId id) const override {
        const auto it = std::ranges::find(pedList, id, &PedInfo::id);
        return it == pedList.end() ? std::nullopt : std::optional{*it};
    }

    [[nodiscard]] shared::PedId createPed(const PedInfo& ped) override {
        if (ped.model == 0) {
            return shared::kInvalidPedId;
        }

        PedInfo info = ped;
        info.id = nextPedId++;
        pedList.push_back(info);

        said.push_back(std::format("ped {}", info.id));
        return info.id;
    }

    bool updatePed(shared::PedId id, const PedInfo& ped) override {
        const auto it = std::ranges::find(pedList, id, &PedInfo::id);
        if (it == pedList.end()) {
            return false;
        }

        const std::uint32_t model = it->model;

        *it = ped;
        it->id = id;
        it->model = model;

        said.push_back(std::format("ped {}", id));
        return true;
    }

    bool removePed(shared::PedId id) override {
        if (std::erase_if(pedList, [id](const PedInfo& info) { return info.id == id; }) == 0) {
            return false;
        }

        said.push_back(std::format("ped- {}", id));
        return true;
    }

    bool setPedDimension(shared::PedId id, std::int32_t dimension) override {
        const auto it = std::ranges::find(pedList, id, &PedInfo::id);
        if (it == pedList.end()) {
            return false;
        }

        it->dimension = dimension;
        return true;
    }

    [[nodiscard]] std::vector<ObjectInfo> objects() const override { return objectList; }

    [[nodiscard]] std::optional<ObjectInfo> object(shared::ObjectId id) const override {
        const auto it = std::ranges::find(objectList, id, &ObjectInfo::id);
        return it == objectList.end() ? std::nullopt : std::optional{*it};
    }

    [[nodiscard]] shared::ObjectId createObject(std::uint32_t model, const shared::Vec3& position,
                                                 const shared::Vec3& rotation) override {
        if (model == 0) {
            return shared::kInvalidObjectId;
        }

        ObjectInfo info;
        info.id = nextObjectId++;
        info.model = model;
        info.position = position;
        info.rotation = rotation;

        objectList.push_back(info);
        return info.id;
    }

    bool removeObject(shared::ObjectId id) override {
        return std::erase_if(objectList, [id](const ObjectInfo& info) {
                   return info.id == id;
               }) != 0;
    }

    bool setVehicleDoor(shared::VehicleId id, std::uint8_t door, std::uint8_t level) override {
        const auto it = std::ranges::find(vehicleList, id, &VehicleInfo::id);

        if (it == vehicleList.end() || door >= shared::kVehicleDoorCount ||
            level > shared::kDoorFullyOpen) {
            return false;
        }

        it->doorLevels = shared::withDoorLevel(it->doorLevels, door, level);
        return true;
    }

    bool setVehicleLock(shared::VehicleId id, std::uint8_t lockState) override {
        const auto it = std::ranges::find(vehicleList, id, &VehicleInfo::id);
        if (it == vehicleList.end()) {
            return false;
        }

        it->lockState = lockState;
        return true;
    }

    bool setVehicleWindows(shared::VehicleId id, std::uint8_t windowsOpen) override {
        const auto it = std::ranges::find(vehicleList, id, &VehicleInfo::id);
        if (it == vehicleList.end()) {
            return false;
        }

        it->windowsOpen = windowsOpen;
        return true;
    }

    bool moveObject(shared::ObjectId id, const shared::Vec3& position,
                    const shared::Vec3& rotation) override {
        const auto it = std::ranges::find(objectList, id, &ObjectInfo::id);
        if (it == objectList.end()) {
            return false;
        }

        it->position = position;
        it->rotation = rotation;
        return true;
    }

    bool setObjectDimension(shared::ObjectId id, std::int32_t dimension) override {
        const auto it = std::ranges::find(objectList, id, &ObjectInfo::id);
        if (it == objectList.end()) {
            return false;
        }

        it->dimension = dimension;
        return true;
    }

    // --- Метки на карте ---------------------------------------------------

    [[nodiscard]] std::vector<BlipInfo> blips() const override { return blipList; }

    [[nodiscard]] std::optional<BlipInfo> blip(shared::BlipId id) const override {
        const auto it = std::ranges::find(blipList, id, &BlipInfo::id);
        return it == blipList.end() ? std::nullopt : std::optional{*it};
    }

    [[nodiscard]] shared::BlipId createBlip(const BlipInfo& blip) override {
        BlipInfo kept = blip;
        kept.id = nextBlipId++;

        blipList.push_back(kept);
        said.push_back(std::format("blip {} {}", kept.id, kept.name));

        return kept.id;
    }

    bool updateBlip(shared::BlipId id, const BlipInfo& blip) override {
        const auto it = std::ranges::find(blipList, id, &BlipInfo::id);
        if (it == blipList.end()) {
            return false;
        }

        *it = blip;
        it->id = id;
        return true;
    }

    bool removeBlip(shared::BlipId id) override {
        return std::erase_if(blipList, [id](const BlipInfo& info) { return info.id == id; }) != 0;
    }

    // --- Нарисованное в мире ----------------------------------------------

    [[nodiscard]] std::vector<MarkerInfo> markers() const override { return markerList; }

    [[nodiscard]] std::optional<MarkerInfo> marker(shared::MarkerId id) const override {
        const auto it = std::ranges::find(markerList, id, &MarkerInfo::id);
        return it == markerList.end() ? std::nullopt : std::optional{*it};
    }

    [[nodiscard]] shared::MarkerId createMarker(const MarkerInfo& marker) override {
        MarkerInfo kept = marker;
        kept.id = nextMarkerId++;

        markerList.push_back(kept);
        said.push_back(std::format("marker {} type {}", kept.id, kept.type));

        return kept.id;
    }

    bool updateMarker(shared::MarkerId id, const MarkerInfo& marker) override {
        const auto it = std::ranges::find(markerList, id, &MarkerInfo::id);
        if (it == markerList.end()) {
            return false;
        }

        *it = marker;
        it->id = id;
        return true;
    }

    bool removeMarker(shared::MarkerId id) override {
        return std::erase_if(markerList,
                             [id](const MarkerInfo& info) { return info.id == id; }) != 0;
    }

    [[nodiscard]] std::vector<CheckpointInfo> checkpoints() const override {
        return checkpointList;
    }

    [[nodiscard]] std::optional<CheckpointInfo> checkpoint(
        shared::CheckpointId id) const override {
        const auto it = std::ranges::find(checkpointList, id, &CheckpointInfo::id);
        return it == checkpointList.end() ? std::nullopt : std::optional{*it};
    }

    [[nodiscard]] shared::CheckpointId createCheckpoint(
        const CheckpointInfo& checkpoint) override {
        CheckpointInfo kept = checkpoint;
        kept.id = nextCheckpointId++;

        checkpointList.push_back(kept);
        said.push_back(std::format("checkpoint {} type {}", kept.id, kept.type));

        return kept.id;
    }

    bool updateCheckpoint(shared::CheckpointId id, const CheckpointInfo& checkpoint) override {
        const auto it = std::ranges::find(checkpointList, id, &CheckpointInfo::id);
        if (it == checkpointList.end()) {
            return false;
        }

        *it = checkpoint;
        it->id = id;
        return true;
    }

    bool removeCheckpoint(shared::CheckpointId id) override {
        return std::erase_if(checkpointList,
                             [id](const CheckpointInfo& info) { return info.id == id; }) != 0;
    }

    void broadcast(std::string_view text) override {
        said.push_back(std::format("all: {}", text));
    }

    bool tell(shared::PlayerId id, std::string_view text) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("{}: {}", id, text));
        return true;
    }

    bool setWeather(std::string_view weather) override {
        said.push_back(std::format("weather {}", weather));
        return true;
    }

    bool setTime(std::uint8_t hour, std::uint8_t minute) override {
        said.push_back(std::format("time {}:{}", hour, minute));
        return true;
    }

    /// Заводит игрока — то, чего у настоящего ядра нет: игроки приходят по сети.
    PlayerInfo& join(shared::PlayerId id, std::string nickname, bool admin = false) {
        PlayerInfo info;
        info.id = id;
        info.nickname = std::move(nickname);
        info.health = 200;
        info.admin = admin;

        return playerList.emplace_back(std::move(info));
    }

    void leave(shared::PlayerId id) {
        std::erase_if(playerList, [id](const PlayerInfo& info) { return info.id == id; });
    }
};

} // namespace oxymp::script::testing
