#pragma once

#include <oxymp/net/host.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace oxymp::client {

enum class ConnectionState {
    /// Соединения нет, идёт отсчёт до следующей попытки.
    Waiting,

    /// Транспорт устанавливает соединение.
    Connecting,

    /// Соединение есть, отправлено представление, ждём ответа сервера.
    Handshaking,

    /// Сервер принял: игрок в сессии.
    Connected,

    /// Сервер отказал. Попытки прекращены: причина отказа сама не изменится.
    Rejected,
};

/// Другой игрок, о котором сообщил сервер.
struct RemotePlayer {
    shared::PlayerId id = shared::kInvalidPlayerId;
    std::string nickname;
};

/// Соединение с сервером и состояние сессии.
///
/// Ничего не знает ни про игру, ни про то, откуда его вызывают: одинаково
/// работает внутри процесса игры и в консольном клиенте для отладки. Благодаря
/// этому сеть отлаживается без запуска GTA.
///
/// Не потокобезопасен: предполагается, что update и чтение состояния происходят
/// в одном потоке.
class Connection {
public:
    struct Settings {
        std::string address = "127.0.0.1";
        std::uint16_t port = shared::kDefaultServerPort;
        std::string nickname = "player";
    };

    explicit Connection(Settings settings);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// Обрабатывает сеть и таймеры.
    ///
    /// budget — сколько времени разрешено ждать событий. Вызывать нужно
    /// постоянно: на этом же вызове транспорт обслуживает подтверждения,
    /// повторные отправки и разрывы по таймауту.
    void update(std::chrono::milliseconds budget);

    /// Просит закрыть соединение и больше не подключаться.
    void disconnect();

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }

    [[nodiscard]] shared::PlayerId localPlayerId() const noexcept { return localPlayerId_; }

    /// Время оборота до сервера. Пусто, пока не получен первый ответ.
    [[nodiscard]] std::optional<std::chrono::milliseconds> latency() const noexcept {
        return latency_;
    }

    /// Причина отказа, если сервер отказал.
    [[nodiscard]] std::optional<shared::RejectReason> rejectReason() const noexcept {
        return rejectReason_;
    }

    [[nodiscard]] const std::unordered_map<shared::PlayerId, RemotePlayer>& remotePlayers()
        const noexcept {
        return remotePlayers_;
    }

private:
    using Clock = std::chrono::steady_clock;

    void beginAttempt();
    void handleEvent(const net::Event& event);
    void handleMessage(const std::vector<std::uint8_t>& payload);
    void handleWelcome(const shared::ServerWelcome& welcome);
    void handleReject(const shared::ServerReject& reject);
    void handlePong(const shared::Pong& pong);

    void sendHello();
    void sendPingIfDue();

    /// Сбрасывает состояние сессии и назначает следующую попытку.
    void fallBackToWaiting(std::string_view reason);

    Settings settings_;

    std::unique_ptr<net::Host> host_;
    net::PeerId serverPeer_ = net::kInvalidPeerId;

    ConnectionState state_ = ConnectionState::Waiting;
    shared::PlayerId localPlayerId_ = shared::kInvalidPlayerId;
    std::optional<shared::RejectReason> rejectReason_;
    std::unordered_map<shared::PlayerId, RemotePlayer> remotePlayers_;

    Clock::time_point nextAttemptAt_ = Clock::now();
    Clock::time_point nextPingAt_ = Clock::time_point::max();

    /// Задержка до следующей попытки. Растёт при неудачах, чтобы клиент не
    /// долбился в выключенный сервер по десять раз в секунду.
    std::chrono::milliseconds retryDelay_{500};

    std::optional<std::chrono::milliseconds> latency_;
    bool stopped_ = false;
};

/// Человекочитаемое описание причины отказа.
[[nodiscard]] std::string_view describe(shared::RejectReason reason) noexcept;

/// Человекочитаемое описание состояния соединения.
[[nodiscard]] std::string_view describe(ConnectionState state) noexcept;

} // namespace oxymp::client
