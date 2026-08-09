#pragma once

#include <oxymp/net/event.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace oxymp::server {

/// Игрок, прошедший рукопожатие.
///
/// Соединения, которые ещё не представились, игроками не считаются и здесь
/// не хранятся: до рукопожатия о подключившемся неизвестно ничего.
struct Player {
    shared::PlayerId id = shared::kInvalidPlayerId;
    net::PeerId peer = net::kInvalidPeerId;
    std::string nickname;
};

/// Список игроков на сервере.
///
/// Соединение и игрок — разные сущности с разным временем жизни, поэтому
/// идентификаторы у них тоже разные, а связь между ними хранится здесь.
class PlayerRegistry {
public:
    using Storage = std::unordered_map<net::PeerId, Player>;

    /// Заводит игрока и выдаёт ему идентификатор.
    const Player& add(net::PeerId peer, std::string nickname);

    /// Удаляет игрока по соединению. Возвращает его данные, если он был.
    std::optional<Player> removeByPeer(net::PeerId peer);

    [[nodiscard]] const Player* findByPeer(net::PeerId peer) const;

    [[nodiscard]] bool nicknameTaken(std::string_view nickname) const;

    [[nodiscard]] std::size_t size() const noexcept { return players_.size(); }

    [[nodiscard]] Storage::const_iterator begin() const noexcept { return players_.begin(); }
    [[nodiscard]] Storage::const_iterator end() const noexcept { return players_.end(); }

private:
    Storage players_;
    shared::PlayerId nextPlayerId_ = 1;
};

} // namespace oxymp::server
