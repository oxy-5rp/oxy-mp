#include <oxymp/net/host.hpp>

#include <enet/enet.h>

#include <cstdlib>
#include <mutex>

namespace oxymp::net {
namespace {

/// Инициализация транспорта, ровно один раз на процесс.
///
/// Освобождение регистрируется через atexit: библиотека живёт столько же,
/// сколько процесс, и отдельного владельца у неё нет.
bool ensureTransportReady(std::string& error) {
    static bool ready = false;
    static std::once_flag once;

    std::call_once(once, [] {
        if (::enet_initialize() == 0) {
            ready = true;
            std::atexit([] { ::enet_deinitialize(); });
        }
    });

    if (!ready) {
        error = "не удалось инициализировать сетевую библиотеку";
    }

    return ready;
}

/// Управляющие сообщения обязаны дойти и в исходном порядке; снимки состояния
/// устаревают быстрее, чем имеет смысл их переотправлять.
std::uint32_t packetFlagsFor(shared::Channel channel) noexcept {
    return channel == shared::Channel::Control ? ENET_PACKET_FLAG_RELIABLE
                                               : ENET_PACKET_FLAG_UNSEQUENCED;
}

PeerId peerIdOf(const ENetPeer* peer) noexcept {
    return static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(peer->data));
}

} // namespace

Host::~Host() {
    if (host_ != nullptr) {
        ::enet_host_destroy(host_);
    }
}

std::unique_ptr<Host> Host::listen(std::uint16_t port, std::size_t maxPeers, std::string& error) {
    if (!ensureTransportReady(error)) {
        return nullptr;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    ENetHost* host = ::enet_host_create(&address, maxPeers, shared::kChannelCount, 0, 0);
    if (host == nullptr) {
        error = "не удалось занять порт — возможно, он уже используется";
        return nullptr;
    }

    std::unique_ptr<Host> result{new Host};
    result->host_ = host;
    return result;
}

std::unique_ptr<Host> Host::connect(const std::string& address, std::uint16_t port,
                                    std::string& error) {
    if (!ensureTransportReady(error)) {
        return nullptr;
    }

    ENetAddress target{};
    target.port = port;
    if (::enet_address_set_host(&target, address.c_str()) != 0) {
        error = "не удалось разобрать адрес сервера";
        return nullptr;
    }

    // Клиенту нужно ровно одно исходящее соединение.
    ENetHost* host = ::enet_host_create(nullptr, 1, shared::kChannelCount, 0, 0);
    if (host == nullptr) {
        error = "не удалось создать сетевой узел";
        return nullptr;
    }

    if (::enet_host_connect(host, &target, shared::kChannelCount, 0) == nullptr) {
        ::enet_host_destroy(host);
        error = "не удалось начать подключение";
        return nullptr;
    }

    std::unique_ptr<Host> result{new Host};
    result->host_ = host;
    return result;
}

PeerId Host::registerPeer(ENetPeer* peer) {
    const PeerId id = nextPeerId_++;

    peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    peers_.emplace(id, peer);

    return id;
}

void Host::forgetPeer(ENetPeer* peer) {
    peers_.erase(peerIdOf(peer));
    peer->data = nullptr;
}

ENetPeer* Host::find(PeerId peer) const {
    const auto it = peers_.find(peer);
    return it == peers_.end() ? nullptr : it->second;
}

std::optional<Event> Host::poll(std::chrono::milliseconds timeout) {
    ENetEvent raw{};

    const int status =
        ::enet_host_service(host_, &raw, static_cast<enet_uint32>(timeout.count()));
    if (status <= 0) {
        // Ноль — событий не было; отрицательное — сбой обслуживания, и он тоже
        // не даёт события: разрыв придёт отдельно.
        return std::nullopt;
    }

    Event event;

    switch (raw.type) {
    case ENET_EVENT_TYPE_CONNECT:
        event.type = Event::Type::Connected;
        event.peer = registerPeer(raw.peer);
        return event;

    case ENET_EVENT_TYPE_DISCONNECT:
        event.type = Event::Type::Disconnected;
        event.peer = peerIdOf(raw.peer);
        forgetPeer(raw.peer);
        return event;

    case ENET_EVENT_TYPE_RECEIVE: {
        event.type = Event::Type::Message;
        event.peer = peerIdOf(raw.peer);
        event.channel = static_cast<shared::Channel>(raw.channelID);
        event.payload.assign(raw.packet->data, raw.packet->data + raw.packet->dataLength);

        ::enet_packet_destroy(raw.packet);
        return event;
    }

    case ENET_EVENT_TYPE_NONE:
        break;
    }

    return std::nullopt;
}

void Host::send(PeerId peer, shared::Channel channel, shared::ByteView payload) {
    ENetPeer* target = find(peer);
    if (target == nullptr) {
        return;
    }

    ENetPacket* packet =
        ::enet_packet_create(payload.data(), payload.size(), packetFlagsFor(channel));
    if (packet == nullptr) {
        return;
    }

    if (::enet_peer_send(target, static_cast<enet_uint8>(channel), packet) != 0) {
        // Транспорт не принял пакет и владения не забрал — освобождаем сами,
        // иначе это утечка на каждой неудачной отправке.
        ::enet_packet_destroy(packet);
    }
}

void Host::broadcast(shared::Channel channel, shared::ByteView payload, PeerId except) {
    for (const auto& [id, peer] : peers_) {
        if (id == except) {
            continue;
        }
        send(id, channel, payload);
    }
}

void Host::disconnect(PeerId peer) {
    if (ENetPeer* target = find(peer); target != nullptr) {
        ::enet_peer_disconnect(target, 0);
    }
}

void Host::flush() {
    ::enet_host_flush(host_);
}

} // namespace oxymp::net
