#pragma once

#include <oxymp/script/core.hpp>

#include <algorithm>
#include <format>
#include <string>

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

    /// Что ядру велели сделать. Проверки смотрят сюда вместо сети.
    std::vector<std::string> said;

    shared::VehicleId nextVehicleId = 1;
    shared::ObjectId nextObjectId = 1;

    [[nodiscard]] std::vector<PlayerInfo> players() const override { return playerList; }

    [[nodiscard]] std::optional<PlayerInfo> player(shared::PlayerId id) const override {
        const auto it = std::ranges::find(playerList, id, &PlayerInfo::id);
        return it == playerList.end() ? std::nullopt : std::optional{*it};
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

    bool giveWeapon(shared::PlayerId id, std::uint32_t weapon, std::uint16_t ammo) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("weapon {} {} {}", id, weapon, ammo));
        return true;
    }

    bool clearWeapons(shared::PlayerId id) override {
        if (!player(id)) {
            return false;
        }

        said.push_back(std::format("disarm {}", id));
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
