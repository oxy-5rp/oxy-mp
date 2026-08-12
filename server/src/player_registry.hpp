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

    /// Деньги игрока в нашей сессии.
    ///
    /// Наши собственные, не имеющие отношения к сохранению GTA Online. Игра
    /// показывает деньги оттуда, и это чужие деньги: они принадлежат игроку и
    /// его покупкам в настоящей игре, а не нам. Мы их не читаем и не пишем — мы
    /// прячем их счётчик и показываем свой.
    ///
    /// Живут пока живёт соединение: сервер ничего не сохраняет на диск, и
    /// притворяться, что сохраняет, было бы хуже, чем не уметь.
    std::int64_t money = 0;
};

/// Список игроков на сервере.
///
/// Соединение и игрок — разные сущности с разным временем жизни, поэтому
/// идентификаторы у них тоже разные, а связь между ними хранится здесь.
class PlayerRegistry {
public:
    using Storage = std::unordered_map<net::PeerId, Player>;

    /// Заводит игрока и выдаёт ему идентификатор.
    ///
    /// Идентификатор — наименьшее свободное место, а не следующее по счёту:
    /// первый вошедший получает ноль, а место ушедшего достаётся следующему.
    const Player& add(net::PeerId peer, std::string nickname, std::int64_t money);

    /// Удаляет игрока по соединению. Возвращает его данные, если он был.
    std::optional<Player> removeByPeer(net::PeerId peer);

    [[nodiscard]] const Player* findByPeer(net::PeerId peer) const;

    /// Находит игрока по выданному идентификатору.
    ///
    /// Нужно для сообщений, адресованных игроку, а не соединению: урон приходит
    /// от того, кто попал, и назван в нём не сосед по сети, а жертва.
    [[nodiscard]] const Player* findById(shared::PlayerId id) const;

    [[nodiscard]] bool nicknameTaken(std::string_view nickname) const;

    [[nodiscard]] std::size_t size() const noexcept { return players_.size(); }

    [[nodiscard]] Storage::const_iterator begin() const noexcept { return players_.begin(); }
    [[nodiscard]] Storage::const_iterator end() const noexcept { return players_.end(); }

private:
    /// Наименьший незанятый идентификатор.
    [[nodiscard]] shared::PlayerId freeId() const;

    Storage players_;
};

} // namespace oxymp::server
