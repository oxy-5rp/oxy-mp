#include "player_registry.hpp"

#include <algorithm>

namespace oxymp::server {

shared::PlayerId PlayerRegistry::freeId() const {
    // Перебором с нуля, а не счётчиком: список короток — мест на сервере
    // десятки, — а взамен идентификаторы остаются маленькими и повторно
    // используемыми. Игрок называет своё число в чате, и оно не должно расти до
    // бесконечности оттого, что кто-то весь вечер переподключался.
    for (shared::PlayerId candidate = 0;; ++candidate) {
        const bool taken = std::ranges::any_of(players_, [candidate](const auto& entry) {
            return entry.second.id == candidate;
        });

        if (!taken) {
            return candidate;
        }
    }
}

const Player& PlayerRegistry::add(net::PeerId peer, std::string nickname, std::int64_t money) {
    Player player;
    player.id = freeId();
    player.peer = peer;
    player.nickname = std::move(nickname);
    player.money = money;

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

Player* PlayerRegistry::findByPeer(net::PeerId peer) {
    const auto it = players_.find(peer);
    return it == players_.end() ? nullptr : &it->second;
}

const Player* PlayerRegistry::findById(shared::PlayerId id) const {
    const auto it = std::ranges::find_if(players_, [id](const auto& entry) {
        return entry.second.id == id;
    });

    return it == players_.end() ? nullptr : &it->second;
}

Player* PlayerRegistry::findById(shared::PlayerId id) {
    const auto it = std::ranges::find_if(players_, [id](const auto& entry) {
        return entry.second.id == id;
    });

    return it == players_.end() ? nullptr : &it->second;
}

bool PlayerRegistry::nicknameTaken(std::string_view nickname) const {
    return std::ranges::any_of(players_, [nickname](const auto& entry) {
        return entry.second.nickname == nickname;
    });
}

} // namespace oxymp::server
