#include "screen.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {

Screen::Screen(const NativeTable& table) noexcept
    : shutdownLoading_(table.handlerFor(natives::kShutdownLoadingScreen)),
      fadeIn_(table.handlerFor(natives::kDoScreenFadeIn)),
      fadeOut_(table.handlerFor(natives::kDoScreenFadeOut)),
      fadeInAfterLoad_(table.handlerFor(natives::kSetFadeInAfterLoad)),
      fadeInAfterDeath_(table.handlerFor(natives::kSetFadeInAfterDeathArrest)),
      displayHud_(table.handlerFor(natives::kDisplayHud)),
      displayRadar_(table.handlerFor(natives::kDisplayRadar)),
      revealMap_(table.handlerFor(natives::kSetMinimapRevealed)),
      hudWhenDead_(table.handlerFor(natives::kDisplayHudWhenDeadThisFrame)) {}

bool Screen::ready() const noexcept {
    return shutdownLoading_ != nullptr && fadeIn_ != nullptr && fadeOut_ != nullptr &&
           fadeInAfterLoad_ != nullptr && fadeInAfterDeath_ != nullptr && displayHud_ != nullptr &&
           displayRadar_ != nullptr;
}

void Screen::keepInterfaceUp() const {
    if (displayHud_ != nullptr && displayRadar_ != nullptr) {
        invokeNative<void>(displayHud_, true);
        invokeNative<void>(displayRadar_, true);
    }

    // Действует ровно один кадр — отсюда и «ThisFrame» в имени. Постоянного
    // выключателя у этого запрета нет.
    if (hudWhenDead_ != nullptr) {
        invokeNative<void>(hudWhenDead_);
    }

    if (revealMap_ != nullptr) {
        invokeNative<void>(revealMap_, true);
    }
}


void Screen::hideLoadingScreen() const {
    if (shutdownLoading_ == nullptr) {
        return;
    }

    invokeNative<void>(shutdownLoading_);
}

void Screen::fadeIn(int milliseconds) const {
    if (fadeIn_ == nullptr) {
        return;
    }

    invokeNative<void>(fadeIn_, milliseconds);
}

void Screen::fadeOut(int milliseconds) const {
    if (fadeOut_ == nullptr) {
        return;
    }

    invokeNative<void>(fadeOut_, milliseconds);
}

void Screen::takeOverFades() const {
    if (fadeInAfterLoad_ == nullptr || fadeInAfterDeath_ == nullptr) {
        return;
    }

    invokeNative<void>(fadeInAfterLoad_, false);
    invokeNative<void>(fadeInAfterDeath_, false);
}

} // namespace oxymp::client::game
