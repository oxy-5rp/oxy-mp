#pragma once

#include <oxymp/client/interpolation.hpp>
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

    /// Принятые снимки, от старого к свежему.
    ///
    /// Лента, а не пара «предыдущий и последний», и это не запас на будущее.
    /// Показываемое мгновение отстаёт от последнего снимка на отставание, а
    /// отставание больше промежутка между снимками — в этом весь его смысл.
    /// Значит лежит оно раньше предыдущего снимка, и паре его показать нечем:
    /// расчёт упирается в край отрезка и замирает там до следующего снимка.
    /// Подробности — у самой ленты.
    interpolation::Timeline<shared::PlayerState> timeline;

    /// Когда последний снимок пришёл — по нашим часам.
    ///
    /// Время прихода нужно только для одного: знать, сколько мы уже показываем
    /// этот отрезок. Длину самого отрезка оно больше не задаёт — её говорит
    /// отправитель отметкой времени, и оттого движение перестало дрожать.
    std::chrono::steady_clock::time_point latestAt{};

    /// Сколько снимков принято.
    ///
    /// Показывать или нет, по нему больше не решают — это спрашивают у ленты.
    /// Остался он ради журнала и бота: по нему видно, идут ли снимки вообще.
    std::uint32_t snapshots = 0;

    /// На сколько показывать этого игрока позади настоящего времени.
    ///
    /// Своя на каждого, а не одна на всех: сети у чужих игроков разные, и общая
    /// оценка означала бы, что хуже видно сразу всех — по худшему из них.
    interpolation::DelayEstimator pace;

    /// Часы показа: они и говорят, какое мгновение показывать.
    ///
    /// Оценка pace говорит, каким отставание должно быть; часы говорят, каково
    /// оно сейчас. Разница видна только под дрожанием сети, зато там она и есть
    /// вся ровность движения — подробности у самих часов.
    interpolation::ShowClock show;

    /// Расхождение, оставшееся от прошлого отрезка.
    ///
    /// Каждый новый отрезок начинается не там, где кончился прошлый: пока
    /// свежих снимков не было, движение достраивалось вперёд по скорости, а
    /// пришедший снимок возвращает тело туда, где оно было на самом деле.
    /// Разница запоминается здесь и гасится за четверть секунды — иначе каждая
    /// потеря пакета видна как рывок назад и тут же вперёд.
    shared::Vec3 seam;

    /// Есть ли чем показать игрока.
    [[nodiscard]] bool visible() const noexcept { return !timeline.empty(); }

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
/// Снимки лежат лентой, как и у игрока, и по той же причине: показываемое
/// мгновение отстаёт от последнего снимка дальше, чем на один промежуток, и
/// паре снимков показать его нечем. Машина едет быстрее человека, и ступеньки
/// на ней заметнее вдвойне.
struct SessionVehicle {
    interpolation::Timeline<shared::VehicleState> timeline;

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

    /// На сколько показывать машину позади настоящего времени.
    ///
    /// Своя, как и у игрока, и по той же причине; вдобавок машина меняет
    /// ведущего по ходу игры, а с ним меняется и сеть, по которой приходят её
    /// снимки. Смена ведущего оценку обнуляет — см. forget.
    interpolation::DelayEstimator pace;

    /// Часы показа: они и говорят, какое мгновение показывать.
    ///
    /// Оценка pace говорит, каким отставание должно быть; часы говорят, каково
    /// оно сейчас. Разница видна только под дрожанием сети, зато там она и есть
    /// вся ровность движения — подробности у самих часов.
    interpolation::ShowClock show;

    /// Расхождение, оставшееся от прошлого отрезка.
    ///
    /// Каждый новый отрезок начинается не там, где кончился прошлый: пока
    /// свежих снимков не было, движение достраивалось вперёд по скорости, а
    /// пришедший снимок возвращает тело туда, где оно было на самом деле.
    /// Разница запоминается здесь и гасится за четверть секунды — иначе каждая
    /// потеря пакета видна как рывок назад и тут же вперёд.
    shared::Vec3 seam;

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
    /// раз в сессию, а не каждый такт, — но не дошедший цвет сам собой
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

    /// Сообщает серверу о попадании по чужой машине.
    ///
    /// Отдельно от попадания по человеку, а не одним сообщением на оба: у машины
    /// три прочности, а не одна, и ведут они себя по-разному. Да и путь у них
    /// разный — урон человеку сервер применяет сам, а прочность машины отнимает
    /// её ведущий.
    void reportVehicleDamage(shared::VehicleId vehicle, const shared::VehicleHarm& harm,
                             std::uint32_t weapon);

