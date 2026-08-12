#pragma once

#include <oxymp/shared/protocol/messages.hpp>

#include <mutex>
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

    /// Распоряжение из админ-меню.
    void postAdmin(shared::AdminCommand command, shared::PlayerId target, shared::Vec3 position) {
        shared::AdminAction action;
        action.command = command;
        action.target = target;
        action.position = position;

        const std::lock_guard guard{mutex_};
        outgoingAdmin_.push_back(action);
    }

    [[nodiscard]] std::vector<shared::AdminAction> takeOutgoingAdmin() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingAdmin_, {});
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

    void deliverAdmin(std::vector<shared::AdminOrder> orders) {
        if (orders.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingAdmin_.insert(incomingAdmin_.end(), orders.begin(), orders.end());
    }

    [[nodiscard]] std::vector<shared::AdminOrder> takeIncomingAdmin() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingAdmin_, {});
    }

private:
    mutable std::mutex mutex_;

    std::vector<std::string> outgoingChat_;
    std::vector<shared::DamageReport> outgoingDamage_;
    std::vector<shared::AdminAction> outgoingAdmin_;

    std::vector<shared::DamageTaken> incomingDamage_;
    std::vector<shared::AdminOrder> incomingAdmin_;
};

} // namespace oxymp::client
