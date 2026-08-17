#pragma once

#include "config.hpp"
#include "http_server.hpp"
#include "player_registry.hpp"
#include "resource_catalog.hpp"
#include "resource_store.hpp"
#include "object_directory.hpp"
#include "runtime.hpp"
#include "server_core.hpp"
#include "vehicle_directory.hpp"
#include "world_clock.hpp"

#include <oxymp/net/host.hpp>
#include <oxymp/script/events.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oxymp::server {

/// Сервер: владеет сетевым узлом и списком игроков.
///
/// Единственный владелец своих подсистем — они создаются вместе с ним и живут
/// ровно столько же.
///
/// Наследование от CoreSink закрытое и означает ровно одно: сервер умеет
/// разослать то, что скриптовое ядро изменило. Наружу это не видно и видно быть
/// не должно — принимает ядро эту способность при сборке и больше ни у кого о
/// ней не спрашивает.
class Server : private CoreSink {
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

    /// Принимает объявленную игроком внешность и пересказывает её остальным.
    void handlePlayerAppearance(net::PeerId peer, shared::PlayerAppearance appearance);

    /// Рассылает накопленные снимки: по одной посылке на игрока за такт.
    void broadcastStates();

    /// Пишет в отладочный журнал, во что обходится сессия.
    void reportTraffic();
    void handlePing(net::PeerId peer, const shared::Ping& ping);
    void handlePlayerState(net::PeerId peer, shared::PlayerState state);
    void handleVehicleState(net::PeerId peer, shared::VehicleState state);
    void handleVehicleAppearance(net::PeerId peer, shared::VehicleAppearance appearance);
    /// Именованное событие от клиента.
    ///
    /// Единственное, чем клиент теперь просит сервер что-либо сделать. Раньше
    /// таких сообщений было семь — завести машину, убрать её, выдать оружие,
    /// сменить погоду, поставить предмет, распорядиться игроком, — и каждое несло
    /// в себе кусок правил игры. Правила уехали в ресурсы, а клиенту осталось
    /// передать нажатие.
    void handleClientEvent(net::PeerId peer, const shared::ClientEvent& event);

    /// Сообщает игроку его здоровье и броню.
    void sendHealth(const Player& player, shared::PlayerId attacker);

    /// Выдаёт игроку его снаряжение.
    void sendLoadout(const Player& player, bool replace);

    /// Поднимает мёртвых, у которых вышел срок.
    void reviveDead();

    // --- То, чего скриптовое ядро не может само (CoreSink) ------------------------
    //
    // Каждое из них — рассылка: ядро меняет реестры, а рассказать об этом сети
    // умеет только сервер.

    void healthChanged(const Player& player) override;
    void teleported(const Player& player, const shared::Vec3& position) override;
    void emitted(const Player& player, std::string_view name, std::string_view payload) override;
    void loadoutChanged(const Player& player, bool replace) override;
    void vehicleAdded(shared::VehicleId id) override;
    void vehicleRemoved(shared::VehicleId id) override;
    void objectAdded(shared::ObjectId id) override;
    void objectRemoved(shared::ObjectId id) override;
    void worldChanged() override;
    void chatLine(shared::PlayerId to, std::string text) override;

    /// Объявляет скриптам событие про игрока.
    ///
    /// Возвращает false, если кто-то из них его отменил. Считается это только у
    /// реплики в чат — остальное скрипту отменять незачем, и делать вид, что он
    /// на это влияет, было бы обманом.
    bool tellScripts(script::EventKind kind, shared::PlayerId about, std::string text = {});

    /// Сколько игрок лежит мёртвым, прежде чем сервер поднимет его.
    static constexpr auto kRespawnDelay = std::chrono::seconds{5};
    void handleChatSay(net::PeerId peer, const shared::ChatSay& say);
    void handleDamageReport(net::PeerId peer, const shared::DamageReport& report);

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

    /// Читает описания ресурсов, поднимает их и берёт в раздачу их файлы.
    void startResources();

    /// Останавливает поднятое.
    void stopResources();

    /// Заводит машины под те языки, что встретились в перечне ресурсов.
    ///
    /// По надобности, а не про запас: голый сервер не должен платить памятью и
    /// временем запуска за движок, которым никто не пользуется.
    void ensureRuntimes();

    /// Машина для ресурсов такого типа. nullptr — такой в сервере нет.
    [[nodiscard]] Runtime* runtimeFor(std::string_view type) const;

    /// Разрывает соединения, которые подключились, но так и не представились.
    void dropSilentPeers();

    /// Пересматривает, кто какую машину ведёт, и рассылает изменения.
    ///
    /// Не каждый тик: пересмотр перебирает все машины на всех игроков, а
    /// расстояния между ними за тридцатую долю секунды меняются на сантиметры.
    /// Раз в полсекунды и достаточно — этого хватает, чтобы подъехавший принял
    /// машину раньше, чем успеет к ней подойти.
    void reassignVehicles();

