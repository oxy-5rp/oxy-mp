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

/// Чем кончилась связь с сервером.
///
/// Отдельно от ConnectionState, потому что отвечает на другой вопрос. Состояние
/// говорит, что происходит сейчас; здесь — что случилось и почему игрок остался
/// без сессии. По состоянию этого не узнать: Waiting одинаково означает «ещё ни
/// разу не подключались» и «выпали из игры, идёт повтор», а игроку это две очень
/// разные новости.
enum class DisconnectReason : std::uint8_t {
    /// Разрыва не было: либо мы в сессии, либо идёт первое подключение.
    None = 0,

    /// Связь оборвалась после того, как игрок уже был в сессии.
    ///
    /// Кто закрыл соединение — мы или сервер, — здесь не различается, и не по
    /// небрежности: транспорт этого не сообщает, а для игрока разницы нет. Он
    /// остался без сервера, и это всё, что ему нужно знать.
    Lost = 1,

    /// Сервер отказал окончательно. Причина — в rejectReason().
    Refused = 2,
};

/// Другой игрок, о котором сообщил сервер.
struct RemotePlayer {
    shared::PlayerId id = shared::kInvalidPlayerId;
    std::string nickname;

    /// Последнее и предыдущее известные состояния.
    ///
    /// Их два, потому что рисовать игрока строго по последнему снимку нельзя:
    /// снимки приходят реже кадров и с неровными промежутками, и модель будет
    /// прыгать. Промежуточное положение считается между этими двумя.
    shared::PlayerState previous;
    shared::PlayerState latest;

    /// Когда последний снимок пришёл — по нашим часам.
    ///
    /// Время прихода нужно только для одного: знать, сколько мы уже показываем
    /// этот отрезок. Длину самого отрезка оно больше не задаёт — её говорит
    /// отправитель отметкой времени, и оттого движение перестало дрожать.
    std::chrono::steady_clock::time_point latestAt{};

    /// Сколько снимков принято.
    ///
    /// Три состояния, а не два, и различать их обязательно. Ноль — показывать
    /// игрока нечем, он ещё нигде. Один — показать можно, но смешивать не с чем:
    /// отрезка ещё нет. Два и больше — обычная работа.
    std::uint32_t snapshots = 0;

    /// Есть ли чем показать игрока.
    [[nodiscard]] bool visible() const noexcept { return snapshots > 0; }

    /// Состояние на текущее мгновение.
    ///
    /// Плавно меняющееся — положение и направление взгляда — считается между
    /// двумя последними снимками пропорционально прошедшему времени, а за
    /// пределами этого промежутка достраивается, чтобы игрок не замирал при
    /// потере пакета. Остальное берётся из последнего снимка как есть:
    /// «целится» и «стреляет» плавно не меняются.
    ///
    /// Направление взгляда попало в первую половину не сразу, и в этом была
    /// причина того, что чужие игроки стояли повёрнутыми не туда. Раньше сюда
    /// уходил последний снимок, а доворачивал персонажа получатель — долей
    /// расхождения за кадр. Доля за кадр означает, что скорость доворота зависит
    /// от частоты кадров, и до нужного угла персонаж доходил уже тогда, когда
    /// хозяин смотрел в другую сторону.
    [[nodiscard]] shared::PlayerState at(std::chrono::steady_clock::time_point now) const;
};

/// Машина сессии.
///
/// Не «чужая»: список машин ведёт сервер, и в нём лежат все — включая те, что
/// ведём мы сами. Кто именно ведёт машину, сказано в owner, и от этого зависит
/// всё остальное обращение с ней.
///
/// Снимков хранится два, как и у игрока, и с тем же расчётом: они приходят реже
/// кадров, и показывать последний пришедший значит дёргать машину от точки к
/// точке. Машина едет быстрее человека, и рывки на ней заметнее вдвойне.
struct SessionVehicle {
    shared::VehicleState previous;
    shared::VehicleState latest;

    /// Когда последний снимок пришёл — по нашим часам.
    std::chrono::steady_clock::time_point latestAt{};

