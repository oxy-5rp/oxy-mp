#include <oxymp/client/connection.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace oxymp::client {
namespace {

/// Как часто отправляется проверка связи.
constexpr auto kPingInterval = std::chrono::seconds{2};

/// Как часто уходит снимок своего состояния.
///
/// Двадцать раз в секунду: чаще — лишний трафик, реже — интерполяция начинает
/// заметно отставать от настоящего движения.
constexpr auto kStateInterval = std::chrono::milliseconds{50};

/// Насколько далеко разрешено достраивать движение за последним снимком.
///
/// Без ограничения потерянная связь уводила бы модель игрока в бесконечность.
constexpr auto kMaxExtrapolation = std::chrono::milliseconds{250};

/// На сколько чужие игроки показываются позади настоящего времени.
///
/// Плата за плавность: столько времени нужно, чтобы в запасе всегда была пара
/// снимков, между которыми можно считать положение. Примерно два интервала
/// отправки — этого хватает, чтобы пережить одиночную потерю пакета.
constexpr auto kInterpolationDelay = std::chrono::milliseconds{100};

/// Границы задержки между попытками подключения.
constexpr auto kMinRetryDelay = std::chrono::milliseconds{500};
constexpr auto kMaxRetryDelay = std::chrono::seconds{10};

/// Отметка времени для проверки связи.
///
/// Смысл имеет только для нас: сервер возвращает её как есть, поэтому сверять
/// часы сторон не требуется.
std::uint64_t nowMilliseconds() {
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

} // namespace

std::string_view describe(shared::RejectReason reason) noexcept {
    switch (reason) {
    case shared::RejectReason::ProtocolMismatch:
        return "версия протокола не совпадает с серверной";
    case shared::RejectReason::ServerFull:
        return "на сервере нет свободных мест";
    case shared::RejectReason::InvalidNickname:
        return "имя недопустимо или уже занято";
    }
    return "причина не указана";
}

std::string_view describe(ConnectionState state) noexcept {
    switch (state) {
    case ConnectionState::Waiting:
        return "ожидание";
    case ConnectionState::Connecting:
        return "подключение";
    case ConnectionState::Handshaking:
        return "представление";
    case ConnectionState::Connected:
        return "в сессии";
    case ConnectionState::Rejected:
        return "отказано";
    }
    return "?";
}

shared::Vec3 RemotePlayer::interpolatedPosition(std::chrono::steady_clock::time_point now) const {
    if (!hasState) {
        return {};
    }

    // Рисуем чужих игроков с небольшой задержкой относительно настоящего времени.
    //
    // Это принципиальный момент: если показывать самый свежий снимок, между
    // снимками показывать будет нечего, и движение превратится в рывки. Отставая
    // на kInterpolationDelay, мы почти всегда оказываемся между двумя уже
    // полученными снимками и можем считать положение между ними.
    const auto renderAt = now - kInterpolationDelay;

    const auto span = std::chrono::duration<float>{latestAt - previousAt}.count();
    const auto ahead = std::chrono::duration<float>{renderAt - latestAt}.count();

    if (ahead <= 0.0F) {
        if (span <= 0.0F) {
            // Снимок пока один — интерполировать не между чем.
            return latest.position;
        }

        const float progress = 1.0F + ahead / span;
        const float clamped = std::clamp(progress, 0.0F, 1.0F);

        return shared::Vec3{
            std::lerp(previous.position.x, latest.position.x, clamped),
            std::lerp(previous.position.y, latest.position.y, clamped),
            std::lerp(previous.position.z, latest.position.z, clamped),
        };
    }

    // Свежих снимков нет дольше задержки: достраиваем движение по последней
    // известной скорости, но не бесконечно — иначе потеря связи унесёт игрока.
    const float limit = std::chrono::duration<float>{kMaxExtrapolation}.count();
    const float elapsed = std::min(ahead, limit);

    return shared::Vec3{
        latest.position.x + latest.velocity.x * elapsed,
        latest.position.y + latest.velocity.y * elapsed,
        latest.position.z + latest.velocity.z * elapsed,
    };
}

Connection::Connection(Settings settings) : settings_(std::move(settings)) {}

Connection::~Connection() = default;

void Connection::update(std::chrono::milliseconds budget) {
    if (stopped_ || state_ == ConnectionState::Rejected) {
        return;
    }

    if (host_ == nullptr) {
        if (Clock::now() >= nextAttemptAt_) {
            beginAttempt();
        }
        return;
    }

    while (auto event = host_->poll(budget)) {
        handleEvent(*event);

        if (host_ == nullptr) {
            return;
        }

        // Оставшиеся события забираем без ожидания: время уже потрачено.
        budget = std::chrono::milliseconds{0};
    }

    sendPingIfDue();
    sendStateIfDue();
}

void Connection::beginAttempt() {
    std::string error;
    host_ = net::Host::connect(settings_.address, settings_.port, error);

    if (host_ == nullptr) {
        spdlog::warn("не удалось начать подключение к {}:{}: {}", settings_.address,
                     settings_.port, error);
        fallBackToWaiting("подключение не началось");
        return;
    }

    state_ = ConnectionState::Connecting;
    spdlog::info("подключение к {}:{}", settings_.address, settings_.port);
}

void Connection::handleEvent(const net::Event& event) {
    switch (event.type) {
    case net::Event::Type::Connected:
        serverPeer_ = event.peer;
        state_ = ConnectionState::Handshaking;
        sendHello();
        return;

    case net::Event::Type::Disconnected:
        // Разрыв на этапе представления почти всегда означает отказ сервера,
        // но точную причину мы уже могли получить отдельным сообщением.
        fallBackToWaiting(state_ == ConnectionState::Connecting ? "сервер недоступен"
                                                                : "соединение потеряно");
        return;

    case net::Event::Type::Message:
        handleMessage(event.payload);
        return;
    }
}

void Connection::handleMessage(const std::vector<std::uint8_t>& payload) {
    const shared::ByteView packet{payload};

    const auto id = shared::peekMessageId(packet);
    if (!id) {
        spdlog::warn("сервер прислал нераспознанный пакет ({} байт)", payload.size());
        return;
    }

    switch (*id) {
    case shared::MessageId::ServerWelcome:
        if (const auto welcome = shared::decode<shared::ServerWelcome>(packet)) {
            handleWelcome(*welcome);
        }
        return;

    case shared::MessageId::ServerReject:
        if (const auto reject = shared::decode<shared::ServerReject>(packet)) {
            handleReject(*reject);
        }
        return;

    case shared::MessageId::Pong:
        if (const auto pong = shared::decode<shared::Pong>(packet)) {
            handlePong(*pong);
        }
        return;

    case shared::MessageId::PlayerJoined:
        if (const auto joined = shared::decode<shared::PlayerJoined>(packet)) {
            // Именно обновление полей, а не замена записи: снимок состояния мог
            // прийти раньше объявления о входе, и затирать его нельзя.
            RemotePlayer& player = remotePlayers_[joined->playerId];
            player.id = joined->playerId;
            player.nickname = joined->nickname;

            spdlog::info("в сессии появился игрок \"{}\" (id {})", joined->nickname,
                         joined->playerId);
        }
        return;

    case shared::MessageId::PlayerLeft:
        if (const auto left = shared::decode<shared::PlayerLeft>(packet)) {
            remotePlayers_.erase(left->playerId);
            spdlog::info("игрок id {} вышел", left->playerId);
        }
        return;

    case shared::MessageId::PlayerState:
        if (const auto state = shared::decode<shared::PlayerState>(packet)) {
            handleRemoteState(*state);
        }
        return;

    // Это сообщения клиента. Сервер их не присылает.
    case shared::MessageId::ClientHello:
    case shared::MessageId::Ping:
        spdlog::warn("сервер прислал клиентское сообщение");
        return;
    }
}

void Connection::handleWelcome(const shared::ServerWelcome& welcome) {
    localPlayerId_ = welcome.playerId;
    state_ = ConnectionState::Connected;

    // Подключение удалось — следующая неудача должна начинать отсчёт заново,
    // а не с накопленной задержки.
    retryDelay_ = kMinRetryDelay;
    nextPingAt_ = Clock::now();
    nextStateAt_ = Clock::now();

    spdlog::info("сервер принял: наш id {}, темп {} тактов в секунду", welcome.playerId,
                 welcome.tickRate);
}

void Connection::handleReject(const shared::ServerReject& reject) {
    rejectReason_ = reject.reason;
    state_ = ConnectionState::Rejected;

    spdlog::error("сервер отказал: {}", describe(reject.reason));
}

void Connection::handlePong(const shared::Pong& pong) {
    const std::uint64_t now = nowMilliseconds();
    if (now < pong.timestampMs) {
        return;
    }

    latency_ = std::chrono::milliseconds{now - pong.timestampMs};
}

void Connection::handleRemoteState(const shared::PlayerState& state) {
    if (state.playerId == localPlayerId_ || state.playerId == shared::kInvalidPlayerId) {
        return;
    }

    // Снимок может прийти раньше объявления о входе — например если пакеты
    // разошлись. Заводим игрока молча: имя придёт следующим PlayerJoined.
    RemotePlayer& player = remotePlayers_[state.playerId];
    player.id = state.playerId;

    const auto now = Clock::now();

    if (player.hasState) {
        player.previous = player.latest;
        player.previousAt = player.latestAt;
    } else {
        player.previous = state;
        player.previousAt = now;
        player.hasState = true;
    }

    player.latest = state;
    player.latestAt = now;
}

void Connection::setLocalState(const shared::PlayerState& state) {
    localState_ = state;
    localState_.playerId = shared::kInvalidPlayerId;
}

void Connection::sendHello() {
    shared::ClientHello hello;
    hello.protocolVersion = shared::kProtocolVersion;
    hello.nickname = settings_.nickname;

    const auto packet = shared::encode(hello);
    host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
    host_->flush();
}

void Connection::sendPingIfDue() {
    if (state_ != ConnectionState::Connected || Clock::now() < nextPingAt_) {
        return;
    }

    shared::Ping ping;
    ping.timestampMs = nowMilliseconds();

    const auto packet = shared::encode(ping);
    host_->send(serverPeer_, shared::Channel::State, shared::ByteView{packet});

    nextPingAt_ = Clock::now() + kPingInterval;
}

void Connection::sendStateIfDue() {
    if (state_ != ConnectionState::Connected || Clock::now() < nextStateAt_) {
        return;
    }

    const auto packet = shared::encode(localState_);
    host_->send(serverPeer_, shared::Channel::State, shared::ByteView{packet});

    nextStateAt_ = Clock::now() + kStateInterval;
}

void Connection::fallBackToWaiting(std::string_view reason) {
    if (state_ == ConnectionState::Rejected) {
        return;
    }

    spdlog::warn("{}, повтор через {} мс", reason, retryDelay_.count());

    host_.reset();
    serverPeer_ = net::kInvalidPeerId;
    localPlayerId_ = shared::kInvalidPlayerId;
    remotePlayers_.clear();
    latency_.reset();
    nextPingAt_ = Clock::time_point::max();
    nextStateAt_ = Clock::time_point::max();

    state_ = ConnectionState::Waiting;
    nextAttemptAt_ = Clock::now() + retryDelay_;

    // Удвоение с потолком: сервер может быть просто ещё не запущен, и заваливать
    // его попытками бессмысленно.
    retryDelay_ = std::min(retryDelay_ * 2, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                kMaxRetryDelay));
}

void Connection::disconnect() {
    stopped_ = true;

    if (host_ != nullptr && serverPeer_ != net::kInvalidPeerId) {
        host_->disconnect(serverPeer_);
        host_->flush();
    }

    host_.reset();
    serverPeer_ = net::kInvalidPeerId;
    state_ = ConnectionState::Waiting;
}

} // namespace oxymp::client
