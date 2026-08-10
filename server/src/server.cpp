#include "server.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace oxymp::server {
namespace {

/// Сколько ждать рукопожатия от подключившегося соединения.
constexpr auto kHelloTimeout = std::chrono::seconds{10};

/// Шаг цикла обслуживания. Заодно это максимальное ожидание события,
/// поэтому сервер не «спит» дольше одного такта.
constexpr auto kTickInterval = std::chrono::milliseconds{1000 / shared::kDefaultTickRate};

std::string_view describe(shared::RejectReason reason) {
    switch (reason) {
    case shared::RejectReason::ProtocolMismatch:
        return "версия протокола не совпадает";
    case shared::RejectReason::ServerFull:
        return "сервер заполнен";
    case shared::RejectReason::InvalidNickname:
        return "недопустимое имя";
    }
    return "причина не указана";
}

bool nicknameLooksValid(const std::string& nickname) {
    if (nickname.empty() || nickname.size() > shared::kMaxNicknameLength) {
        return false;
    }

    // Управляющие символы в имени ломают вывод и ничего не добавляют.
    return std::ranges::none_of(nickname, [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

} // namespace

std::unique_ptr<Server> Server::start(const Config& config, std::string& error) {
    auto host = net::Host::listen(config.port, config.maxPlayers, error);
    if (!host) {
        return nullptr;
    }

    std::unique_ptr<Server> server{new Server};
    server->config_ = config;
    server->host_ = std::move(host);

    spdlog::info("сервер \"{}\" слушает порт {}, мест: {}", config.name, config.port,
                 config.maxPlayers);

    return server;
}

void Server::run(const std::atomic<bool>& stopRequested) {
    while (!stopRequested.load()) {
        while (auto event = host_->poll(kTickInterval)) {
            switch (event->type) {
            case net::Event::Type::Connected:
                handleConnected(event->peer);
                break;
            case net::Event::Type::Disconnected:
                handleDisconnected(event->peer);
                break;
            case net::Event::Type::Message:
                handleMessage(event->peer, event->payload);
                break;
            }
        }

        dropSilentPeers();
    }

    spdlog::info("остановка, игроков было: {}", players_.size());
    host_->flush();
}

void Server::handleConnected(net::PeerId peer) {
    spdlog::debug("соединение {} установлено, ждём представления", peer);
    awaitingHello_.emplace(peer, std::chrono::steady_clock::now());
}

void Server::handleDisconnected(net::PeerId peer) {
    awaitingHello_.erase(peer);

    const auto player = players_.removeByPeer(peer);
    if (!player) {
        spdlog::debug("соединение {} закрыто до представления", peer);
        return;
    }

    spdlog::info("игрок \"{}\" (id {}) отключился, осталось: {}", player->nickname, player->id,
                 players_.size());

    shared::PlayerLeft left;
    left.playerId = player->id;
    broadcast(left);
}

void Server::handleMessage(net::PeerId peer, const std::vector<std::uint8_t>& payload) {
    const shared::ByteView packet{payload};

    const auto id = shared::peekMessageId(packet);
    if (!id) {
        spdlog::warn("соединение {} прислало нераспознанный пакет ({} байт)", peer, payload.size());
        return;
    }

    switch (*id) {
    case shared::MessageId::ClientHello:
        if (const auto hello = shared::decode<shared::ClientHello>(packet)) {
            handleHello(peer, *hello);
        }
        return;

    case shared::MessageId::Ping:
        if (const auto ping = shared::decode<shared::Ping>(packet)) {
            handlePing(peer, *ping);
        }
        return;

    case shared::MessageId::PlayerState:
        if (const auto state = shared::decode<shared::PlayerState>(packet)) {
            handlePlayerState(peer, *state);
        }
        return;

    // Эти сообщения посылает сервер, а не клиент. Получить их обратно означает
    // либо ошибку в клиенте, либо попытку что-то подделать.
    case shared::MessageId::ServerWelcome:
    case shared::MessageId::ServerReject:
    case shared::MessageId::Pong:
    case shared::MessageId::PlayerJoined:
    case shared::MessageId::PlayerLeft:
        spdlog::warn("соединение {} прислало серверное сообщение", peer);
        return;
    }
}

void Server::handleHello(net::PeerId peer, const shared::ClientHello& hello) {
    if (players_.findByPeer(peer) != nullptr) {
        spdlog::warn("соединение {} представилось повторно", peer);
        return;
    }

    if (hello.protocolVersion != shared::kProtocolVersion) {
        spdlog::info("соединение {} отклонено: протокол {} против нашего {}", peer,
                     hello.protocolVersion, shared::kProtocolVersion);
        reject(peer, shared::RejectReason::ProtocolMismatch);
        return;
    }

    if (!nicknameLooksValid(hello.nickname) || players_.nicknameTaken(hello.nickname)) {
        reject(peer, shared::RejectReason::InvalidNickname);
        return;
    }

    if (players_.size() >= config_.maxPlayers) {
        reject(peer, shared::RejectReason::ServerFull);
        return;
    }

    awaitingHello_.erase(peer);

    // О новичке нужно рассказать остальным, а ему — про остальных. Порядок важен:
    // список существующих собирается до того, как он сам попадёт в реестр.
    std::vector<shared::PlayerJoined> existing;
    existing.reserve(players_.size());
    for (const auto& [otherPeer, other] : players_) {
        shared::PlayerJoined joined;
        joined.playerId = other.id;
        joined.nickname = other.nickname;
        existing.push_back(std::move(joined));
    }

    const Player& player = players_.add(peer, hello.nickname);

    shared::ServerWelcome welcome;
    welcome.playerId = player.id;
    welcome.tickRate = shared::kDefaultTickRate;
    sendTo(peer, welcome);

    for (const auto& joined : existing) {
        sendTo(peer, joined);
    }

    shared::PlayerJoined announcement;
    announcement.playerId = player.id;
    announcement.nickname = player.nickname;
    broadcast(announcement, peer);

    spdlog::info("игрок \"{}\" (id {}) подключился, всего: {}", player.nickname, player.id,
                 players_.size());
}

void Server::handlePing(net::PeerId peer, const shared::Ping& ping) {
    shared::Pong pong;
    pong.timestampMs = ping.timestampMs;

    const auto packet = shared::encode(pong);
    host_->send(peer, shared::Channel::State, shared::ByteView{packet});
}

void Server::handlePlayerState(net::PeerId peer, shared::PlayerState state) {
    const Player* player = players_.findByPeer(peer);
    if (player == nullptr) {
        // Состояние до рукопожатия рассылать некому и незачем.
        return;
    }

    // Идентификатор проставляет сервер, а не клиент. Иначе достаточно было бы
    // подменить одно поле, чтобы двигать чужого игрока.
    state.playerId = player->id;

    const auto packet = shared::encode(state);
    host_->broadcast(shared::Channel::State, shared::ByteView{packet}, peer);
}

void Server::reject(net::PeerId peer, shared::RejectReason reason) {
    spdlog::info("соединение {} отклонено: {}", peer, describe(reason));

    shared::ServerReject message;
    message.reason = reason;
    sendTo(peer, message);

    // Отказ нужно вытолкнуть до разрыва, иначе клиент увидит молчаливое
    // закрытие и не поймёт причины.
    host_->flush();
    host_->disconnect(peer);
}

void Server::dropSilentPeers() {
    const auto now = std::chrono::steady_clock::now();

    for (auto it = awaitingHello_.begin(); it != awaitingHello_.end();) {
        if (now - it->second < kHelloTimeout) {
            ++it;
            continue;
        }

        spdlog::info("соединение {} не представилось за отведённое время", it->first);
        host_->disconnect(it->first);
        it = awaitingHello_.erase(it);
    }
}

} // namespace oxymp::server