    /// Сколько снимков принято от нынешнего ведущего.
    ///
    /// От нынешнего — потому что отметки времени двух разных клиентов между
    /// собой несравнимы: у каждого свои часы, и разница между их отметками не
    /// означает ничего. Со сменой ведущего отсчёт начинается заново.
    ///
    /// Ноль здесь, в отличие от игрока, не означает «показывать нечем»: машину
    /// объявляет сервер вместе с её состоянием, и стоящую машину никто никогда
    /// не рассылает — снимков о ней не будет вовсе.
    std::uint32_t snapshots = 0;

    /// Кто ведёт машину. kInvalidPlayerId — никто.
    shared::PlayerId owner = shared::kInvalidPlayerId;

    /// Состояние на текущее мгновение.
    ///
    /// Поворот считается наравне с положением, и без этого не обойтись: машина в
    /// повороте разворачивается за десятые доли секунды, а снимков приходит
    /// двадцать в секунду. Взятый из последнего снимка, поворот отставал бы от
    /// положения — машина ехала бы боком.
    ///
    /// У машины без ведущего не считается ничего: новых снимков не будет, и
    /// достраивать движение не по чему. Она стоит там, где её оставили.
    [[nodiscard]] shared::VehicleState at(std::chrono::steady_clock::time_point now) const;
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

        /// Пароль сервера, если игрок его назвал. Пусто — обычное дело.
        std::string password;
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

    /// Как назвался сервер в приветствии. Пусто, пока он не ответил.
    ///
    /// Показывает его меню вместо адреса — так же, как alt:V. Взяться ему
    /// больше неоткуда: в адресе имени нет, а общего каталога серверов, где
    /// можно было бы спросить, у oxyMP тоже нет.
    [[nodiscard]] const std::string& serverName() const noexcept { return serverName_; }

    /// Задаёт состояние своего игрока, которое уходит на сервер.
    ///
    /// Идентификатор заполнять не нужно: сервер знает, чьё это соединение, и
    /// проставляет его сам.
    void setLocalState(const shared::PlayerState& state);

    /// Задаёт снимки машин, которые ведём мы.
    ///
    /// Их бывает много, и это не исключение, а обычное дело: ведущим машины
    /// назначают того, кто к ней ближе всех, и стоящий посреди двора отвечает
    /// разом за все машины во дворе. Раньше машина была одна — та, в которой
    /// игрок сидел, — и всё, что стояло рядом, не вёл никто.
    void setOwnedVehicles(std::vector<shared::VehicleState> vehicles);

    /// Задаёт внешности машин, которые мы ведём.
    ///
    /// Уходят по надёжному каналу и только при изменении: цвет и тюнинг меняются
    /// раз в сессию, а не двадцать раз в секунду, — но не дошедший цвет сам собой
    /// не исправится, в отличие от потерянного снимка.
    void setOwnedAppearances(std::vector<shared::VehicleAppearance> appearances);

    /// Отправляет серверу именованное событие.
    ///
    /// Единственный способ, которым клиент теперь просит сервер что-либо
    /// сделать. Раньше их было семь — завести машину, убрать её, выдать оружие,
    /// сменить погоду, поставить предмет, распорядиться игроком, — и каждый нёс
    /// в себе кусок правил игры. Правила уехали в ресурсы сервера; здесь
    /// осталось передать нажатие и не толковать его.
    void emit(std::string name, std::string payload);

    /// Забирает присланное снаряжение, если оно менялось.
    [[nodiscard]] std::optional<shared::PlayerLoadout> takeLoadout();

    /// Забирает присланное здоровье, если оно менялось.
    ///
    /// Не очередь: промежуточные значения никому не нужны, важно последнее. Кто
    /// именно попал, приходит отдельным сообщением об уроне — оно как раз
    /// очередь, потому что каждое попадание стоит показать.
    [[nodiscard]] std::optional<shared::HealthChanged> takeHealth();

    /// Забирает появившиеся предметы.
    [[nodiscard]] std::vector<shared::ObjectAdded> takeObjects();

    /// Забирает номера пропавших предметов.
    [[nodiscard]] std::vector<shared::ObjectId> takeRemovedObjects();

    /// Забирает присланное состояние мира, если оно менялось.
    ///
    /// Не очередь, в отличие от соседей: промежуточные значения никому не нужны,
    /// важно последнее. Пусто — значит с прошлого раза ничего не приходило.
    [[nodiscard]] std::optional<shared::WorldState> takeWorld();

