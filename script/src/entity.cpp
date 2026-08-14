#include <oxymp/script/entity.hpp>

namespace oxymp::script {

// Общий приём всех трёх ссылок: сперва ядро, потом разрешение номера. Ядра нет
// у ссылки, собранной по умолчанию, — такая встречается там, где событие не
// называет второго участника: смерть от падения обходится без убийцы.

std::optional<PlayerInfo> Player::info() const {
    return core_ == nullptr ? std::nullopt : core_->player(id_);
}

bool Player::valid() const {
    return info().has_value();
}

std::string Player::nickname() const {
    const auto known = info();
    return known ? known->nickname : std::string{};
}

shared::Vec3 Player::position() const {
    const auto known = info();
    return known ? known->position : shared::Vec3{};
}

std::uint16_t Player::health() const {
    const auto known = info();
    return known ? known->health : 0;
}

std::uint16_t Player::armour() const {
    const auto known = info();
    return known ? known->armour : 0;
}

bool Player::admin() const {
    const auto known = info();
    return known && known->admin;
}

Vehicle Player::vehicle() const {
    const auto known = info();

    if (!known || known->vehicle == shared::kInvalidVehicleId) {
        return {};
    }

    return Vehicle{*core_, known->vehicle};
}

bool Player::setHealth(std::uint16_t health, std::uint16_t armour) const {
    return core_ != nullptr && core_->setHealth(id_, health, armour);
}

bool Player::giveWeapon(std::uint32_t weapon, std::uint16_t ammo) const {
    return core_ != nullptr && core_->giveWeapon(id_, weapon, ammo);
}

bool Player::clearWeapons() const {
    return core_ != nullptr && core_->clearWeapons(id_);
}

bool Player::teleport(const shared::Vec3& position) const {
    return core_ != nullptr && core_->teleport(id_, position);
}

bool Player::tell(std::string_view text) const {
    return core_ != nullptr && core_->tell(id_, text);
}

bool Player::emit(std::string_view name, std::string_view payload) const {
    return core_ != nullptr && core_->emit(id_, name, payload);
}

std::optional<VehicleInfo> Vehicle::info() const {
    return core_ == nullptr ? std::nullopt : core_->vehicle(id_);
}

bool Vehicle::valid() const {
    return info().has_value();
}

std::uint32_t Vehicle::model() const {
    const auto known = info();
    return known ? known->model : 0;
}

shared::Vec3 Vehicle::position() const {
    const auto known = info();
    return known ? known->position : shared::Vec3{};
}

shared::Vec3 Vehicle::rotation() const {
    const auto known = info();
    return known ? known->rotation : shared::Vec3{};
}

Player Vehicle::owner() const {
    const auto known = info();

    if (!known || known->owner == shared::kInvalidPlayerId) {
        return {};
    }

    return Player{*core_, known->owner};
}

bool Vehicle::remove() const {
    return core_ != nullptr && core_->removeVehicle(id_);
}

std::optional<ObjectInfo> Object::info() const {
    return core_ == nullptr ? std::nullopt : core_->object(id_);
}

bool Object::valid() const {
    return info().has_value();
}

std::uint32_t Object::model() const {
    const auto known = info();
    return known ? known->model : 0;
}

shared::Vec3 Object::position() const {
    const auto known = info();
    return known ? known->position : shared::Vec3{};
}

shared::Vec3 Object::rotation() const {
    const auto known = info();
    return known ? known->rotation : shared::Vec3{};
}

bool Object::remove() const {
    return core_ != nullptr && core_->removeObject(id_);
}

} // namespace oxymp::script
