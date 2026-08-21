#include <oxymp/client/connection.hpp>

#include "interpolation.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace oxymp::client {
namespace {

/// Как часто отправляется проверка связи.
constexpr auto kPingInterval = std::chrono::seconds{2};

/// Как часто уходит снимок своего состояния.
///
/// Двадцать раз в секунду: чаще — лишний трафик, реже — интерполяция начинает
/// заметно отставать от настоящего движения.
constexpr auto kStateInterval = std::chrono::milliseconds{50};

/// Отстал ли пришедший снимок от уже принятого.
///
/// Отметки времени ходят по кругу, поэтому сравниваются не как числа, а как
/// последовательные номера: берётся беззнаковая разница, и если она больше
/// половины круга — значит снимок не обогнал предыдущий, а отстал от него.
/// Обычное «меньше» на переходе счётчика через край решило бы, что весь
/// дальнейший поток пришёл из прошлого, и движение встало бы намертво.
///
/// Одинаковые отметки тоже считаются отставшими: это тот же снимок, пришедший
/// дважды, а промежуток нулевой длины ничего не описывает.
[[nodiscard]] bool stale(shared::Timestamp accepted, shared::Timestamp arrived) noexcept {
    constexpr std::uint32_t kHalfCircle = 0x8000'0000U;

    const std::uint32_t ahead = shared::elapsedSince(accepted, arrived);
    return ahead == 0 || ahead >= kHalfCircle;
}

/// Границы задержки между попытками подключения.
constexpr auto kMinRetryDelay = std::chrono::milliseconds{500};
constexpr auto kMaxRetryDelay = std::chrono::seconds{10};

/// Сколько молчания сервера считается разрывом.
///
/// Считается здесь, а не транспортом, и это не дублирование. Пороги транспорта
/// нарочно подняты до полуминуты: клиент живёт внутри процесса игры, и пока GTA
/// грузится, его сетевой поток не получает управления секундами — на коротких
/// порогах соединение рвалось прямо на загрузочном экране. Опустить их обратно
/// значило бы вернуть ту поломку.
///
/// Но игроку, у которого сервер умер, ждать полминуты нельзя: он всё это время
/// ходит по мёртвому миру, не зная об этом. Отсюда второй, короткий счёт —
/// здесь, где известно то, чего не знает транспорт: работал ли в это время сам
/// клиент.
///
/// Пять секунд — это два с половиной пропущенных обмена ping-pong. Меньше брать
/// нельзя: одиночная потеря пакета не должна выглядеть смертью сервера.
constexpr auto kServerSilenceLimit = std::chrono::seconds{5};

/// Промежуток между двумя оборотами цикла, после которого он считается
/// проспавшим.
///
/// Тишина засчитывается только за то время, что мы её слушали. Проспал поток
/// двадцать секунд на подгрузке мира — эти двадцать секунд не в счёт: сервер мог
/// говорить всё это время, а не услышали мы. Без этой поправки первая же тяжёлая
/// подгрузка объявляла бы разрыв на ровном месте.
constexpr auto kLoopStall = std::chrono::milliseconds{500};

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
        return "имя недопустимо";
    case shared::RejectReason::NicknameTaken:
        return "имя занято";
    case shared::RejectReason::WrongPassword:
        return "пароль сервера неверен";
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

shared::PlayerState RemotePlayer::at(std::chrono::steady_clock::time_point now) const {
    if (snapshots == 0) {
        return {};
    }

    // Снимок один: отрезка ещё нет, смешивать не с чем. Показываем как есть —
    // это верно ровно один раз, до прихода второго.
    if (snapshots < 2) {
        return latest;
    }

    // Всё, кроме плавно меняющегося, берётся из последнего снимка как есть:
    // достраивать по времени имеет смысл только то, что меняется плавно, а
    // «целится» и «стреляет» плавно не меняются.
    shared::PlayerState state = latest;

    const auto blend = interpolation::blend(
        std::chrono::milliseconds{shared::elapsedSince(previous.sentAt, latest.sentAt)},
        now - latestAt);

    if (blend.ahead > 0.0F) {
        state.position = interpolation::advance(latest.position, latest.velocity, blend.ahead);
        return state;
    }

    state.position = interpolation::mix(previous.position, latest.position, blend.progress);
    state.heading = interpolation::mixAngle(previous.heading, latest.heading, blend.progress);
    state.aimAt = interpolation::mix(previous.aimAt, latest.aimAt, blend.progress);

    return state;
}

shared::VehicleState SessionVehicle::at(std::chrono::steady_clock::time_point now) const {
    // Машину без ведущего считать не по чему и незачем: снимков о ней больше не
    // будет, и достраивать движение — значит уводить стоящую машину в сторону
    // по последней запомненной скорости. То же и пока снимков меньше двух:
    // отрезка нет, показываем объявленное состояние как есть.
    if (snapshots < 2 || owner == shared::kInvalidPlayerId) {
        return latest;
    }

    // Тот же расчёт, что и у игрока, и по той же причине: снимки приходят реже
    // кадров. Разница в том, что машина проходит между снимками не полметра, а
    // десяток, — и отставание на ней видно вдесятеро отчётливее.
    shared::VehicleState state = latest;

    const auto blend = interpolation::blend(
        std::chrono::milliseconds{shared::elapsedSince(previous.sentAt, latest.sentAt)},
        now - latestAt);

    if (blend.ahead > 0.0F) {
        state.position = interpolation::advance(latest.position, latest.velocity, blend.ahead);
        state.rotation =
            interpolation::advanceAngles(latest.rotation, latest.angularVelocity, blend.ahead);
        return state;
    }

    state.position = interpolation::mix(previous.position, latest.position, blend.progress);
    state.rotation = interpolation::mixAngles(previous.rotation, latest.rotation, blend.progress);
    state.velocity = interpolation::mix(previous.velocity, latest.velocity, blend.progress);
    state.steer = std::lerp(previous.steer, latest.steer, blend.progress);

    return state;
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
    sendQueued();

    noticeSilence();
}

void Connection::noticeSilence() {
    const auto now = Clock::now();

    // Сколько мы отсутствовали между этим оборотом и прошлым. Всё, что дольше
    // обычного, — не тишина сервера, а наше беспамятство, и отсчёт сдвигается
    // на эту величину.
    if (loopSeenAt_ != Clock::time_point{}) {
        const auto asleep = now - loopSeenAt_;

        if (asleep > kLoopStall && heardAt_ != Clock::time_point{}) {
            heardAt_ += asleep;
        }
    }

    loopSeenAt_ = now;

    if (state_ != ConnectionState::Connected || heardAt_ == Clock::time_point{}) {
        return;
    }

    if (now - heardAt_ < kServerSilenceLimit) {
        return;
    }

    // Соединение транспорт всё ещё считает живым — он ждёт дольше. Разрываем
    // сами: сервер, молчащий пять секунд подряд, для игры уже мёртв, а
    // повторные попытки начнутся тем раньше, чем раньше мы это признаем.
    disconnect_ = DisconnectReason::Lost;
    fallBackToWaiting("сервер замолчал");
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
        // Игрока, который был в сессии, разрыв выбрасывает из игры — и об этом
        // ему говорят сразу, окном поверх всего. Разрыв на пути к сессии — дело
        // другое: там ещё нечего терять, и о ходе подключения рассказывает экран
        // загрузки.
        if (state_ == ConnectionState::Connected) {
            disconnect_ = DisconnectReason::Lost;
        }

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
    // Отметка ставится на любом пакете, а не только на ответе ping. Сервер,
    // рассылающий снимки, говорит с нами постоянно, и ждать от него отдельного
    // подтверждения жизни, когда он и так не молчит, незачем.
    heardAt_ = Clock::now();

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

            // Машины ушедшего остаются, и это перемена. Машина больше не
            // числится за игроком — у неё свой номер, и в ней могли остаться
            // пассажиры. Она уйдёт сама, когда о ней перестанут приходить
            // снимки, то есть когда её и правда некому станет вести.
            spdlog::info("игрок id {} вышел", left->playerId);
        }
        return;

