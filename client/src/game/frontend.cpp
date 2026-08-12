#include "frontend.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {

Frontend::Frontend(const NativeTable& table) noexcept
    : loadingScreenActive_(table.handlerFor(natives::kIsLoadingScreenActive)),
      pauseMenuActive_(table.handlerFor(natives::kIsPauseMenuActive)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)) {}

bool Frontend::ready() const noexcept {
    return loadingScreenActive_ != nullptr && pauseMenuActive_ != nullptr;
}

bool Frontend::showing() const {
    if (!ready()) {
        return false;
    }

    return !invokeNative<bool>(loadingScreenActive_) && invokeNative<bool>(pauseMenuActive_);
}

std::optional<Frontend::PauseMeasurement> Frontend::measurePause() {
    if (!ready() || gameTimer_ == nullptr) {
        return std::nullopt;
    }

    const bool now = showing();

    if (now) {
        if (!wasShowing_) {
            wasShowing_ = true;
            framesWhileShowing_ = 0;
            openedAtGameTime_ = invokeNative<std::int32_t>(gameTimer_);
            return std::nullopt;
        }

        // Считается здесь, а не по счётчику кадров игры, и это важно: игра
        // рисует кадры и при остановленном мире, а вопрос стоит про наш тик.
        // Дошло сюда управление — значит тик дошёл.
        ++framesWhileShowing_;
        return std::nullopt;
    }

    if (!wasShowing_) {
        return std::nullopt;
    }

    wasShowing_ = false;

    return PauseMeasurement{
        .frames = framesWhileShowing_,
        .gameTime = invokeNative<std::int32_t>(gameTimer_) - openedAtGameTime_,
    };
}

} // namespace oxymp::client::game
