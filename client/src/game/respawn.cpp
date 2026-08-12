#include "respawn.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Сколько игрок лежит мёртвым, прежде чем встать.
constexpr std::int32_t kDeathPauseMilliseconds = 3000;

} // namespace

Respawn::Respawn(const NativeTable& table) noexcept
    : pauseDeathRestart_(table.handlerFor(natives::kPauseDeathArrestRestart)),
      ignoreNextRestart_(table.handlerFor(natives::kIgnoreNextRestart)),
      fadeOutAfterDeath_(table.handlerFor(natives::kSetFadeOutAfterDeath)),
      isPlayerDead_(table.handlerFor(natives::kIsPlayerDead)),
      resurrect_(table.handlerFor(natives::kNetworkResurrectLocalPlayer)),
      resetArrest_(table.handlerFor(natives::kResetPlayerArrestState)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)) {}

bool Respawn::ready() const noexcept {
    return pauseDeathRestart_ != nullptr && ignoreNextRestart_ != nullptr &&
           fadeOutAfterDeath_ != nullptr && isPlayerDead_ != nullptr && resurrect_ != nullptr &&
           gameTimer_ != nullptr;
}

void Respawn::suppressGameHandling() const {
    if (!ready()) {
        return;
    }

    invokeNative<void>(pauseDeathRestart_, true);
    invokeNative<void>(ignoreNextRestart_, true);
    invokeNative<void>(fadeOutAfterDeath_, false);
}

bool Respawn::dead(int player) const {
    if (!ready()) {
        return false;
    }

    return invokeNative<bool>(isPlayerDead_, player);
}

bool Respawn::due(bool isDead) {
    if (!ready()) {
        return false;
    }

    if (!isDead) {
        diedAt_ = 0;
        return false;
    }

    const auto now = invokeNative<std::int32_t>(gameTimer_);

    if (diedAt_ == 0) {
        diedAt_ = now;
        return false;
    }

    return now - diedAt_ >= kDeathPauseMilliseconds;
}

void Respawn::resurrect(shared::Vec3 point, float heading, int player) {
    if (!ready()) {
        return;
    }

    // Признаки: не менять игровое время и не восстанавливать окружение вокруг
    // точки. Оба лишние — временем и миром распоряжаемся мы сами.
    invokeNative<void>(resurrect_, point.x, point.y, point.z, heading, false, false, false);

    // Игра помнит, что игрок умер или был арестован, и пока помнит — держит
    // интерфейс погашенным, а меню паузы урезанным. Подъём эту память сам не
    // стирает: стирать её положено тому порядку разбора смерти, который мы у
    // игры отобрали. Отсюда и «после смерти пропал интерфейс и не открыть
    // карту» — воскресший игрок для игры оставался покойником.
    if (resetArrest_ != nullptr) {
        invokeNative<void>(resetArrest_, player);
    }

    diedAt_ = 0;
}

} // namespace oxymp::client::game