    case shared::MessageId::PlayerState:
        if (const auto state = shared::decode<shared::PlayerState>(packet)) {
            handleRemoteState(*state);
        }
        return;

    case shared::MessageId::PlayerStates:
        if (const auto states = shared::decode<shared::PlayerStates>(packet)) {
            // Связка разбирается снимок за снимком тем же путём, что и
            // одиночный: разница между ними — только в том, как они доехали.
            for (const shared::PlayerState& state : states->players) {
                handleRemoteState(state);
            }
        }
        return;

    case shared::MessageId::VehicleState:
        if (const auto state = shared::decode<shared::VehicleState>(packet)) {
            handleRemoteVehicle(*state);
        }
        return;

    case shared::MessageId::VehicleAppearance:
        if (const auto appearance = shared::decode<shared::VehicleAppearance>(packet)) {
            vehicleAppearances_.push_back(*appearance);
        }
        return;

    case shared::MessageId::PlayerAppearance:
        if (const auto appearance = shared::decode<shared::PlayerAppearance>(packet)) {
            playerAppearances_.push_back(*appearance);
        }
        return;

    case shared::MessageId::VehicleAdded:
        if (const auto added = shared::decode<shared::VehicleAdded>(packet)) {
            handleVehicleAdded(*added);
        }
        return;