    /// Отправляет реплику в чат. Сервер разошлёт её всем, включая нас.
    void say(std::string text);

    /// Сообщает серверу о попадании по чужому игроку.
    void reportDamage(shared::PlayerId victim, std::uint16_t amount, std::uint32_t weapon);

    /// Забирает пришедшие строки чата. Каждая отдаётся ровно один раз.
    [[nodiscard]] std::vector<shared::ChatLine> takeChatLines();

    /// Забирает пришедшие сообщения о попаданиях по нам.
    [[nodiscard]] std::vector<shared::DamageTaken> takeDamage();

    /// Забирает точки, в которые сервер велел перенести игрока.
    [[nodiscard]] std::vector<shared::Vec3> takeTeleports();

    /// Забирает пришедшие от сервера именованные события.
    ///
    /// Клиент их не толкует: имя и нагрузку сочиняет ресурс сервера, а здесь
    /// они лишь передаются дальше — странице интерфейса.
    [[nodiscard]] std::vector<shared::ServerEvent> takeServerEvents();

    /// Забирает пришедшие описания внешности чужих машин.
    [[nodiscard]] std::vector<shared::VehicleAppearance> takeVehicleAppearances();

    /// Забирает список раздаваемого сервером, если он приходил.
    ///
    /// Один раз за подключение: список приходит следом за приветствием и больше
    /// не меняется, пока соединение живо.
    [[nodiscard]] std::optional<std::vector<shared::ResourceEntry>> takeResources();

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }

    [[nodiscard]] shared::PlayerId localPlayerId() const noexcept { return localPlayerId_; }

    /// Где сервер велел появиться. Пусто, пока он не принял нас.
    [[nodiscard]] std::optional<shared::Vec3> spawnPosition() const noexcept {
        return spawnPosition_;
    }

    /// Время оборота до сервера. Пусто, пока не получен первый ответ.
    [[nodiscard]] std::optional<std::chrono::milliseconds> latency() const noexcept {
        return latency_;
    }

    /// Причина отказа, если сервер отказал.
    [[nodiscard]] std::optional<shared::RejectReason> rejectReason() const noexcept {
        return rejectReason_;
    }

    /// Остался ли игрок без сервера и почему.
    ///
    /// Держится до тех пор, пока сервер не примет нас заново: повторные попытки
    /// идут своим чередом, и удавшаяся снимает признак сама. Так окно разрыва
    /// исчезает ровно тогда, когда игрок снова в сессии, а не раньше.
    [[nodiscard]] DisconnectReason disconnectReason() const noexcept { return disconnect_; }

    [[nodiscard]] const std::unordered_map<shared::PlayerId, RemotePlayer>& remotePlayers()
        const noexcept {
        return remotePlayers_;
    }

    /// Все машины сессии по их номеру в ней.
    [[nodiscard]] const std::unordered_map<shared::VehicleId, SessionVehicle>& vehicles()
        const noexcept {
        return vehicles_;
    }

