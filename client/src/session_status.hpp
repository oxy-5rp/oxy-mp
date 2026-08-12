#pragma once

#include <oxymp/client/connection.hpp>

#include <oxymp/shared/math/vec3.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace oxymp::client {

/// Чужой игрок в том виде, в каком его показывает игра.
struct RemoteView {
    shared::PlayerId id = shared::kInvalidPlayerId;
    std::string nickname;

    /// Состояние, посчитанное на текущее мгновение: положение уже пропущено
    /// через интерполяцию, остальное взято из последнего снимка.
    shared::PlayerState state;
};

/// Машина чужого игрока, посчитанная на текущее мгновение.
struct RemoteVehicleView {
    shared::VehicleState state;
};

/// Кто сейчас в сессии, кроме нас, и на чём они ездят.
///
/// Здесь, в отличие от остального состояния, без блокировки не обойтись:
/// передаётся не число, а список переменной длины со строками. Блокировка
/// заведомо коротка — под ней происходит только обмен двух векторов, — и
/// удерживают её обе стороны на считанные микросекунды.
///
/// Список готовится сетевым потоком целиком и подменяется разом. Так поток игры
/// не может застать его наполовину обновлённым: он либо старый, либо новый.
class RemoteRoster {
public:
    void replace(std::vector<RemoteView> players, std::vector<RemoteVehicleView> vehicles) {
        const std::lock_guard guard{mutex_};
        players_ = std::move(players);
        vehicles_ = std::move(vehicles);
    }

    [[nodiscard]] std::vector<RemoteView> snapshot() const {
        const std::lock_guard guard{mutex_};
        return players_;
    }

    [[nodiscard]] std::vector<RemoteVehicleView> vehicles() const {
        const std::lock_guard guard{mutex_};
        return vehicles_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<RemoteView> players_;
    std::vector<RemoteVehicleView> vehicles_;
};

/// Состояние своего игрока и его машины, уходящее на сервер.
///
/// Направление здесь обратное остальному: пишет поток игры, читает сетевой.
///
/// Раньше поля лежали порознь, каждое своим атомарным числом. С прибавлением
/// признаков, оружия, точки прицела и машины такой набор перестал быть
/// осмысленным: полей стало два десятка, и читатель мог застать половину снимка
/// от одного кадра, а половину от другого — например «в машине» вместе со старым
/// хозяином машины. Одна короткая блокировка честнее.
class LocalState {
public:
    void set(const shared::PlayerState& state,
             const std::optional<shared::VehicleState>& vehicle) {
        const std::lock_guard guard{mutex_};
        state_ = state;
        vehicle_ = vehicle;
        known_ = true;
    }

    /// Пусто, пока игрок не появился в мире: отправлять нули — значит собрать
    /// всех игроков сервера в начале координат.
    [[nodiscard]] std::optional<shared::PlayerState> get() const {
        const std::lock_guard guard{mutex_};
        return known_ ? std::optional{state_} : std::nullopt;
    }

    [[nodiscard]] std::optional<shared::VehicleState> vehicle() const {
        const std::lock_guard guard{mutex_};
        return known_ ? vehicle_ : std::nullopt;
    }

private:
    mutable std::mutex mutex_;
    shared::PlayerState state_;
    std::optional<shared::VehicleState> vehicle_;
    bool known_ = false;
};

/// Состояние сессии, доступное потоку игры.
///
/// Соединение живёт в своём потоке, а рисование — в потоке игры, и общее между
/// ними только это. Устроено намеренно на атомарных полях, без блокировок:
/// отрисовка кадра не имеет права ждать сеть, иначе сетевая заминка обернётся
/// подвисанием картинки.
///
/// Атомарные поля читаются по отдельности, поэтому снимок может застать
/// мгновение, когда состояние уже новое, а число игроков ещё старое. Для
/// показаний на экране это безразлично — расхождение живёт один кадр.
class SessionStatus {
public:
    /// Что показывать на экране.
    struct Snapshot {
        ConnectionState state = ConnectionState::Waiting;
        std::size_t players = 0;

        /// Время оборота до сервера, отрицательное — пока неизвестно.
        int latencyMilliseconds = -1;

        /// Наш номер на сервере. Нужен не только для показа: сидя за рулём, мы
        /// называем себя хозяином машины, а назвать себя можно только числом,
        /// которое выдал сервер.
        shared::PlayerId playerId = shared::kInvalidPlayerId;

        [[nodiscard]] bool inSession() const noexcept {
            return state == ConnectionState::Connected;
        }
    };

    void update(ConnectionState state, std::size_t players,
                std::optional<std::chrono::milliseconds> latency,
                shared::PlayerId playerId) noexcept {
        state_.store(state, std::memory_order_relaxed);
        players_.store(static_cast<std::uint32_t>(players), std::memory_order_relaxed);
        latency_.store(latency.has_value() ? static_cast<std::int32_t>(latency->count()) : -1,
                       std::memory_order_relaxed);
        playerId_.store(playerId, std::memory_order_relaxed);
    }

    [[nodiscard]] Snapshot snapshot() const noexcept {
        return Snapshot{
            .state = state_.load(std::memory_order_relaxed),
            .players = players_.load(std::memory_order_relaxed),
            .latencyMilliseconds = latency_.load(std::memory_order_relaxed),
            .playerId = playerId_.load(std::memory_order_relaxed),
        };
    }

private:
    std::atomic<ConnectionState> state_{ConnectionState::Waiting};
    std::atomic<std::uint32_t> players_{0};
    std::atomic<std::int32_t> latency_{-1};
    std::atomic<shared::PlayerId> playerId_{shared::kInvalidPlayerId};
};

} // namespace oxymp::client
