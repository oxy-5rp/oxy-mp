#pragma once

#include "native_table.hpp"

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oxymp::client::game {

/// Машины чужих игроков в игровом мире.
///
/// Машина числится за своим водителем и живёт ровно столько, сколько он в ней
/// сидит. Отдельной нумерации машин нет намеренно: сервер не ведёт мир, он
/// пересылает снимки, — а раз так, машин в сессии ровно столько, сколько игроков
/// за рулём, и называть их проще по хозяину.
///
/// У этого решения есть граница, и её стоит знать: брошенная машина исчезает.
/// Синхронизировать весь транспорт мира — другая задача, требующая от сервера
/// хранить его состояние; здесь же синхронизируется то, на чём ездят игроки.
///
/// Вызывать можно только изнутри скриптового тика.
class Vehicles {
public:
    explicit Vehicles(const NativeTable& table) noexcept;
    ~Vehicles();

    Vehicles(const Vehicles&) = delete;
    Vehicles& operator=(const Vehicles&) = delete;

    [[nodiscard]] bool ready() const noexcept;

    /// Где сидит персонаж.
    struct Seat {
        /// Номер машины в игре.
        int vehicle = 0;

        /// Место: -1 водитель, 0 и дальше — пассажирские.
        std::int8_t index = shared::kDriverSeat;

        /// Персонаж за рулём. Нужен пассажиру: по нему он узнаёт, чья это
        /// машина, — а больше узнать это неоткуда.
        int driverPed = 0;
    };

    /// В какой машине и на каком месте сидит персонаж. Пусто — идёт пешком.
    [[nodiscard]] std::optional<Seat> seatOf(int ped) const;

    /// Снимок машины для отправки на сервер.
    [[nodiscard]] shared::VehicleState describe(int vehicle) const;

    /// Приводит мир в соответствие со списком чужих машин. Вызывать раз в кадр.
    void sync(const std::vector<shared::VehicleState>& vehicles);

    /// Номер машины, которой показана машина указанного игрока.
    ///
    /// Ноль означает «ещё нет»: модель могла не успеть загрузиться. Сажать
    /// персонажа в этом кадре некуда, и это нормально — сядет в следующем.
    [[nodiscard]] int handleFor(shared::PlayerId owner) const;

    /// Убирает все машины. Нужно при выходе.
    void clear();

    [[nodiscard]] std::size_t shown() const noexcept { return puppets_.size(); }

private:
    struct Puppet {
        int vehicle = 0;
        std::uint32_t model = 0;
    };

    /// Заводит машину по снимку. Ноль, если модель ещё не загрузилась.
    [[nodiscard]] int spawn(const shared::VehicleState& state);

    void remove(int vehicle) const;

    /// Ставит машину туда, где ей положено быть на этот кадр.
    void place(int vehicle, const shared::VehicleState& state) const;

    NativeHandler requestModel_ = nullptr;
    NativeHandler hasModelLoaded_ = nullptr;
    NativeHandler modelNoLongerNeeded_ = nullptr;
    NativeHandler createVehicle_ = nullptr;
    NativeHandler deleteVehicle_ = nullptr;
    NativeHandler doesExist_ = nullptr;
    NativeHandler setCoords_ = nullptr;
    NativeHandler setRotation_ = nullptr;
    NativeHandler setVelocity_ = nullptr;
    NativeHandler getRotation_ = nullptr;
    NativeHandler getVelocity_ = nullptr;
    NativeHandler getCoords_ = nullptr;
    NativeHandler getModel_ = nullptr;
    NativeHandler engineOn_ = nullptr;
    NativeHandler asMissionEntity_ = nullptr;
    NativeHandler lodDistance_ = nullptr;
    NativeHandler invincible_ = nullptr;
    NativeHandler isInAnyVehicle_ = nullptr;
    NativeHandler vehiclePedIsIn_ = nullptr;
    NativeHandler pedInSeat_ = nullptr;

    std::unordered_map<shared::PlayerId, Puppet> puppets_;
};

} // namespace oxymp::client::game
