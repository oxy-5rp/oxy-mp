#include "player_registry.hpp"

#include <algorithm>

namespace oxymp::server {

const Player& PlayerRegistry::add(net::PeerId peer, std::string nickname) {
    Player player;
    player.id = nextPlayerId_++;
    player.peer = peer;
    player.nickname = std::move(nickname);

    return players_.insert_or_assign(peer, std::move(player)).first->second;
}

std::optional<Player> PlayerRegistry::removeByPeer(net::PeerId peer) {
    const auto it = players_.find(peer);
    if (it == players_.end()) {
        return std::nullopt;
    }

    Player removed = std::move(it->second);
    players_.erase(it);

    return removed;
}

const Player* PlayerRegistry::findByPeer(net::PeerId peer) const {
    const auto it = players_.find(peer);
    return it == players_.end() ? nullptr : &it->second;
}

bool PlayerRegistry::nicknameTaken(std::string_view nickname) const {
    return std::ranges::any_of(players_, [nickname](const auto& entry) {
        return entry.second.nickname == nickname;
    });
}

} // namespace oxymp::server
