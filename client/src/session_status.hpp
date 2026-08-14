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

/// Машина сессии, посчитанная на текущее мгновение.
///
/// Не «чужая»: список машин ведёт сервер, и в нём все машины сессии, включая те,
/// что ведём мы сами. Отличить их друг от друга можно по ведущему.
struct SessionVehicleView {
    shared::VehicleState state;

    /// Кто её ведёт. kInvalidPlayerId — никто, машина стоит.
    shared::PlayerId owner = shared::kInvalidPlayerId;
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
    void replace(std::vector<RemoteView> players, std::vector<SessionVehicleView> vehicles) {
        const std::lock_guard guard{mutex_};
        players_ = std::move(players);
        vehicles_ = std::move(vehicles);
    }

    [[nodiscard]] std::vector<RemoteView> snapshot() const {
        const std::lock_guard guard{mutex_};
        return players_;
    }

    [[nodiscard]] std::vector<SessionVehicleView> vehicles() const {
        const std::lock_guard guard{mutex_};
        return vehicles_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<RemoteView> players_;
    std::vector<SessionVehicleView> vehicles_;
};

/// Состояние своего игрока и ведомых им машин, уходящее на сервер.
///
/// Направление здесь обратное остальному: пишет поток игры, читает сетевой.
///
/// Раньше поля лежали порознь, каждое своим атомарным числом. С прибавлением
/// признаков, оружия, точки прицела и машины такой набор перестал быть
/// осмысленным: полей стало два десятка, и читатель мог застать половину снимка
/// от одного кадра, а половину от другого — например «в машине» вместе со старым
/// хозяином машины. Одна короткая блокировка честнее.
///
/// Машина здесь не одна, и это не запас на будущее: ведущим машины назначают
/// того, кто к ней ближе всех, и стоящий посреди двора отвечает разом за все
/// машины во дворе.
class LocalState {
public:
    void set(const shared::PlayerState& state, std::vector<shared::VehicleState> vehicles,
             std::vector<shared::VehicleAppearance> appearances) {
        const std::lock_guard guard{mutex_};
        state_ = state;
        vehicles_ = std::move(vehicles);
        appearances_ = std::move(appearances);
        known_ = true;
    }

    /// Пусто, пока игрок не появился в мире: отправлять нули — значит собрать
    /// всех игроков сервера в начале координат.
    [[nodiscard]] std::optional<shared::PlayerState> get() const {
        const std::lock_guard guard{mutex_};
        return known_ ? std::optional{state_} : std::nullopt;
    }

    /// Снимки машин, которые ведём мы.
    [[nodiscard]] std::vector<shared::VehicleState> vehicles() const {
        const std::lock_guard guard{mutex_};
        return known_ ? vehicles_ : std::vector<shared::VehicleState>{};
    }

    /// Как выглядят машины, которые мы ведём.
    ///
    /// Снимаются редко и отправляются только при изменении, но лежат здесь же:
    /// разойдись они со снимками машин — и цвет уехал бы к машине, которой уже
    /// нет.
    [[nodiscard]] std::vector<shared::VehicleAppearance> appearances() const {
        const std::lock_guard guard{mutex_};
        return known_ ? appearances_ : std::vector<shared::VehicleAppearance>{};
    }

private:
    mutable std::mutex mutex_;
    shared::PlayerState state_;
    std::vector<shared::VehicleState> vehicles_;
    std::vector<shared::VehicleAppearance> appearances_;
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

        /// Где сервер велел появиться. Пусто — он ещё не сказал.
        ///
        /// От сервера, а не из сборки клиента: точка появления — свойство
        /// сессии, и хозяин вправе поменять её одной строкой в server.cfg.
        std::optional<shared::Vec3> spawn;

        /// Деньги игрока, как их считает сервер.
        ///
        /// Известны не сразу: сервер присылает их сразу за приветствием, но до
        /// того показывать нечего, и прочерк честнее нуля.
        std::optional<std::int64_t> money;

        [[nodiscard]] bool inSession() const noexcept {
            return state == ConnectionState::Connected;
        }
    };

    /// Запоминает точку появления, названную сервером.
    ///
    /// Три числа пишутся раньше признака их наличия, а признак — с барьером.
    /// Порядок обязателен: без него читатель вправе увидеть поднятый признак
    /// раньше самих чисел и появиться в начале координат.
    void setSpawn(shared::Vec3 position) noexcept {
        spawnX_.store(position.x, std::memory_order_relaxed);
        spawnY_.store(position.y, std::memory_order_relaxed);
        spawnZ_.store(position.z, std::memory_order_relaxed);
        spawnKnown_.store(true, std::memory_order_release);
    }

    void update(ConnectionState state, std::size_t players,
                std::optional<std::chrono::milliseconds> latency,
                shared::PlayerId playerId) noexcept {
        state_.store(state, std::memory_order_relaxed);
        players_.store(static_cast<std::uint32_t>(players), std::memory_order_relaxed);
        latency_.store(latency.has_value() ? static_cast<std::int32_t>(latency->count()) : -1,
                       std::memory_order_relaxed);
        playerId_.store(playerId, std::memory_order_relaxed);
    }

    /// Деньги приходят отдельным сообщением и меняются редко, поэтому пишутся
    /// отдельно от остального: складывать их в общий update значило бы требовать
    /// их у каждого, кто обновляет задержку.
    void setMoney(std::int64_t amount) noexcept {
        money_.store(amount, std::memory_order_relaxed);
        moneyKnown_.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] Snapshot snapshot() const noexcept {
        // Признак читается первым и с барьером — парой к записи выше: увидев
        // его поднятым, читатель заведомо видит и сами числа.
        const bool spawnKnown = spawnKnown_.load(std::memory_order_acquire);

        return Snapshot{
            .state = state_.load(std::memory_order_relaxed),
            .players = players_.load(std::memory_order_relaxed),
            .latencyMilliseconds = latency_.load(std::memory_order_relaxed),
            .playerId = playerId_.load(std::memory_order_relaxed),
            .spawn = spawnKnown ? std::optional{shared::Vec3{
                                      .x = spawnX_.load(std::memory_order_relaxed),
                                      .y = spawnY_.load(std::memory_order_relaxed),
                                      .z = spawnZ_.load(std::memory_order_relaxed),
                                  }}
                                : std::nullopt,
            .money = moneyKnown_.load(std::memory_order_relaxed)
                         ? std::optional{money_.load(std::memory_order_relaxed)}
                         : std::nullopt,
        };
    }

private:
    std::atomic<ConnectionState> state_{ConnectionState::Waiting};
    std::atomic<std::uint32_t> players_{0};
    std::atomic<std::int32_t> latency_{-1};
    std::atomic<shared::PlayerId> playerId_{shared::kInvalidPlayerId};
    std::atomic<std::int64_t> money_{0};
    std::atomic<bool> moneyKnown_{false};

    std::atomic<float> spawnX_{0.0F};
    std::atomic<float> spawnY_{0.0F};
    std::atomic<float> spawnZ_{0.0F};
    std::atomic<bool> spawnKnown_{false};
};

} // namespace oxymp::client