    /// Сообщает о своём выстреле.
    ///
    /// Выстрел — событие, и снимком его не передать: между двумя снимками
    /// помещается три выстрела из автомата, а признак «стреляет» говорит лишь
    /// «жмёт на спуск».
    void reportShot(std::uint32_t weapon, const shared::Vec3& target);

    /// Выстрелы, о которых сообщил сервер.
    [[nodiscard]] std::vector<shared::WeaponFired> takeShots();

    /// Как собрано оружие у чужих игроков.
    [[nodiscard]] std::vector<shared::PlayerWeapon> takeWeaponLooks();

    /// Забирает пришедшие строки чата. Каждая отдаётся ровно один раз.
    [[nodiscard]] std::vector<shared::ChatLine> takeChatLines();

    /// Забирает пришедшие сообщения о попаданиях по нам.
    [[nodiscard]] std::vector<shared::DamageTaken> takeDamage();

    /// Забирает точки, в которые сервер велел перенести игрока.
    [[nodiscard]] std::vector<shared::Vec3> takeTeleports();

    /// Чем сервер распорядился о нашем теле. Не забирается, а спрашивается:
    /// накладывать это нужно каждый кадр.
    [[nodiscard]] std::uint8_t control() const noexcept { return control_; }

    /// Замки машин, о которых сказал сервер с прошлого раза.
    [[nodiscard]] std::vector<shared::VehicleControl> takeVehicleControls();

    /// Распоряжения о дверях, пришедшие с прошлого раза.
    [[nodiscard]] std::vector<shared::VehicleDoors> takeVehicleDoors();

    /// Распоряжения о машинах: переставить и починить.
    ///
    /// Приходят только ведущему: машина живёт в игре у него, и сделать с ней
    /// что-либо может только она.
    [[nodiscard]] std::vector<shared::VehicleTeleport> takeVehicleTeleports();
    [[nodiscard]] std::vector<shared::VehicleRepair> takeVehicleRepairs();

    /// Попадания по машинам, которые ведём мы.
    [[nodiscard]] std::vector<shared::VehicleDamaged> takeVehicleDamage();

    /// Метки на карте: какими их задал сервер и какие он убрал.
    ///
    /// Расстоянием не отбираются: метка на то и метка, что видна на карте
    /// целиком. Сервер шлёт их разом при входе, а дальше — по изменению.
    [[nodiscard]] std::vector<shared::BlipState> takeBlips();
    [[nodiscard]] std::vector<shared::BlipId> takeRemovedBlips();

    /// Маркеры и контрольные точки — тем же порядком и по той же причине.
    ///
    /// Расстоянием их не отбирает сервер: у обоих своё поле видимости, и
    /// считает его тот, кто рисует.
    /// Прохожие, о которых объявил сервер, и те, кого он убрал.
    ///
    /// Одним списком на «заведи» и «поправь»: сообщение у них одно, и разбирать
    /// его надвое здесь незачем — знает ли клиент эту куклу, решает он сам.
    [[nodiscard]] std::vector<shared::PedState> takePeds();
    [[nodiscard]] std::vector<shared::PedId> takeRemovedPeds();

    /// Привязки сущностей, присланные сервером.
    ///
    /// Состояние, а не события, но забираются они списком: за один такт может
    /// прийти и десяток — при входе сервер пересказывает вошедшему все разом.
    [[nodiscard]] std::vector<shared::EntityAttachment> takeAttachments();

    [[nodiscard]] std::vector<shared::MarkerState> takeMarkers();
    [[nodiscard]] std::vector<shared::MarkerId> takeRemovedMarkers();

    [[nodiscard]] std::vector<shared::CheckpointState> takeCheckpoints();
    [[nodiscard]] std::vector<shared::CheckpointId> takeRemovedCheckpoints();

    /// Движения, которые сервер велел сыграть.
    ///
    /// Приходят и про нас самих, и про чужих: персонаж живёт в игре у хозяина,
    /// но показан он у каждого — куклой, которую ведут снимки.
    [[nodiscard]] std::vector<shared::PlayerAnimation> takeAnimations();

    /// Взрывы, которые устроил сервер.
    ///
    /// Забираются и исполняются разом, а не откладываются, как движения: у
    /// движения есть цель, которой может ещё не быть в игре, а взрыв случается
    /// на пустом месте — ему ждать нечего.
    [[nodiscard]] std::vector<shared::Explosion> takeExplosions();

    /// Куда сервер велел сесть. Приходит только нам самим.
    [[nodiscard]] std::vector<shared::PlayerIntoVehicle> takeSeats();

    /// Забирает пришедшие от сервера именованные события.
    ///
    /// Клиент их не толкует: имя и нагрузку сочиняет ресурс сервера, а здесь
    /// они лишь передаются дальше — странице интерфейса.
    [[nodiscard]] std::vector<shared::ServerEvent> takeServerEvents();