    case shared::MessageId::VehicleRemoved:
        if (const auto removed = shared::decode<shared::VehicleRemoved>(packet)) {
            vehicles_.erase(removed->id);
            sentAppearances_.erase(removed->id);
        }
        return;

    case shared::MessageId::VehicleAuthority:
        if (const auto authority = shared::decode<shared::VehicleAuthority>(packet)) {
            handleVehicleAuthority(*authority);
        }
        return;

    case shared::MessageId::WorldState:
        if (auto state = shared::decode<shared::WorldState>(packet)) {
            world_ = std::move(*state);
        }
        return;

    case shared::MessageId::PlayerLoadout:
        if (auto loadout = shared::decode<shared::PlayerLoadout>(packet)) {
            loadout_ = std::move(*loadout);
        }
        return;

    case shared::MessageId::HealthChanged:
        if (const auto health = shared::decode<shared::HealthChanged>(packet)) {
            health_ = *health;
        }
        return;

    case shared::MessageId::ObjectAdded:
        if (const auto object = shared::decode<shared::ObjectAdded>(packet)) {
            objects_.push_back(*object);
        }
        return;

    case shared::MessageId::ObjectRemoved:
        if (const auto object = shared::decode<shared::ObjectRemoved>(packet)) {
            removedObjects_.push_back(object->id);
        }
        return;

    case shared::MessageId::ChatLine:
        if (auto line = shared::decode<shared::ChatLine>(packet)) {
            spdlog::info("чат: {}", line->text);
            chatLines_.push_back(std::move(*line));
        }
        return;

    case shared::MessageId::DamageTaken:
        if (const auto taken = shared::decode<shared::DamageTaken>(packet)) {
            damage_.push_back(*taken);
        }
        return;

    case shared::MessageId::PlayerTeleport:
        if (const auto teleport = shared::decode<shared::PlayerTeleport>(packet)) {
            teleports_.push_back(teleport->position);
        }
        return;

    case shared::MessageId::VehicleTeleport:
        if (const auto teleport = shared::decode<shared::VehicleTeleport>(packet)) {
            vehicleTeleports_.push_back(*teleport);
        }
        return;

    case shared::MessageId::VehicleRepair:
        if (const auto repair = shared::decode<shared::VehicleRepair>(packet)) {
            vehicleRepairs_.push_back(*repair);
        }
        return;

    case shared::MessageId::BlipState:
        if (auto blip = shared::decode<shared::BlipState>(packet)) {
            blips_.push_back(std::move(*blip));
        }
        return;

    case shared::MessageId::BlipRemoved:
        if (const auto removed = shared::decode<shared::BlipRemoved>(packet)) {
            removedBlips_.push_back(removed->id);
        }
        return;

    case shared::MessageId::ServerEvent:
        if (auto event = shared::decode<shared::ServerEvent>(packet)) {
            serverEvents_.push_back(std::move(*event));
        }
        return;

    // Деньги сервер присылает, а клиенту их показать негде: уголка интерфейса
    // больше нет, и заводить ради одного числа свой — значит заводить клиенту
    // правила игры. Показывать деньги будет страница игрового режима, когда
    // клиентская часть ресурсов появится; до тех пор сообщение принимается и
    // молча откладывается.
    //
    // Ветка не удалена нарочно: без неё разбор упёрся бы в предупреждение о
    // неразобранном значении перечисления, а это тот самый случай, когда
    // молчание должно быть написано явно.
    case shared::MessageId::MoneyChanged:
        return;

    case shared::MessageId::ResourceList:
        if (auto list = shared::decode<shared::ResourceList>(packet)) {
            resources_ = std::move(list->entries);
        }
        return;

    // Это сообщения клиента. Сервер их не присылает.
    case shared::MessageId::ClientHello:
    case shared::MessageId::Ping:
    case shared::MessageId::ChatSay:
    case shared::MessageId::DamageReport:
    case shared::MessageId::ClientEvent:
        spdlog::warn("сервер прислал клиентское сообщение");
        return;
    }
}

