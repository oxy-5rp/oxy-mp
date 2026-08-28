#pragma once

#include "attachment_directory.hpp"
#include "config.hpp"
#include "console.hpp"
#include "http_server.hpp"
#include "player_registry.hpp"
#include "resource_catalog.hpp"
#include "resource_store.hpp"
#include "drawn_directory.hpp"
#include "object_directory.hpp"
#include "ped_directory.hpp"
#include "runtime.hpp"
#include "server_core.hpp"
#include "state_bundler.hpp"
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

    /// Объявляет скриптам вход игрока — когда его клиент к этому готов.
    ///
    /// Отдельно от самого входа: соединение установилось раньше, чем на той
    /// стороне поднялись ресурсы, и объявленный сразу вход застал бы клиента
    /// глухим.
    void announcePlayerReady(Player& player);


    /// Принимает объявленную игроком внешность и пересказывает её остальным.
    void handlePlayerAppearance(net::PeerId peer, shared::PlayerAppearance appearance);

    /// Рассылает накопленные снимки: по одной посылке на игрока за такт.
    void broadcastStates();

    /// То же для машин: снимки всех, кого игрок видит, одной посылкой за такт.
    ///
    /// Отдельным проходом, а не вместе с игроками, и это не разделение ради
    /// разделения. У машин другой круг получателей: снимок машины нужен тому,
    /// кто её видит, а видит он не тех же, кого видит его сосед. Список видимых
    /// машин у сервера уже есть — его ведёт раздача (streamVehicles), — и
    /// перебирать по нему дешевле, чем считать расстояния до всех машин сессии
    /// на каждого игрока.
    void broadcastVehicleStates();

    /// Пишет в отладочный журнал, во что обходится сессия.
    ///
    /// Заодно и то, поспевает ли сервер за собственной частотой. Настройка
    /// `tickrate` говорит, сколько тактов в секунду он должен сделать, а эта
    /// строка — сколько сделал: разошлись, значит такт не укладывается в
    /// отведённое ему время, и снимки уходят реже обещанного. Узнать это иначе
    /// нельзя ниоткуда — снаружи оно выглядит просто как «дёргается».
    void reportTraffic();
    void handlePing(net::PeerId peer, const shared::Ping& ping);
    void handlePlayerState(net::PeerId peer, shared::PlayerState state);
    void handleVehicleState(net::PeerId peer, shared::VehicleState state);
    void handleVehicleAppearance(net::PeerId peer, shared::VehicleAppearance appearance);

    /// Игрок выстрелил: пересказать всем, кто это должен увидеть.
    ///
    /// Сервер выстрел не толкует и урона по нему не считает — урон приходит
    /// отдельным сообщением от того, кто попал. Здесь только пересылка: пуля
    /// это, ракета или граната, знает игра получателя по хешу оружия.
    void handleWeaponFired(net::PeerId peer, shared::WeaponFired fired);
    /// Именованное событие от клиента.
    ///
    /// Единственное, чем клиент теперь просит сервер что-либо сделать. Раньше
    /// таких сообщений было семь — завести машину, убрать её, выдать оружие,
    /// сменить погоду, поставить предмет, распорядиться игроком, — и каждое несло
    /// в себе кусок правил игры. Правила уехали в ресурсы, а клиенту осталось
    /// передать нажатие.
    void handleClientEvent(net::PeerId peer, const shared::ClientEvent& event);

    /// Пересказывает игроку распоряжение о его теле.
    ///
    /// Отдельно от `controlChanged`, потому что зовётся ещё и при возрождении:
    /// `RESURRECT_PED` сбрасывает признаки персонажа, и поднятый игрок
    /// разморозился бы сам собой, а сервер об этом не узнал бы никогда.
    void sendControl(const Player& player);

    /// Сообщает игроку его здоровье и броню.
    void sendHealth(const Player& player, shared::PlayerId attacker);

    /// Выдаёт игроку его снаряжение.
    void sendLoadout(const Player& player, bool replace);

    /// Рассказывает остальным, как выглядит оружие в руках у игрока.
    ///
    /// Только при изменении: список насадок меняется раз в несколько минут, а
    /// весит больше самого снимка. Что изменилось, сервер знает по тому, что
    /// объявлял в прошлый раз (Player::shownWeapon).
    void announceWeapon(Player& player);

    /// Поднимает мёртвых, у которых вышел срок.
    void reviveDead();

    // --- То, чего скриптовое ядро не может само (CoreSink) ------------------------
    //
    // Каждое из них — рассылка: ядро меняет реестры, а рассказать об этом сети
    // умеет только сервер.

    void healthChanged(const Player& player) override;
    void appearanceChanged(const Player& player) override;
    void teleported(const Player& player, const shared::Vec3& position) override;
    void seated(const Player& player, shared::VehicleId vehicle,
                std::int8_t seat) override;
    void kicked(const Player& player, std::string_view reason) override;
    void emitted(const Player& player, std::string_view name, std::string_view payload) override;
    void loadoutChanged(const Player& player, bool replace) override;
    void vehicleAdded(shared::VehicleId id) override;
    void vehicleRemoved(shared::VehicleId id) override;
    /// Соединение ведущего машины. kInvalidPeerId — вести её некому.
    [[nodiscard]] net::PeerId ownerPeerOf(shared::VehicleId id) const;

    void vehicleTeleported(shared::VehicleId id, const shared::Vec3& position,
                           float heading) override;
    void vehicleRepaired(shared::VehicleId id) override;
    void vehicleAppearanceChanged(shared::VehicleId id) override;
    void vehicleControlChanged(shared::VehicleId id) override;
    void vehicleDoorsChanged(shared::VehicleId id) override;
    void attachmentChanged(AttachmentDirectory::Ref entity) override;
    void controlChanged(const Player& player) override;
    void objectAdded(shared::ObjectId id) override;
    void objectMoved(shared::ObjectId id) override;
    void objectRemoved(shared::ObjectId id) override;
    void pedChanged(shared::PedId id) override;
    void pedRemoved(shared::PedId id) override;

    /// Раздаёт прохожих по расстоянию — тем же порядком, что и предметы.
    void streamPeds();
    /// Рассказывает вошедшему обо всём нарисованном, что ему видно: о метках
    /// на карте, о маркерах и о контрольных точках.
    void sendDrawnTo(const Player& player);

    /// Рассказывает вошедшему обо всех привязках сессии.
    ///
    /// Обо всех, а не о видимых: привязка связывает две сущности, и хотя бы
    /// одной из них у вошедшего в это мгновение может не быть — машины ему ещё
    /// не раздали. Клиент запомнит привязку и наденет её, когда оба конца у него
    /// появятся, — так же, как поступает с внешностью машины.
    void sendAttachmentsTo(net::PeerId peer);

    /// Всё из одного реестра картинок — одному игроку.
    template<typename Directory>
    void sendKindTo(const Directory& directory, const Player& player);

    /// Одну картинку — всем, кто с ней в одном слое мира.
    ///
    /// Общее на три рода, потому что рассылаются они совершенно одинаково:
    /// найти в реестре, обойти игроков, сверить слой, отправить. Разным их
    /// делает только тип сообщения, а его выводит сам реестр.
    template<typename Directory, typename Removed>
    void broadcastDrawn(const Directory& directory, typename Directory::Id id);

    void blipChanged(shared::BlipId id) override;
    void blipRemoved(shared::BlipId id) override;

    void markerChanged(shared::MarkerId id) override;
    void markerRemoved(shared::MarkerId id) override;

    void checkpointChanged(shared::CheckpointId id) override;
    void checkpointRemoved(shared::CheckpointId id) override;

    void animationPlayed(const Player& player,
                         const shared::PlayerAnimation& animation) override;
    void exploded(const shared::Explosion& explosion, std::int32_t dimension) override;

    void dimensionChanged(const Player& player, std::int32_t previous) override;

    /// Досылает и отзывает картинки одного рода после смены слоя мира.
    template<typename Directory, typename Removed>
    void redrawKindFor(const Directory& directory, const Player& player, std::int32_t previous);
    void worldChanged() override;
    void chatLine(shared::PlayerId to, std::string text) override;

    [[nodiscard]] std::string addressOf(const Player& player) const override;
    [[nodiscard]] std::uint32_t latencyOf(const Player& player) const override;

    /// Объявляет скриптам событие про игрока.
    ///
    /// Возвращает false, если кто-то из них его отменил. Считается это только у
    /// реплики в чат — остальное скрипту отменять незачем, и делать вид, что он
    /// на это влияет, было бы обманом.
    bool tellScripts(script::EventKind kind, shared::PlayerId about, std::string text = {});

    /// Объявляет скриптам всё, что видно из разницы двух снимков игрока.
    ///
    /// Посадка в машину, высадка, пересадка и смена оружия в руках — четыре
    /// события alt:V, которых у нас не было вовсе. Отдельного сообщения ни у
    /// одного из них нет и быть не может: клиент рассказывает о себе снимком, и
    /// всё это в нём уже есть — недоставало сравнения.
    ///
    /// Сравнение живёт здесь, а не в скрипте, потому что в скрипте оно
    /// невозможно: снимки до него не доходят, а состояние, спрошенное в такте,
    /// он застаёт уже переменившимся.
    ///
    /// Зовётся до того, как свежий снимок ляжет в реестр: обработчику нужен
    /// игрок таким, каким он стал, — то есть после `player.state = state`, — но
    /// разница считается по тому, что было. Отсюда и два довода.
    /// Объявляет скриптам то, что видно из разницы признаков машины.
    ///
    /// Гудок и сирена приходят признаком, а не сообщением: увидеть нажатие можно
    /// только сравнением снимков — так же, как посадку в машину.
    void tellScriptsAboutVehicle(const Player& owner, shared::VehicleId vehicle,
                                 std::uint16_t before, std::uint16_t after);

    void tellScriptsAboutChanges(const Player& player, const shared::PlayerState& before,
                                 const shared::PlayerState& after);

    /// Сколько игрок лежит мёртвым, прежде чем сервер поднимет его.
    static constexpr auto kRespawnDelay = std::chrono::seconds{5};
    /// Разбирает набранное в окне сервера и объявляет это скриптам.
    ///
    /// Раз в такт, а не по строке: строки приходят чужим потоком, и сервер
    /// забирает накопленное там же, где делает всё остальное.
    void runConsole();

    void handleChatSay(net::PeerId peer, const shared::ChatSay& say);
    void handleDamageReport(net::PeerId peer, const shared::DamageReport& report);

    /// Разбирает свидетельство о попадании по чужой машине.
    ///
    /// Сервер здесь не судья, а посредник: прочность машины живёт в игре у её
    /// ведущего — физику сервер не считает, — и отнять её может только он. Дело
    /// сервера — проверить, что попадание правдоподобно, и передать его тому,
    /// кто машину ведёт.
    void handleVehicleDamage(net::PeerId peer, const shared::VehicleDamageReport& report);

    /// Рассылает строку чата всем и записывает её в журнал сервера.
    void announce(shared::ChatKind kind, shared::PlayerId author, std::string nickname,
                  std::string text);

    /// Отправляет отказ и закрывает соединение.
    void reject(net::PeerId peer, shared::RejectReason reason, std::string_view nickname);

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
                       std::int32_t dimension, net::PeerId except = net::kInvalidPeerId) {
        const auto packet = shared::encode(message);
        const float reach = config_.streamDistance * config_.streamDistance;

        for (const auto& [peer, player] : players_) {
            if (peer == except || shared::distanceSquared(origin, player.position) > reach) {
                continue;
            }

            // Из чужого слоя мира — никому и ничего.
            if (!script::dimensionsMeet(player.dimension, dimension)) {
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

    /// Прохожие: куклы, поставленные сервером. Отдельно от предметов, потому что
    /// у них есть здоровье, броня и оружие, а у ящика — только место.
    PedDirectory peds_;

    /// Что нарисовано на карте.
    BlipDirectory blips_;

    /// Что нарисовано в мире: фигуры под ногами и контрольные точки.
    ///
    /// Отдельно от предметов, хотя и стоят в тех же местах: у предмета есть
    /// тело, его можно объехать и об него можно удариться, а эти двое —
    /// картинки, сквозь которые проходят насквозь.
    MarkerDirectory markers_;
    CheckpointDirectory checkpoints_;

    /// Кто к кому привязан.
    ///
    /// Отдельным реестром, потому что привязка не принадлежит ни одному роду
    /// сущностей: предмет вешают на человека, человека сажают на предмет,
    /// предмет цепляют к машине.
    AttachmentDirectory attachments_;

    /// Что сервер раздаёт клиентам сверх самой игры.
    ///
    /// Объявлен раньше раздачи и потому переживает её: раздача держит на него
    /// ссылку и читает из своего потока.
    /// Уходит ли файл ресурса в свёрток, а не отдельным файлом.
    ///
    /// В свёрток — исходники режима и страницы интерфейса: то, чему у игрока на
    /// диске обычным текстом лежать незачем. Отдельно — всё остальное, начиная с
    /// моделей: их читает сама игра, а свёрток ей не открыть.
    ///
    /// `described` — назван ли файл в `[meta]`. Названный не прячется никогда:
    /// его читает загрузчик данных игры, и подать ему свёрток нечем.
    ///
    /// `size` — длина файла. Крупное остаётся снаружи: свёрток лежит у сервера в
    /// памяти целиком, а клиент качает его одним куском.
    [[nodiscard]] static bool goesIntoBundle(std::string_view file, bool described,
                                             std::uintmax_t size);

    ResourceStore resources_;

    /// Какой файл в каком свёртке лежит. Ключ — составное имя, как в раздаче.
    ///
    /// Заполняется при запуске ресурсов и живёт до конца: список раздаваемого
    /// собирается заново на каждое подключение, а свёртки — один раз.
    std::unordered_map<std::string, std::string> bundled_;

    std::unique_ptr<HttpServer> http_;

    /// Когда соединение подключилось. Запись живёт до рукопожатия: молчащий
    /// клиент иначе занимал бы место в лимите игроков бесконечно.
    std::unordered_map<net::PeerId, std::chrono::steady_clock::time_point> awaitingHello_;

    /// Погода и часы сессии. Идут здесь, а не у клиентов.
    ///
    /// Со значением по умолчанию, потому что сервер собирается пустым, а
    /// настройки прикладываются к нему в start. Заменяется там же целиком.
    WorldClock world_{"EXTRASUNNY", 12, 0};

    /// Окно сервера: то, что в нём набирает хозяин.
    ///
    /// Читается своим потоком и забирается на такте. Сам сервер набранным не
    /// распоряжается — строка уходит скриптам событием `consoleCommand`.
    Console console_;

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
    ServerCore core_{players_,  vehicles_, objects_,     peds_,   blips_,   markers_,
                     checkpoints_, attachments_, world_, config_, events_, *this};

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

    /// Каким был счётчик тактов на прошлой такой записи.
    ///
    /// По разнице видно, сколько тактов сервер успел сделать за десять секунд, —
    /// то есть поспевает ли он за собственной частотой.
    std::uint64_t reportedTick_ = 0;

    /// Номер такта. По нему решается, кому из дальних достанется снимок в этот
    /// раз: считать по счётчику на каждую пару игроков было бы вдвое дороже
    /// самой рассылки.
    std::uint64_t tick_ = 0;

    // --- Рабочие буферы рассылки снимков ------------------------------------
    //
    // Живут между тактами не ради скорости выделения, хотя и ради неё тоже, а
    // потому что рассылка — единственное место сервера, которое растёт как
    // квадрат числа игроков. Всё остальное трогает игрока по разу.

    /// Один игрок в рабочем списке рассылки.
    ///
    /// Плоская выжимка из реестра, собираемая заново на каждый такт. Причина не
    /// в стройности: перебор идёт всех против всех, и при тысяче игроков это
    /// миллион пар за такт. Ходить по ним через хеш-таблицу с записями в
    /// несколько сотен байт — значит промахнуться мимо кеша миллион раз;
    /// выжимка укладывает тысячу игроков в двадцать четыре килобайта и живёт в
    /// кеше целиком.
    struct StateSlot {
        net::PeerId peer = net::kInvalidPeerId;
        shared::PlayerId id = shared::kInvalidPlayerId;
        shared::Vec3 position;

        /// В каком слое мира игрок. Здесь же, а не спрашивается из реестра:
        /// перебор идёт всех против всех, и лишний поход в хеш-таблицу на
        /// каждую пару стоил бы дороже самой рассылки.
        std::int32_t dimension = script::kDefaultDimension;

        /// Где в общем буфере лежат готовые байты снимка этого игрока.
        /// Нулевая длина означает, что в этот такт он не рассылается вовсе.
        std::uint32_t offset = 0;
        std::uint32_t length = 0;

        /// Молчал ли этот игрок до нынешнего снимка.
        ///
        /// Такому снимку прореживание по расстоянию не помеха: см.
        /// kQuietGap. Признак считается один раз в первом проходе, а не на
        /// каждую пару, — перебор идёт всех против всех.
        bool quiet = false;
    };

    std::vector<StateSlot> slots_;

    /// Одна машина в рабочем списке рассылки.
    ///
    /// Того же устройства и по той же причине, что и StateSlot: снимок машины
    /// пишется в байты один раз за такт, а получателям достаётся копией байт.
    /// Прежде он собирался заново на каждого, кто машину видит.
    struct VehicleSlot {
        shared::VehicleId id = shared::kInvalidVehicleId;
        shared::Vec3 position;

        /// Кто её ведёт. Ему самому снимок не отправляется: он же его и прислал.
        shared::PlayerId owner = shared::kInvalidPlayerId;

        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };

    std::vector<VehicleSlot> vehicleSlots_;

    /// Снимки машин, разосланных в этот такт, записанные подряд.
    shared::ByteWriter vehicleBlob_;

    /// Когда рассылка машин смотрела на них в прошлый раз.
    ///
    /// По ней и решается, что рассылать: машина, о которой с тех пор пришёл
    /// снимок, уезжает получателям, а та, о которой не приходило ничего,
    /// молчит. Отдельного признака «свежий» для этого не нужно — время прихода
    /// снимка реестр машин помнит и так.
    ///
    /// Молчащую машину досылать незачем, в отличие от молчащего игрока: о
    /// машине, попавшей в поле зрения, получателю рассказывают полностью
    /// (VehicleAdded), а брошенная у обочины машина снимков не шлёт вовсе — её
    /// некому слать.
    std::chrono::steady_clock::time_point vehiclesSweptAt_{};

    /// Снимки всех, кто рассылается в этот такт, записанные подряд.
    ///
    /// Каждый пишется ровно один раз, а получателям достаётся копией байт.
    /// Прежде снимок собирался заново для каждого, кто его увидит: при сотне
    /// соседей у каждой тысячи игроков это сто тысяч сборок за такт вместо
    /// тысячи.
    shared::ByteWriter blob_;

    /// Складывает снимки в связки, не давая связке перерасти посылку.
    StateBundler bundler_;
};

} // namespace oxymp::server
