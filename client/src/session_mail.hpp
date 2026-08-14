#pragma once

#include <oxymp/shared/protocol/messages.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oxymp::client {

/// Почта между потоком игры и сетевым потоком.
///
/// Всё остальное, что ходит между ними, — это состояние: последнее значение
/// вытесняет предыдущее, и потерять промежуточное не жалко. Здесь наоборот:
/// реплика в чате и попадание — события, каждое из которых обязано дойти ровно
/// один раз. Состоянием их не передать.
///
/// Направления два и они не смешиваются: игра просит отправить, сеть приносит
/// пришедшее. Раскладывать их по разным объектам незачем — оба конца одинаковы,
/// а вместе видно, что именно эти два потока обмениваются событиями.
///
/// Блокировка короткая и не спорная: под ней происходит только обмен векторов.
class SessionMail {
public:
    // --- Игра просит отправить -------------------------------------------------

    /// Реплика игрока в чат.
    void postChat(std::string text) {
        if (text.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        outgoingChat_.push_back(std::move(text));
    }

    /// Попадание по чужому игроку: считает его тот, кто попал.
    void postDamage(shared::PlayerId victim, std::uint16_t amount, std::uint32_t weapon) {
        shared::DamageReport report;
        report.victim = victim;
        report.amount = amount;
        report.weapon = weapon;

        const std::lock_guard guard{mutex_};
        outgoingDamage_.push_back(report);
    }

    [[nodiscard]] std::vector<std::string> takeOutgoingChat() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingChat_, {});
    }

    [[nodiscard]] std::vector<shared::DamageReport> takeOutgoingDamage() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingDamage_, {});
    }

    /// Именованное событие серверу: нажатие в интерфейсе.
    ///
    /// Единственное, чем игра теперь просит сервер что-либо сделать. Раньше
    /// здесь стояли просьбы завести машину, выдать оружие, сменить погоду — по
    /// очереди на каждую, — и все они выросли из админ-меню, жившего в клиенте.
    /// Меню уехало на сервер, а с ним и правила.
    void postEvent(std::string name, std::string payload) {
        if (name.empty()) {
            return;
        }

        shared::ClientEvent event;
        event.name = std::move(name);
        event.payload = std::move(payload);

        const std::lock_guard guard{mutex_};
        outgoingEvents_.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<shared::ClientEvent> takeOutgoingEvents() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingEvents_, {});
    }

    // --- Сеть приносит пришедшее -----------------------------------------------

    void deliverDamage(std::vector<shared::DamageTaken> taken) {
        if (taken.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingDamage_.insert(incomingDamage_.end(), taken.begin(), taken.end());
    }

    [[nodiscard]] std::vector<shared::DamageTaken> takeIncomingDamage() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingDamage_, {});
    }

    /// Снаряжение, выданное сервером.
    ///
    /// Состоянием, а не событием: список полный, и последний вытесняет
    /// предыдущий, ничего не теряя.
    void deliverLoadout(std::optional<shared::PlayerLoadout> loadout) {
        if (!loadout) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingLoadout_ = std::move(loadout);
    }

    [[nodiscard]] std::optional<shared::PlayerLoadout> takeIncomingLoadout() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingLoadout_, std::nullopt);
    }

    /// Здоровье, назначенное сервером.
    void deliverHealth(std::optional<shared::HealthChanged> health) {
        if (!health) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingHealth_ = health;
    }

    [[nodiscard]] std::optional<shared::HealthChanged> takeIncomingHealth() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingHealth_, std::nullopt);
    }

    /// Появившиеся и пропавшие предметы.
    ///
    /// Событиями, а не состоянием: каждое появление и каждая пропажа — это
    /// отдельное действие над миром, и потерять их нельзя.
    /// Точки, в которые сервер велел перенести игрока.
    ///
    /// Событиями: перенос — действие, и потерять его нельзя. Исполняет их поток
    /// игры, потому что переставить персонажа может только она.
    void deliverTeleports(std::vector<shared::Vec3> points) {
        if (points.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        teleports_.insert(teleports_.end(), points.begin(), points.end());
    }

    [[nodiscard]] std::vector<shared::Vec3> takeTeleports() {
        const std::lock_guard guard{mutex_};
        return std::exchange(teleports_, {});
    }

    /// Именованные события от сервера.
    ///
    /// Клиент их не толкует: имя и нагрузку сочиняет ресурс сервера, а здесь они
    /// лишь передаются странице интерфейса.
    void deliverServerEvents(std::vector<shared::ServerEvent> events) {
        if (events.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingEvents_.insert(incomingEvents_.end(), events.begin(), events.end());
    }

    [[nodiscard]] std::vector<shared::ServerEvent> takeIncomingEvents() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingEvents_, {});
    }

    void deliverObjects(std::vector<shared::ObjectAdded> added,
                        std::vector<shared::ObjectId> removed) {
        if (added.empty() && removed.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingObjects_.insert(incomingObjects_.end(), added.begin(), added.end());
        removedObjects_.insert(removedObjects_.end(), removed.begin(), removed.end());
    }

    [[nodiscard]] std::vector<shared::ObjectAdded> takeIncomingObjects() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingObjects_, {});
    }

    [[nodiscard]] std::vector<shared::ObjectId> takeRemovedObjects() {
        const std::lock_guard guard{mutex_};
        return std::exchange(removedObjects_, {});
    }

    /// Погода и время сессии.
    ///
    /// Состоянием, а не событием, и в этом отличие от соседей: промежуточные
    /// значения никому не нужны, важно последнее. Пропущенная минута ничего не
    /// значит — следующая придёт через две секунды.
    void deliverWorld(std::optional<shared::WorldState> state) {
        if (!state) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingWorld_ = std::move(state);
    }

    [[nodiscard]] std::optional<shared::WorldState> takeIncomingWorld() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingWorld_, std::nullopt);
    }

    /// Внешность чужой машины.
    ///
    /// Событием, а не состоянием, и это не мелочь: внешность приходит по одному
    /// сообщению на машину, и сложенная в «последнее значение» она вытеснила бы
    /// сама себя, стоило двум машинам объявиться в один сетевой тик.
    void deliverVehicleAppearances(std::vector<shared::VehicleAppearance> appearances) {
        if (appearances.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingAppearances_.insert(incomingAppearances_.end(), appearances.begin(),
                                    appearances.end());
    }

    [[nodiscard]] std::vector<shared::VehicleAppearance> takeIncomingVehicleAppearances() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingAppearances_, {});
    }

private:
    mutable std::mutex mutex_;

    std::vector<std::string> outgoingChat_;
    std::vector<shared::DamageReport> outgoingDamage_;
    std::vector<shared::ClientEvent> outgoingEvents_;

    std::vector<shared::DamageTaken> incomingDamage_;
    std::vector<shared::ServerEvent> incomingEvents_;
    std::vector<shared::Vec3> teleports_;
    std::vector<shared::VehicleAppearance> incomingAppearances_;
    std::optional<shared::WorldState> incomingWorld_;
    std::optional<shared::PlayerLoadout> incomingLoadout_;
    std::optional<shared::HealthChanged> incomingHealth_;
    std::vector<shared::ObjectAdded> incomingObjects_;
    std::vector<shared::ObjectId> removedObjects_;
};

} // namespace oxymp::client
