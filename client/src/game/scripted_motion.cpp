#include "scripted_motion.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Сколько движению, названному сервером, даётся на то, чтобы начаться, в
/// миллисекундах.
///
/// Треть секунды — с запасом на низкую частоту кадров и заведомо меньше всякого
/// осмысленного движения.
constexpr std::int32_t kGrace = 300;

/// Довод IS_ENTITY_PLAYING_ANIM, которым его зовут скрипты самой игры.
constexpr int kPlayingAnimTaskFlag = 3;

} // namespace

ScriptedMotion::ScriptedMotion(const NativeTable& table) noexcept
    : playingAnim_(table.handlerFor(natives::kIsEntityPlayingAnim)),
      usingScenario_(table.handlerFor(natives::kIsPedUsingScenario)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)) {}

std::int32_t ScriptedMotion::now() const {
    return gameTimer_ != nullptr ? invokeNative<std::int32_t>(gameTimer_) : 0;
}

void ScriptedMotion::begin(const shared::PlayerAnimation& animation) {
    dictionary_ = animation.dictionary;
    scenario_ = !animation.scenario.empty();
    name_ = scenario_ ? animation.scenario : animation.name;
    startedAt = now();
}

void ScriptedMotion::forget() noexcept {
    dictionary_.clear();
    name_.clear();
    scenario_ = false;
    startedAt = 0;
}

bool ScriptedMotion::playing(int ped) {
    if (!named()) {
        return false;
    }

    if (startedAt != 0 && now() - startedAt < kGrace) {
        return true;
    }

    if (scenario_) {
        if (usingScenario_ != nullptr &&
            invokeNative<bool>(usingScenario_, ped, name_.c_str())) {
            return true;
        }
    } else if (playingAnim_ != nullptr &&
               invokeNative<bool>(playingAnim_, ped, dictionary_.c_str(), name_.c_str(),
                                  kPlayingAnimTaskFlag)) {
        return true;
    }

    forget();
    return false;
}

} // namespace oxymp::client::game