void Connection::handleWelcome(const shared::ServerWelcome& welcome) {
    localPlayerId_ = welcome.playerId;
    spawnPosition_ = welcome.spawnPosition;
    state_ = ConnectionState::Connected;

    // Прошлый отказ и прошлый разрыв больше не описывают происходящее: нас
    // приняли. Отсюда и снимается окно разрыва — само собой, без отдельного
    // распоряжения: игрок снова в сессии, и рассказывать ему больше не о чем.
    rejectReason_.reset();
    disconnect_ = DisconnectReason::None;

    // Подключение удалось — следующая неудача должна начинать отсчёт заново,
    // а не с накопленной задержки.
    retryDelay_ = kMinRetryDelay;
    nextPingAt_ = Clock::now();
    nextStateAt_ = Clock::now();

    serverName_ = welcome.name;

    spdlog::info("сервер принял: наш id {}, темп {} тактов в секунду, имя \"{}\"",
                 welcome.playerId, welcome.tickRate, welcome.name);
}

void Connection::handleReject(const shared::ServerReject& reject) {
    rejectReason_ = reject.reason;

    // Занятое имя проходит само: почти всегда это наше же прошлое соединение,
    // которое сервер ещё не успел похоронить. Прекращать попытки здесь значило
    // бы не вернуться в игру после единственного разрыва связи.
    if (reject.reason == shared::RejectReason::NicknameTaken) {
        spdlog::warn("сервер отказал: {}", describe(reject.reason));
        fallBackToWaiting("имя пока занято");
        return;
    }

    state_ = ConnectionState::Rejected;
    disconnect_ = DisconnectReason::Refused;
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

    // Снимок, отставший от уже принятого, отбрасывается. Канал ненадёжный и
    // порядка не обещает, а принятый задом наперёд снимок отматывал бы игрока
    // назад — и следующий тут же дёргал бы его обратно вперёд.
    if (player.snapshots > 0 && stale(player.latest.sentAt, state.sentAt)) {
        return;
    }

    player.previous = player.snapshots > 0 ? player.latest : state;
    player.latest = state;
    player.latestAt = Clock::now();
    ++player.snapshots;
}

void Connection::handleRemoteVehicle(const shared::VehicleState& state) {
    // Снимок машины, о существовании которой нам не говорили, отбрасывается.
    // Заводить машину по снимку больше нельзя: о появлении машин объявляет
    // сервер, и он же говорит, кто их ведёт. Машина, заведённая здесь, осталась
    // бы без ведущего навсегда — и никогда не сдвинулась бы с места.
    //
    // Случай не выдуманный: снимок идёт по ненадёжному каналу и обгоняет
    // объявление, идущее по надёжному. Потеря такого снимка ничего не стоит —
    // следующий придёт через полсотни миллисекунд, уже после объявления.
    const auto known = vehicles_.find(state.id);
    if (known == vehicles_.end()) {
        return;
    }

    SessionVehicle& vehicle = known->second;

    if (vehicle.snapshots > 0 && stale(vehicle.latest.sentAt, state.sentAt)) {
        return;
    }

    vehicle.previous = vehicle.snapshots > 0 ? vehicle.latest : state;
    vehicle.latest = state;
    vehicle.latestAt = Clock::now();
    ++vehicle.snapshots;
}

void Connection::handleVehicleAdded(const shared::VehicleAdded& added) {
    if (added.state.id == shared::kInvalidVehicleId) {
        return;
    }

    const auto now = Clock::now();

    // Оба снимка сразу и одинаковые: машина объявлена стоящей там, где она
    // есть, и считать между ними нечего, пока не придёт первый настоящий
    // снимок. Оставь мы их пустыми — машина на мгновение оказалась бы в начале
    // координат.
    SessionVehicle& vehicle = vehicles_[added.state.id];
    vehicle.previous = added.state;
    vehicle.latest = added.state;
    vehicle.latestAt = now;
    vehicle.owner = added.owner;

    // Настоящих снимков ещё не было: то, что пришло, — объявление, а не снимок.
    // Счёт нужен, чтобы не считать движение между двумя одинаковыми точками.
    vehicle.snapshots = 0;

    spdlog::debug("в сессии появилась машина {}, ведёт её {}", added.state.id,
                  added.owner == shared::kInvalidPlayerId ? -1 : static_cast<int>(added.owner));
}

