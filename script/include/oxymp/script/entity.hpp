#pragma once

#include "core.hpp"

namespace oxymp::script {

/// Ссылка на игрока сессии.
///
/// Держит номер и ядро, а не указатель на запись, и это не осторожность, а
/// необходимость. Игрок волен выйти посреди обработчика события: скрипт,
/// откликнувшийся на его выстрел, вправе решить показать строку в чат — а к
/// этому мгновению отправителя уже нет. Указатель на его запись пережил бы её
/// саму.
///
/// Поэтому всякое обращение разрешает номер заново. Дороже указателя — и
/// заведомо дешевле обращения к тому, чего нет.
///
/// Недействительная ссылка не бросает исключений и не падает: она отвечает
/// пустыми значениями, а на вопрос valid() — отрицанием. Скрипт, забывший
/// спросить, получит нули, а не крах сервера.
class Player {
public:
    Player() = default;
    Player(Core& core, shared::PlayerId id) noexcept : core_(&core), id_(id) {}

    /// Есть ли такой игрок в сессии прямо сейчас.
    [[nodiscard]] bool valid() const;

    [[nodiscard]] shared::PlayerId id() const noexcept { return id_; }

    /// Всё, что о нём известно на это мгновение. Пусто — его больше нет.
    [[nodiscard]] std::optional<PlayerInfo> info() const;

    [[nodiscard]] std::string nickname() const;
    [[nodiscard]] shared::Vec3 position() const;
    [[nodiscard]] std::uint16_t health() const;
    [[nodiscard]] std::uint16_t armour() const;

    /// Позволено ли ему распоряжаться сессией.
    ///
    /// Спрашивать обязан всякий обработчик, меняющий мир: событие от клиента
    /// сервером не проверено, а собрать его может кто угодно.
    [[nodiscard]] bool admin() const;

    /// В какой машине он сидит. Недействительная — идёт пешком.
    [[nodiscard]] class Vehicle vehicle() const;

    bool setHealth(std::uint16_t health, std::uint16_t armour) const;
    bool giveWeapon(std::uint32_t weapon, std::uint16_t ammo) const;
    bool clearWeapons() const;

    /// Переносит его в точку.
    bool teleport(const shared::Vec3& position) const;

    /// Строка в чат ему одному.
    bool tell(std::string_view text) const;

    /// Именованное событие его странице интерфейса.
    bool emit(std::string_view name, std::string_view payload) const;

    [[nodiscard]] friend bool operator==(const Player& left, const Player& right) noexcept {
        return left.core_ == right.core_ && left.id_ == right.id_;
    }

private:
    Core* core_ = nullptr;
    shared::PlayerId id_ = shared::kInvalidPlayerId;
};

/// Ссылка на машину сессии. Устроена так же и по тем же причинам.
class Vehicle {
public:
    Vehicle() = default;
    Vehicle(Core& core, shared::VehicleId id) noexcept : core_(&core), id_(id) {}

    [[nodiscard]] bool valid() const;

    [[nodiscard]] shared::VehicleId id() const noexcept { return id_; }
    [[nodiscard]] std::optional<VehicleInfo> info() const;

    [[nodiscard]] std::uint32_t model() const;
    [[nodiscard]] shared::Vec3 position() const;
    [[nodiscard]] shared::Vec3 rotation() const;

    /// Кто её ведёт — то есть у кого она живёт по-настоящему и кто считает её
    /// физику. Недействительный игрок означает, что машину не ведёт никто: она
    /// стоит там, где её оставили, и это обычное положение вещей.
    [[nodiscard]] Player owner() const;

    bool remove() const;

    [[nodiscard]] friend bool operator==(const Vehicle& left, const Vehicle& right) noexcept {
        return left.core_ == right.core_ && left.id_ == right.id_;
    }

private:
    Core* core_ = nullptr;
    shared::VehicleId id_ = shared::kInvalidVehicleId;
};

/// Ссылка на предмет сессии.
///
/// Беднее машины, и это не упущение: предмет стоит. Ведущего у него нет, физику
/// ему считать некому, и всё, что о нём нужно знать, задано при появлении.
class Object {
public:
    Object() = default;
    Object(Core& core, shared::ObjectId id) noexcept : core_(&core), id_(id) {}

    [[nodiscard]] bool valid() const;

    [[nodiscard]] shared::ObjectId id() const noexcept { return id_; }
    [[nodiscard]] std::optional<ObjectInfo> info() const;

    [[nodiscard]] std::uint32_t model() const;
    [[nodiscard]] shared::Vec3 position() const;
    [[nodiscard]] shared::Vec3 rotation() const;

    bool remove() const;

    [[nodiscard]] friend bool operator==(const Object& left, const Object& right) noexcept {
        return left.core_ == right.core_ && left.id_ == right.id_;
    }

private:
    Core* core_ = nullptr;
    shared::ObjectId id_ = shared::kInvalidObjectId;
};

} // namespace oxymp::script