private:
    using Clock = std::chrono::steady_clock;

    void beginAttempt();
    void handleEvent(const net::Event& event);
    void handleMessage(const std::vector<std::uint8_t>& payload);
    void handleWelcome(const shared::ServerWelcome& welcome);
    void handleReject(const shared::ServerReject& reject);
    void handlePong(const shared::Pong& pong);
    void handleRemoteState(const shared::PlayerState& state);
    void handleRemoteVehicle(const shared::VehicleState& state);
    void handleVehicleAdded(const shared::VehicleAdded& added);
    void handleVehicleAuthority(const shared::VehicleAuthority& authority);

    void sendHello();
    void sendPingIfDue();
    void sendStateIfDue();

    /// Отправляет внешности наших машин, изменившиеся с прошлой отправки.
    void sendAppearancesIfChanged();

    /// Выталкивает всё, что игровой поток попросил отправить.
    void sendQueued();

    /// Сбрасывает состояние сессии и назначает следующую попытку.
    void fallBackToWaiting(std::string_view reason);

    /// Замечает, что сервер замолчал, и объявляет разрыв раньше транспорта.
    void noticeSilence();

    Settings settings_;

    std::unique_ptr<net::Host> host_;
    net::PeerId serverPeer_ = net::kInvalidPeerId;

    ConnectionState state_ = ConnectionState::Waiting;
    shared::PlayerId localPlayerId_ = shared::kInvalidPlayerId;

    /// Точка появления, названная сервером в приветствии.
    std::optional<shared::Vec3> spawnPosition_;
    std::optional<shared::RejectReason> rejectReason_;

    /// Как назвался сервер. Приходит с приветствием и живёт до конца попытки.
    std::string serverName_;

    /// Чем кончилась связь. Поднимается разрывом, снимается новым приветствием.
    DisconnectReason disconnect_ = DisconnectReason::None;

    /// Когда сервер сказал нам хоть что-нибудь в последний раз.
    ///
    /// Пусто до первого пакета: молчание того, кто ещё ни разу не говорил, — это
    /// не разрыв, а подключение, и о нём рассказывает состояние.
    std::chrono::steady_clock::time_point heardAt_{};

    /// Когда цикл обновления проходил здесь в прошлый раз.
    ///
    /// Нужно, чтобы отличить молчание сервера от собственного беспамятства: наш
    /// поток живёт внутри процесса игры и на тяжёлой подгрузке не получает
    /// управления секундами.
    std::chrono::steady_clock::time_point loopSeenAt_{};
    std::unordered_map<shared::PlayerId, RemotePlayer> remotePlayers_;

    /// Машины сессии, какими их объявил сервер. Живут, пока он не скажет иначе.
    std::unordered_map<shared::VehicleId, SessionVehicle> vehicles_;

    /// Пришедшее с сервера, что ещё не забрал игровой поток.
    std::vector<shared::ChatLine> chatLines_;
    std::vector<shared::DamageTaken> damage_;
    std::vector<shared::Vec3> teleports_;
    std::vector<shared::ServerEvent> serverEvents_;
    std::vector<shared::VehicleAppearance> vehicleAppearances_;

    /// Последнее присланное состояние мира, ещё не забранное.
    std::optional<shared::WorldState> world_;

    /// Последнее присланное снаряжение и здоровье, ещё не забранные.
    std::optional<shared::PlayerLoadout> loadout_;
    std::optional<shared::HealthChanged> health_;

    /// Появившиеся и пропавшие предметы, ещё не забранные.
    std::vector<shared::ObjectAdded> objects_;
    std::vector<shared::ObjectId> removedObjects_;

    /// Список раздаваемого сервером, ещё не забранный.
    std::optional<std::vector<shared::ResourceEntry>> resources_;

    /// Отправляемое, что ещё не ушло. Копится между вызовами update: игровой
    /// поток кладёт сюда в любой момент кадра, а отправка идёт своим чередом.
    std::vector<shared::ChatSay> outgoingChat_;
    std::vector<shared::DamageReport> outgoingDamage_;
    std::vector<shared::ClientEvent> outgoingEvents_;

    Clock::time_point nextAttemptAt_ = Clock::now();
    Clock::time_point nextPingAt_ = Clock::time_point::max();

    /// Задержка до следующей попытки. Растёт при неудачах, чтобы клиент не
    /// долбился в выключенный сервер по десять раз в секунду.
    std::chrono::milliseconds retryDelay_{500};

    std::optional<std::chrono::milliseconds> latency_;
    bool stopped_ = false;

    shared::PlayerState localState_;

    /// Снимки машин, которые ведём мы, — то, что уйдёт следующей отправкой.
    std::vector<shared::VehicleState> ownedVehicles_;

    Clock::time_point nextStateAt_ = Clock::time_point::max();

    /// Внешности наших машин и те, что уже отправлены.
    ///
    /// Две карты, а не одна: отправлять внешность нужно при изменении, а узнать
    /// об изменении можно только сравнив с тем, что ушло в прошлый раз.
    std::vector<shared::VehicleAppearance> ownedAppearances_;
    std::unordered_map<shared::VehicleId, shared::VehicleAppearance> sentAppearances_;
};

/// Человекочитаемое описание причины отказа.
[[nodiscard]] std::string_view describe(shared::RejectReason reason) noexcept;

/// Человекочитаемое описание состояния соединения.
[[nodiscard]] std::string_view describe(ConnectionState state) noexcept;

} // namespace oxymp::client