    /// Рассказывает каждому о машинах, которые к нему приблизились, и убирает
    /// те, что отдалились.
    ///
    /// Так же устроено в RAGE MP: клиент знает не обо всей сессии, а о том, что
    /// вокруг него. Рассказывать каждому обо всём — значит слать сотню снимков
    /// в секунду о машинах на другом конце карты, которых он всё равно не
    /// увидит.
    void streamVehicles();

    /// Объявляет игроку машину вместе с её внешностью.
    ///
    /// Вместе — потому что порознь нельзя: внешность накладывается на машину, и
    /// пришедшая раньше неё оказалась бы никому не нужна.
    void sendVehicleTo(net::PeerId peer, const VehicleDirectory::Vehicle& vehicle);

    /// Рассылает погоду и время, если пора.
    void broadcastWorld();

    /// Рассказывает каждому о предметах, которые к нему приблизились.
    ///
    /// Тем же порядком, что и машины, и по той же причине. Отдельно от них —
    /// потому что номера у предметов свои, и списки виденного у игрока тоже
    /// разные.
    void streamObjects();

    /// Рассылает сообщение тем, кто достаточно близко к точке.
    ///
    /// Ею уходит всё, что описывает происходящее в одном месте мира: снимки
    /// игроков и машин, их внешность. Игроку незачем знать, как бежит человек за
    /// полкилометра от него и какого цвета там машина, — он их всё равно не
    /// увидит, а снимков это половина всего, что ходит по сети.
    ///
    /// Канал задаётся вызывающим, потому что разным сообщениям нужен разный:
    /// снимок устаревает через полсотни миллисекунд и переотправки не стоит, а
    /// не дошедший цвет не исправится сам никогда.
    template<typename Message>
    void broadcastNear(const shared::Vec3& origin, shared::Channel channel, const Message& message,
                       net::PeerId except = net::kInvalidPeerId) {
        const auto packet = shared::encode(message);
        const float reach = config_.streamDistance * config_.streamDistance;

        for (const auto& [peer, player] : players_) {
            if (peer == except || shared::distanceSquared(origin, player.position) > reach) {
                continue;
            }

            host_->send(peer, channel, shared::ByteView{packet});
        }
    }

    Config config_;
    std::unique_ptr<net::Host> host_;
    PlayerRegistry players_;

    /// Кто в какой машине сидит и как эти машины выглядят.
    VehicleDirectory vehicles_;

    /// Что расставлено в мире руками.
    ObjectDirectory objects_;

    /// Что сервер раздаёт клиентам сверх самой игры.
    ///
    /// Объявлен раньше раздачи и потому переживает её: раздача держит на него
    /// ссылку и читает из своего потока.
    ResourceStore resources_;

    std::unique_ptr<HttpServer> http_;

    /// Когда соединение подключилось. Запись живёт до рукопожатия: молчащий
    /// клиент иначе занимал бы место в лимите игроков бесконечно.
    std::unordered_map<net::PeerId, std::chrono::steady_clock::time_point> awaitingHello_;

    /// Погода и часы сессии. Идут здесь, а не у клиентов.
    ///
    /// Со значением по умолчанию, потому что сервер собирается пустым, а
    /// настройки прикладываются к нему в start. Заменяется там же целиком.
    WorldClock world_{"EXTRASUNNY", 12, 0};

    /// Кто слушает происходящее в сессии.
    ///
    /// Объявлены раньше ядра и ресурсов, потому что переживают и то и другое:
    /// ресурс отписывается при остановке, и список обязан быть жив в это
    /// мгновение.
    script::Events events_;

    /// Лицо сервера, обращённое к скриптам.
    ///
    /// Собрано на тех же реестрах, что и всё остальное: своих списков у него нет
    /// и быть не должно — они разошлись бы с настоящими молча.
    ServerCore core_{players_, vehicles_, objects_, world_, config_, events_, *this};

    /// Что за ресурсы хозяин велел поднять и что о них сказано в их описаниях.
    ResourceCatalog catalog_;

    /// Скриптовые машины, по одной на язык.
    ///
    /// Списком, а не полем на каждую: сервер не должен знать, сколько их и какие
    /// они — он спрашивает по типу ресурса и получает ту, что откликнулась. Так
    /// добавление языка не трогает ничего, кроме места, где список наполняется.
    ///
    /// Пустой список — обычное состояние голого сервера, а не поломка.
    std::vector<std::unique_ptr<Runtime>> runtimes_;

    /// Когда в последний раз пересматривали ведущих и раздавали машины по виду.
    ///
    /// Отсчёты разные, потому что поводы разные: пересмотр ведущих случается ещё
    /// и по событиям — сел за руль, вышел из сессии, — а раздача идёт своим
    /// ровным чередом.
    std::chrono::steady_clock::time_point reassignedAt_{};
    std::chrono::steady_clock::time_point streamedAt_{};

    /// Когда в последний раз писали в журнал, во что обходится сессия.
    std::chrono::steady_clock::time_point trafficAt_{};
};

} // namespace oxymp::server