void Connection::handleVehicleAuthority(const shared::VehicleAuthority& authority) {
    const auto known = vehicles_.find(authority.id);
    if (known == vehicles_.end()) {
        return;
    }

    if (known->second.owner != authority.owner) {
        // Отсчёт начинается заново: отметки времени нового ведущего с отметками
        // прежнего несравнимы — часы у них свои. Смешай мы снимок одного со
        // снимком другого, промежуток вышел бы любой длины, вплоть до
        // сорока девяти суток.
        known->second.snapshots = 0;
    }

    known->second.owner = authority.owner;

    // Внешность машины, которая перестала быть нашей, забывается как
    // отправленная. Вернись она к нам обратно — и мы объявим её заново: за то
    // время, что машину вёл другой, он мог её перекрасить.
    if (authority.owner != localPlayerId_) {
        sentAppearances_.erase(authority.id);
    }
}

void Connection::setLocalState(const shared::PlayerState& state) {
    localState_ = state;
    localState_.playerId = shared::kInvalidPlayerId;
}

void Connection::setOwnedVehicles(std::vector<shared::VehicleState> vehicles) {
    ownedVehicles_ = std::move(vehicles);
}

void Connection::setOwnedAppearances(std::vector<shared::VehicleAppearance> appearances) {
    ownedAppearances_ = std::move(appearances);
}

std::optional<shared::WorldState> Connection::takeWorld() {
    return std::exchange(world_, std::nullopt);
}

std::optional<shared::PlayerLoadout> Connection::takeLoadout() {
    return std::exchange(loadout_, std::nullopt);
}

std::optional<shared::HealthChanged> Connection::takeHealth() {
    return std::exchange(health_, std::nullopt);
}

std::vector<shared::ObjectAdded> Connection::takeObjects() {
    return std::exchange(objects_, {});
}

std::vector<shared::ObjectId> Connection::takeRemovedObjects() {
    return std::exchange(removedObjects_, {});
}

std::vector<shared::VehicleAppearance> Connection::takeVehicleAppearances() {
    return std::exchange(vehicleAppearances_, {});
}

std::vector<shared::PlayerAppearance> Connection::takePlayerAppearances() {
    return std::exchange(playerAppearances_, {});
}

void Connection::sendAppearance(const shared::PlayerAppearance& appearance) {
    if (state_ != ConnectionState::Connected) {
        return;
    }

    // Надёжным каналом: внешность обязана дойти целиком, а приходит она раз в
    // час. Потерянная, она оставила бы игрока без штанов до следующей смены
    // одежды — и объяснить это было бы нечем.
    const auto packet = shared::encode(appearance);
    host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
}

void Connection::emit(std::string name, std::string payload) {
    if (name.empty()) {
        return;
    }

    shared::ClientEvent event;
    event.name = std::move(name);
    event.payload = std::move(payload);

    outgoingEvents_.push_back(std::move(event));
}

void Connection::say(std::string text) {
    if (text.empty()) {
        return;
    }

    shared::ChatSay say;
    say.text = std::move(text);
    outgoingChat_.push_back(std::move(say));
}

void Connection::reportDamage(shared::PlayerId victim, std::uint16_t amount,
                              std::uint32_t weapon) {
    if (victim == shared::kInvalidPlayerId || amount == 0) {
        return;
    }

    shared::DamageReport report;
    report.victim = victim;
    report.amount = amount;
    report.weapon = weapon;

    outgoingDamage_.push_back(report);
}

std::vector<shared::ChatLine> Connection::takeChatLines() {
    return std::exchange(chatLines_, {});
}

std::vector<shared::DamageTaken> Connection::takeDamage() {
    return std::exchange(damage_, {});
}

std::vector<shared::Vec3> Connection::takeTeleports() {
    return std::exchange(teleports_, {});
}

std::vector<shared::VehicleTeleport> Connection::takeVehicleTeleports() {
    return std::exchange(vehicleTeleports_, {});
}

std::vector<shared::VehicleRepair> Connection::takeVehicleRepairs() {
    return std::exchange(vehicleRepairs_, {});
}

std::vector<shared::BlipState> Connection::takeBlips() {
    return std::exchange(blips_, {});
}

std::vector<shared::BlipId> Connection::takeRemovedBlips() {
    return std::exchange(removedBlips_, {});
}

std::vector<shared::ServerEvent> Connection::takeServerEvents() {
    return std::exchange(serverEvents_, {});
}

std::optional<std::vector<shared::ResourceEntry>> Connection::takeResources() {
    return std::exchange(resources_, std::nullopt);
}