    /// Забирает пришедшие описания внешности чужих машин.
    [[nodiscard]] std::vector<shared::VehicleAppearance> takeVehicleAppearances();

    /// Внешность чужих игроков, пришедшая с прошлого раза.
    [[nodiscard]] std::vector<shared::PlayerAppearance> takePlayerAppearances();

    /// Объявляет серверу, как выглядит наш игрок.
    ///
    /// Отправкой сразу, а не складыванием в снимок: снимок уходит по
    /// ненадёжному каналу каждый такт, а внешность обязана дойти и
    /// меняется редко.
    void sendAppearance(const shared::PlayerAppearance& appearance);

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

    /// Сколько осталось до ближайшей собственной отправки.
    ///
    /// Дольше этого ждать событий нельзя: ожидание кончается на пришедшем
    /// пакете, а слать нужно по расписанию, и эти два мгновения не совпадают.
    /// Свои снимки уходили бы вразнобой, а получатель платил бы за это
    /// отставанием — он считает его по дрожанию промежутков.
    [[nodiscard]] std::chrono::milliseconds untilDue() const;

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

    /// Внешность чужих игроков, пришедшая с прошлого опроса.
    std::vector<shared::PlayerAppearance> playerAppearances_;

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

    /// Замки машин, о которых сказал сервер. Ключ — номер машины.
    ///
    /// Списком, а не очередью, и по той же причине, что и у распоряжения о теле:
    /// замок приходит целиком и заменяет прежний. Копи мы их очередью,
    /// запертая и тут же отпертая машина осталась бы запертой.
    std::vector<shared::VehicleControl> vehicleControls_;

    /// Распоряжения о дверях. Тем же списком и по той же причине: важно
    /// последнее о каждой машине, а не все по очереди.
    std::vector<shared::VehicleDoors> vehicleDoors_;

    /// Чем сервер распорядился о нашем теле: набор shared::PlayerControlFlag.
    ///
    /// Состояние, а не очередь: распоряжение приходит целиком и заменяет
    /// прежнее. Копи мы их списком, замороженный и тут же отпущенный игрок
    /// увидел бы оба распоряжения по очереди — и остался бы замороженным.
    std::uint8_t control_ = 0;
    std::vector<shared::VehicleTeleport> vehicleTeleports_;
    std::vector<shared::VehicleRepair> vehicleRepairs_;
    std::vector<shared::BlipState> blips_;
    std::vector<shared::BlipId> removedBlips_;
    std::vector<shared::PlayerAnimation> animations_;
    std::vector<shared::Explosion> explosions_;
    std::vector<shared::WeaponFired> shots_;
    std::vector<shared::PlayerWeapon> weaponLooks_;
    std::vector<shared::PedState> peds_;
    std::vector<shared::PedId> removedPeds_;
    std::vector<shared::EntityAttachment> attachments_;
    std::vector<shared::MarkerState> markers_;
    std::vector<shared::MarkerId> removedMarkers_;
    std::vector<shared::CheckpointState> checkpoints_;
    std::vector<shared::CheckpointId> removedCheckpoints_;
    std::vector<shared::PlayerIntoVehicle> seats_;
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
    std::vector<shared::VehicleDamageReport> outgoingVehicleDamage_;
    std::vector<shared::VehicleDamaged> vehicleDamage_;
    std::vector<shared::WeaponFired> outgoingShots_;
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

    /// Снимок, который ушёл последним, и когда это было.
    ///
    /// Хранится ради того, чтобы не слать одно и то же: стоящий на месте
    /// человек шлёт неизменный снимок тридцать раз в секунду, а сервер
    /// пересылает его каждому соседу.
    shared::PlayerState sentState_;
    Clock::time_point stateSentAt_{};

    /// Снимок машины, который ушёл последним, и когда это было.
    ///
    /// По нему решается, стоит ли слать следующий: стоящая машина шлёт одно и то
    /// же каждый такт, а платит за него каждый, кто её видит.
    struct SentVehicle {
        shared::VehicleState state;
        Clock::time_point at{};
    };

    std::unordered_map<shared::VehicleId, SentVehicle> sentVehicles_;

    /// Промежуток между снимками своего состояния.
    ///
    /// Не постоянная: частоту называет сервер в приветствии, и до него она
    /// неизвестна. Начальное значение — тридцать снимков в секунду, столько же,
    /// сколько тактов у сервера по умолчанию; оно доживает ровно до первого
    /// ServerWelcome.
    std::chrono::microseconds stateInterval_{1'000'000 / shared::kDefaultTickRate};

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
