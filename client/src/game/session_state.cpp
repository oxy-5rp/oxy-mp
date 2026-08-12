#include "session_state.hpp"

namespace oxymp::client::game {

SessionState::SessionState(const EngineAddresses& addresses) noexcept
    : started_(addresses.pointerTo<StartedFunction>("session_started")) {}

bool SessionState::started() const {
    if (started_ == nullptr) {
        return false;
    }

    return started_();
}

} // namespace oxymp::client::game