void Connection::sendHello() {
    shared::ClientHello hello;
    hello.protocolVersion = shared::kProtocolVersion;
    hello.nickname = settings_.nickname;
    hello.password = settings_.password;

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

    // Отметка ставится здесь, при отправке, а не при снятии снимка в игровом
    // потоке. Разница есть: снимки снимаются каждый кадр, а уходит раз в
    // пятьдесят миллисекунд последний из них, и отметка должна описывать то, что
    // ушло. Одна на всё, что уходит в этот раз, — они и описывают одно мгновение.
    const auto sentAt = static_cast<shared::Timestamp>(nowMilliseconds());

    // Машины уходят раньше своего водителя, и это не мелочь: получатель сажает
    // игрока в машину, а посадить его некуда, пока о машине не сказано.
    for (shared::VehicleState vehicle : ownedVehicles_) {
        vehicle.sentAt = sentAt;

        const auto vehiclePacket = shared::encode(vehicle);
        host_->send(serverPeer_, shared::Channel::State, shared::ByteView{vehiclePacket});
    }

    localState_.sentAt = sentAt;

    const auto packet = shared::encode(localState_);
    host_->send(serverPeer_, shared::Channel::State, shared::ByteView{packet});

    nextStateAt_ = Clock::now() + kStateInterval;

    sendAppearancesIfChanged();
}

void Connection::sendAppearancesIfChanged() {
    for (const shared::VehicleAppearance& appearance : ownedAppearances_) {
        const auto sent = sentAppearances_.find(appearance.id);
        if (sent != sentAppearances_.end() && sent->second == appearance) {
            continue;
        }

        // По надёжному каналу, в отличие от снимков рядом. Разница не в
        // важности, а в том, что происходит с потерянным сообщением: потерянный
        // снимок заменит следующий через полсотни миллисекунд, а потерянный цвет
        // не заменит ничто — он больше не изменится.
        const auto packet = shared::encode(appearance);
        host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});

        sentAppearances_[appearance.id] = appearance;
    }
}

void Connection::sendQueued() {
    if (state_ != ConnectionState::Connected) {
        // Отправлять некуда, а копить бессмысленно: реплика, доставленная через
        // минуту после переподключения, уже никому не нужна.
        outgoingChat_.clear();
        outgoingDamage_.clear();
        outgoingEvents_.clear();
        return;
    }

    // Оба надёжным каналом: реплика чата, потерянная по дороге, не повторится
    // никогда, а потерянное попадание превращает перестрелку в недоразумение.
    for (const shared::ChatSay& say : outgoingChat_) {
        const auto packet = shared::encode(say);
        host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
    }
    outgoingChat_.clear();

    for (const shared::DamageReport& report : outgoingDamage_) {
        const auto packet = shared::encode(report);
        host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
    }
    outgoingDamage_.clear();

    // Тем же надёжным каналом: потерянное нажатие не повторится, а игрок
    // увидит, что оно пропало впустую.
    for (const shared::ClientEvent& event : outgoingEvents_) {
        const auto packet = shared::encode(event);
        host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
    }
    outgoingEvents_.clear();
}

void Connection::fallBackToWaiting(std::string_view reason) {
    if (state_ == ConnectionState::Rejected) {
        return;
    }

    spdlog::warn("{}, повтор через {} мс", reason, retryDelay_.count());

    host_.reset();
    serverPeer_ = net::kInvalidPeerId;
    localPlayerId_ = shared::kInvalidPlayerId;
    spawnPosition_.reset();
    remotePlayers_.clear();

    // Машины забываются вместе с соединением: их список принадлежит серверу, и
    // после переподключения он расскажет о них заново — с теми номерами и
    // ведущими, какие будут к тому времени.
    vehicles_.clear();
    sentAppearances_.clear();
    teleports_.clear();
    serverEvents_.clear();

    // Предметы и снаряжение забываются вместе с соединением: их список
    // принадлежит серверу, и после переподключения он расскажет о них заново.
    objects_.clear();
    removedObjects_.clear();
    vehicleTeleports_.clear();
    vehicleRepairs_.clear();
    blips_.clear();
    removedBlips_.clear();
    loadout_.reset();
    health_.reset();
    latency_.reset();
    nextPingAt_ = Clock::time_point::max();
    nextStateAt_ = Clock::time_point::max();

    // Отсчёт молчания начинается заново с первым словом нового соединения:
    // тишина прошлого сервера к новому отношения не имеет.
    heardAt_ = Clock::time_point{};

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
