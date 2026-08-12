#pragma once

#include "http_server.hpp"
#include "player_registry.hpp"
#include "resource_store.hpp"

#include <oxymp/net/host.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace oxymp::server {

struct Config {
    std::uint16_t port = shared::kDefaultServerPort;
    std::size_t maxPlayers = 32;
    std::string name = "oxyMP";

    /// Кому позволено распоряжаться из админ-меню.
    ///
    /// По умолчанию — нулевому месту, то есть тому, кто вошёл первым. Обычно это
    /// хозяин сервера, который его и запустил.
    ///
    /// Проверка здесь не от взломщиков: пакет умеет собрать кто угодно. Она от
    /// обычного случая — чтобы гость, зашедший в сессию, не мог собрать к себе
    /// всех остальных.
    std::vector<shared::PlayerId> admins{0};

    /// С чего игрок начинает.
    ///
    /// Своя сумма, к настоящему GTA Online отношения не имеющая. Не ноль, чтобы
    /// в сессии было что тратить с первой минуты.
    std::int64_t startingMoney = 100'000;

    /// Откуда брать файлы, которые сервер раздаёт клиентам.
    ///
    /// Путь относительный: сервер запускают из своего каталога, и требовать
    /// от хозяина писать полный путь незачем.
    std::filesystem::path resourceDirectory = "resources/dlcpacks";
};

/// Сервер: владеет сетевым узлом и списком игроков.
///
/// Единственный владелец своих подсистем — они создаются вместе с ним и живут
/// ровно столько же.
class Server {
public:
    [[nodiscard]] static std::unique_ptr<Server> start(const Config& config, std::string& error);

    /// Крутит цикл обслуживания, пока флаг не станет true.
    ///
    /// Флаг атомарный, потому что поднимает его обработчик сигнала — то есть
    /// он приходит извне обычного хода выполнения.
    void run(const std::atomic<bool>& stopRequested);

private:
    Server() = default;

    void handleConnected(net::PeerId peer);
    void handleDisconnected(net::PeerId peer);
    void handleMessage(net::PeerId peer, const std::vector<std::uint8_t>& payload);

    void handleHello(net::PeerId peer, const shared::ClientHello& hello);
    void handlePing(net::PeerId peer, const shared::Ping& ping);
    void handlePlayerState(net::PeerId peer, shared::PlayerState state);
    void handleVehicleState(net::PeerId peer, shared::VehicleState state);
    void handleChatSay(net::PeerId peer, const shared::ChatSay& say);
    void handleDamageReport(net::PeerId peer, const shared::DamageReport& report);
    void handleAdminAction(net::PeerId peer, const shared::AdminAction& action);

    /// Рассылает строку чата всем и записывает её в журнал сервера.
    void announce(shared::ChatKind kind, shared::PlayerId author, std::string nickname,
                  std::string text);

    /// Отправляет отказ и закрывает соединение.
    void reject(net::PeerId peer, shared::RejectReason reason);

    template<typename Message>
    void sendTo(net::PeerId peer, const Message& message) {
        const auto packet = shared::encode(message);
        host_->send(peer, shared::Channel::Control, shared::ByteView{packet});
    }

    template<typename Message>
    void broadcast(const Message& message, net::PeerId except = net::kInvalidPeerId) {
        const auto packet = shared::encode(message);
        host_->broadcast(shared::Channel::Control, shared::ByteView{packet}, except);
    }

    /// Разрывает соединения, которые подключились, но так и не представились.
    void dropSilentPeers();

    Config config_;
    std::unique_ptr<net::Host> host_;
    PlayerRegistry players_;

    /// Что сервер раздаёт клиентам сверх самой игры.
    ///
    /// Объявлен раньше раздачи и потому переживает её: раздача держит на него
    /// ссылку и читает из своего потока.
    ResourceStore resources_;

    std::unique_ptr<HttpServer> http_;

    /// Когда соединение подключилось. Запись живёт до рукопожатия: молчащий
    /// клиент иначе занимал бы место в лимите игроков бесконечно.
    std::unordered_map<net::PeerId, std::chrono::steady_clock::time_point> awaitingHello_;
};

} // namespace oxymp::server
