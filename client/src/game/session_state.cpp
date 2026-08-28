#include "session_state.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {

SessionState::SessionState(const EngineAddresses& addresses, const NativeTable& table) noexcept
    : started_(addresses.pointerTo<StartedFunction>("session_started")),
      active_(table.handlerFor(natives::kNetworkIsSessionActive)) {}

bool SessionState::started() const {
    if (started_ == nullptr) {
        return false;
    }

    return started_();
}

bool SessionState::active() const {
    // Натив, а не своя сигнатура: спрашивается он из кадра игры, где нативы и
    // так зовутся, а лишняя сигнатура — это лишнее место, которое ломается при
    // всяком обновлении игры.
    //
    // Нет натива — отвечаем «начата ли», а не «нет»: прежде судили по одному
    // `started`, и вернуться к прежнему поведению честнее, чем объявить сессию
    // мёртвой из-за ненайденного натива.
    if (active_ == nullptr) {
        return started();
    }

    return invokeNative<bool>(active_);
}

} // namespace oxymp::client::game
