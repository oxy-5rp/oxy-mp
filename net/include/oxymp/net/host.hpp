#pragma once

#include <oxymp/net/event.hpp>
#include <oxymp/shared/protocol/serialization.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <optional>
#include <string>
#include <unordered_map>

struct _ENetHost;
struct _ENetPeer;

namespace oxymp::net {

/// Сколько соединений транспорт согласен держать разом.
///
/// Не наш выбор и не настройка: номер соединения ходит в заголовке пакета
/// двенадцатью битами, и потолок этот заложен в сам протокол ENet. Названо
/// здесь потому, что спрашивают его снаружи — сервер обязан сказать хозяину
/// внятно, что мест больше не бывает, а не отказать общим «не удалось занять
/// порт»: узел с числом соединений сверх потолка не создаётся вовсе, и отличить
/// это от занятого порта по отказу нельзя.
inline constexpr std::size_t kMaxPeers = 4095;

/// Сетевой узел: слушающий сервер либо подключающийся клиент.
///
/// Единственное место в проекте, которое знает про используемую библиотеку сети.
/// Всё остальное работает с PeerId, Event и диапазонами байт.
class Host {
public:
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    /// Поднимает сервер на указанном порту.
    [[nodiscard]] static std::unique_ptr<Host> listen(std::uint16_t port, std::size_t maxPeers,
                                                      std::string& error);

    /// Создаёт клиента и начинает подключение.
    ///
    /// Возврат управления не означает, что соединение установлено: об этом
    /// сообщит событие Connected. Отказ приходит как Disconnected.
    [[nodiscard]] static std::unique_ptr<Host> connect(const std::string& address,
                                                       std::uint16_t port, std::string& error);

    /// Ждёт события не дольше указанного времени.
    ///
    /// Возвращает nullopt, если за это время ничего не произошло. Вызывать нужно
    /// постоянно: на этом же вызове транспорт обслуживает подтверждения и
    /// повторные отправки.
    [[nodiscard]] std::optional<Event> poll(std::chrono::milliseconds timeout);

    /// Отправляет пакет одному получателю.
    ///
    /// Надёжность выбирается каналом, а не отдельным параметром: управляющие
    /// сообщения обязаны дойти, снимки состояния — нет. Держать это правило в
    /// одном месте надёжнее, чем вспоминать его на каждой отправке.
    void send(PeerId peer, shared::Channel channel, shared::ByteView payload);

    /// Отправляет пакет всем соединениям, кроме указанного.
    ///
    /// kInvalidPeerId в качестве исключения означает «отправить всем».
    void broadcast(shared::Channel channel, shared::ByteView payload,
                   PeerId except = kInvalidPeerId);

    /// Просит закрыть соединение. Событие Disconnected придёт позже.
    void disconnect(PeerId peer);

    /// Немедленно выталкивает накопленные пакеты.
    ///
    /// Нужен перед завершением работы: иначе последнее отправленное сообщение
    /// может не успеть уйти.
    void flush();

    [[nodiscard]] std::size_t peerCount() const noexcept { return peers_.size(); }

    /// Сколько посылок и байт ушло с прошлого раза, считая с обнулением.
    ///
    /// Считает не транспорт, а мы сами, и считает именно то, что отдали ему:
    /// заголовки UDP и ENet сюда не входят. Для того, ради чего счёт нужен —
    /// понять, во что обходится сессия и что даёт складывание снимков в один
    /// пакет, — важен не абсолютный байт, а отношение до и после.
    struct Traffic {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
    };

    [[nodiscard]] Traffic takeTraffic() noexcept {
        return {std::exchange(sentPackets_, 0), std::exchange(sentBytes_, 0)};
    }

private:
    Host() = default;

    /// Сколько посылок и байт отдано транспорту с прошлого замера.
    std::uint64_t sentPackets_ = 0;
    std::uint64_t sentBytes_ = 0;

    [[nodiscard]] PeerId registerPeer(_ENetPeer* peer);
    void forgetPeer(_ENetPeer* peer);
    [[nodiscard]] _ENetPeer* find(PeerId peer) const;

    _ENetHost* host_ = nullptr;
    std::unordered_map<PeerId, _ENetPeer*> peers_;
    PeerId nextPeerId_ = 1;
};

} // namespace oxymp::net
