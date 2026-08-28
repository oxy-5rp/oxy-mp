#include <oxymp/client/connection.hpp>

#include <oxymp/client/interpolation.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace oxymp::client {
namespace {

/// Как часто отправляется проверка связи.
constexpr auto kPingInterval = std::chrono::seconds{2};

/// Как часто уходит снимок своего состояния, пока сервер не сказал иначе.
///
/// Своей постоянной здесь больше нет, и это главное. Прежде клиент слал ровно
/// двадцать снимков в секунду, а сервер рассылал их тридцать раз — то есть
/// каждый третий такт получателю нечего было сказать, и снимки приходили
/// неровно: два подряд, потом пропуск. Никакая интерполяция такой поток ровным
/// не сделает — она честно показывает то, что ей дали.
///
/// Частоту называет сервер в приветствии (ServerWelcome::tickRate). Чаще неё
/// слать бессмысленно — лишние снимки сервер выбросит, не разослав; реже —
/// значит оставлять его такты пустыми.
///
/// Названное сервером число приходит по сети, и верить ему на слово нельзя:
/// поделить на ноль или отвести снимку целую секунду не выйдет. Границы общие с
/// сервером — shared::kMinTickRate.
///
/// В микросекундах, как и такт у сервера, и по той же причине: шестьдесят
/// тактов это 16.67 миллисекунды, а округлённые до шестнадцати они дают 62.5
/// снимка в секунду. Округли одна сторона в свою сторону, другая в свою — и
/// снимки пошли бы вразнобой с тактами.
[[nodiscard]] std::chrono::microseconds stateInterval(std::uint16_t tickRate) noexcept {
    const std::uint16_t rate =
        std::clamp(tickRate, shared::kMinTickRate, shared::kMaxTickRate);

    return std::chrono::microseconds{1'000'000 / rate};
}


/// Как часто снимок уходит, даже когда в нём ничего не изменилось.
///
/// Стоящий на месте человек шлёт одно и то же тридцать раз в секунду, и это
/// самое частое, что бывает в сессии: очередь у магазина, толпа на площади,
/// отошедший от клавиатуры. Получателю с этих снимков нет никакой пользы — он и
/// так держит куклу на месте, — а платят за них все: сервер пересылает их
/// каждому соседу, то есть числом игроков в квадрате.
///
/// Совсем замолчать при этом нельзя, и четверть секунды — цена этого «нельзя».
/// Сравнение «изменилось ли что-нибудь» живёт в одном месте (worthSending) и
/// однажды в нём чего-нибудь недосчитается; тогда чужой игрок замрёт — но не
/// навсегда, а на четверть секунды. Заодно это тот запас, по которому получатель
/// отличает молчащего от пропавшего.
constexpr auto kStateKeepalive = std::chrono::milliseconds{250};

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
    // По-английски, и это тот же довод, по какому по-английски говорит лаунчер:
    // строка уходит разом в журнал и в окно поверх игры, а присланный снимок
    // окна и присланный журнал обязаны читаться как одно. Прежде здесь было
    // по-русски, а «Connection refused: » перед ней — по-английски, и в
    // присланном журнале это выглядело поломкой кодировки.
    switch (reason) {
    case shared::RejectReason::ProtocolMismatch:
        return "the server runs another protocol version";
    case shared::RejectReason::ServerFull:
        return "the server is full";
    case shared::RejectReason::InvalidNickname:
        return "that nickname is not allowed";
    case shared::RejectReason::NicknameTaken:
        return "that nickname is taken";
    case shared::RejectReason::WrongPassword:
        return "wrong server password";
    }
    return "no reason given";
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
    if (timeline.empty()) {
        return {};
    }

    const float aged = std::chrono::duration<float>{now - latestAt}.count();
    const auto sample = timeline.at(now - latestAt, show.lag());

    // Всё, кроме плавно меняющегося, берётся из того снимка, в который мы
    // пришли, а не из самого свежего. Разница есть, и она видна: показываем мы
    // мгновение позади настоящего, и признаки самого свежего снимка означали бы,
    // что кукла стреляет там, где её тело окажется только через полсотни
    // миллисекунд.
    shared::PlayerState state = *sample.to;

    if (sample.ahead > 0.0F) {
        state.position = interpolation::healed(
            interpolation::advance(sample.to->position, sample.to->velocity, sample.ahead), seam,
            aged);
        return state;
    }

    if (sample.from == sample.to) {
        // Снимок один — смешивать не с чем. Так бывает дважды: до прихода
        // второго снимка и когда лента не достаёт так далеко назад, как мы
        // показываем.
        state.position = interpolation::healed(sample.to->position, seam, aged);
        return state;
    }

    // По кривой, а не по прямой: скорости на концах отрезка приходят в самом
    // снимке, и построенная по ним кривая приходит в каждый снимок с той
    // скоростью, которую назвал хозяин. Прямая ломалась на каждом снимке — то
    // самое дрожание, которое оставалось после того, как отметки времени
    // расставили верно.
    state.position = interpolation::healed(
        interpolation::curve(sample.from->position, sample.from->velocity, sample.to->position,
                             sample.to->velocity, sample.span, sample.progress),
        seam, aged);
    state.velocity =
        interpolation::mix(sample.from->velocity, sample.to->velocity, sample.progress);
    state.heading =
        interpolation::mixAngle(sample.from->heading, sample.to->heading, sample.progress);
    state.aimAt = interpolation::mix(sample.from->aimAt, sample.to->aimAt, sample.progress);

    return state;
}

shared::VehicleState SessionVehicle::at(std::chrono::steady_clock::time_point now) const {
    if (timeline.empty()) {
        return {};
    }

    // Машину без ведущего считать не по чему и незачем: снимков о ней больше не
    // будет, и достраивать движение — значит уводить стоящую машину в сторону
    // по последней запомненной скорости. То же и пока снимков меньше двух:
    // отрезка нет, показываем объявленное состояние как есть.
    if (snapshots < 2 || owner == shared::kInvalidPlayerId) {
        return timeline.newest();
    }

    const float aged = std::chrono::duration<float>{now - latestAt}.count();
    const auto sample = timeline.at(now - latestAt, show.lag());

    shared::VehicleState state = *sample.to;

    if (sample.ahead > 0.0F) {
        state.position = interpolation::healed(
            interpolation::advance(sample.to->position, sample.to->velocity, sample.ahead), seam,
            aged);
        state.rotation = interpolation::advanceAngles(sample.to->rotation,
                                                      sample.to->angularVelocity, sample.ahead);
        return state;
    }

    if (sample.from == sample.to) {
        state.position = interpolation::healed(sample.to->position, seam, aged);
        return state;
    }

    // И положение, и поворот — по кривой: у машины видно и то и другое. Прямая
    // между двумя снимками положения срезает поворот — на скорости за тридцать с
    // небольшим миллисекунд машина проходит метр с лишним, — а прямая между
    // двумя снимками поворота проходит мимо заноса, который она отыграла за то
    // же время.
    state.position = interpolation::healed(
        interpolation::curve(sample.from->position, sample.from->velocity, sample.to->position,
                             sample.to->velocity, sample.span, sample.progress),
        seam, aged);
    state.rotation = interpolation::curveAngles(sample.from->rotation,
                                                sample.from->angularVelocity, sample.to->rotation,
                                                sample.to->angularVelocity, sample.span,
                                                sample.progress);
    state.velocity =
        interpolation::mix(sample.from->velocity, sample.to->velocity, sample.progress);
    state.angularVelocity = interpolation::mix(sample.from->angularVelocity,
                                               sample.to->angularVelocity, sample.progress);
    state.steer = std::lerp(sample.from->steer, sample.to->steer, sample.progress);

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

    // Ждать событий дольше, чем осталось до собственной отправки, нельзя.
    //
    // Ожидание кончается на первом же пришедшем пакете, и оттого казалось, что
    // всё в порядке: снимки приходят с частотой такта, значит и просыпаемся мы с
    // ней же. Но просыпаемся мы тогда, когда пришло, а слать обязаны тогда,
    // когда пора, — и эти два мгновения не совпадают. Свои снимки уходили
    // вразнобой: то на десять миллисекунд раньше срока, то на двадцать позже.
    //
    // Цена этому — чужое отставание. Получатель считает его по дрожанию: сеть,
    // доставляющая ровно, просит только промежуток, а дёрганая — тем больше, чем
    // сильнее её мотает. Замерено ботом: пятнадцать миллисекунд дрожания на
    // петле обратной связи, где сети нет вовсе, и сто миллисекунд отставания
    // вместо шестидесяти. Половину этого дрожания создавали мы сами.
    budget = std::min(budget, untilDue());

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

std::chrono::milliseconds Connection::untilDue() const {
    if (state_ != ConnectionState::Connected) {
        // Пока сессии нет, слать нечего: расписания тоже нет, и ограничивать
        // ожидание незачем.
        return std::chrono::milliseconds::max();
    }

    const auto due = std::min(nextStateAt_, nextPingAt_);
    const auto left = due - Clock::now();

    if (left <= Clock::duration::zero()) {
        return std::chrono::milliseconds{0};
    }

    // Вверх, а не вниз: округлив вниз, мы просыпались бы за долю миллисекунды до
    // срока и уходили ждать заново.
    return std::chrono::ceil<std::chrono::milliseconds>(left);
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
    fallBackToWaiting("the server went silent");
}

void Connection::beginAttempt() {
    std::string error;
    host_ = net::Host::connect(settings_.address, settings_.port, error);

    if (host_ == nullptr) {
        spdlog::warn("could not start connecting to {}:{}: {}", settings_.address,
                     settings_.port, error);
        fallBackToWaiting("the connection did not start");
        return;
    }

    state_ = ConnectionState::Connecting;
    spdlog::info("Connecting to {}:{}", settings_.address, settings_.port);
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
        fallBackToWaiting(state_ == ConnectionState::Connecting ? "the server is unreachable"
                                                                : "the connection was lost");
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
        spdlog::debug("unrecognised packet from the server ({} bytes)", payload.size());
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

            spdlog::debug("player \"{}\" (id {}) joined", joined->nickname,
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
            spdlog::debug("player id {} left", left->playerId);
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

    case shared::MessageId::VehicleStates:
        if (const auto states = shared::decode<shared::VehicleStates>(packet)) {
            // Как и связка игроков: разбирается снимок за снимком тем же путём,
            // что и одиночный. Разница между ними — только в том, как они
            // доехали.
            for (const shared::VehicleState& state : states->vehicles) {
                handleRemoteVehicle(state);
            }
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
            spdlog::debug("chat: {}", line->text);
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

    case shared::MessageId::VehicleControl:
        if (const auto control = shared::decode<shared::VehicleControl>(packet)) {
            // Прежнее распоряжение о той же машине заменяется, а не копится:
            // важно последнее, а не все по очереди.
            const auto known = std::ranges::find(vehicleControls_, control->id,
                                                 &shared::VehicleControl::id);

            if (known == vehicleControls_.end()) {
                vehicleControls_.push_back(*control);
            } else {
                *known = *control;
            }
        }
        return;

    case shared::MessageId::VehicleDoors:
        if (const auto doors = shared::decode<shared::VehicleDoors>(packet)) {
            const auto known = std::ranges::find(vehicleDoors_, doors->id,
                                                 &shared::VehicleDoors::id);

            if (known == vehicleDoors_.end()) {
                vehicleDoors_.push_back(*doors);
            } else {
                *known = *doors;
            }
        }
        return;

    case shared::MessageId::PlayerControl:
        if (const auto control = shared::decode<shared::PlayerControl>(packet)) {
            control_ = control->flags;
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

    case shared::MessageId::VehicleDamaged:
        if (const auto hit = shared::decode<shared::VehicleDamaged>(packet)) {
            vehicleDamage_.push_back(*hit);
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

    case shared::MessageId::Explosion:
        if (const auto explosion = shared::decode<shared::Explosion>(packet)) {
            explosions_.push_back(*explosion);
        }
        return;

    case shared::MessageId::WeaponFired:
        if (const auto fired = shared::decode<shared::WeaponFired>(packet)) {
            shots_.push_back(*fired);
        }
        return;

    case shared::MessageId::PlayerWeapon:
        if (auto look = shared::decode<shared::PlayerWeapon>(packet)) {
            weaponLooks_.push_back(std::move(*look));
        }
        return;

    case shared::MessageId::PlayerAnimation:
        if (auto animation = shared::decode<shared::PlayerAnimation>(packet)) {
            animations_.push_back(std::move(*animation));
        }
        return;

    case shared::MessageId::PedState:
        if (const auto ped = shared::decode<shared::PedState>(packet)) {
            peds_.push_back(*ped);
        }
        return;

    case shared::MessageId::PedRemoved:
        if (const auto removed = shared::decode<shared::PedRemoved>(packet)) {
            removedPeds_.push_back(removed->id);
        }
        return;

    case shared::MessageId::EntityAttachment:
        if (auto attachment = shared::decode<shared::EntityAttachment>(packet)) {
            attachments_.push_back(std::move(*attachment));
        }
        return;

    case shared::MessageId::MarkerState:
        if (const auto marker = shared::decode<shared::MarkerState>(packet)) {
            markers_.push_back(*marker);
        }
        return;

    case shared::MessageId::MarkerRemoved:
        if (const auto removed = shared::decode<shared::MarkerRemoved>(packet)) {
            removedMarkers_.push_back(removed->id);
        }
        return;

    case shared::MessageId::CheckpointState:
        if (const auto checkpoint = shared::decode<shared::CheckpointState>(packet)) {
            checkpoints_.push_back(*checkpoint);
        }
        return;

    case shared::MessageId::CheckpointRemoved:
        if (const auto removed = shared::decode<shared::CheckpointRemoved>(packet)) {
            removedCheckpoints_.push_back(removed->id);
        }
        return;

    case shared::MessageId::PlayerIntoVehicle:
        if (const auto seat = shared::decode<shared::PlayerIntoVehicle>(packet)) {
            seats_.push_back(*seat);
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
    case shared::MessageId::VehicleDamageReport:
    case shared::MessageId::ClientEvent:
        spdlog::debug("the server sent a client-only message");
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

    // Снимки уходят с той частотой, с какой сервер их рассылает: на каждый его
    // такт — ровно один наш снимок.
    stateInterval_ = stateInterval(welcome.tickRate);

    serverName_ = welcome.name;

    spdlog::info("Connected to \"{2}\" as id {0} ({1} ticks/s)",
                 welcome.playerId, welcome.tickRate, welcome.name);
}

void Connection::handleReject(const shared::ServerReject& reject) {
    rejectReason_ = reject.reason;

    // Занятое имя проходит само: отказ этот временный, и прекращать попытки
    // здесь значило бы не вернуться в игру после единственного разрыва связи.
    //
    // Наш сервер эту причину больше не присылает — двое с одним именем в сессии
    // ему не мешают, — но разбор её остаётся: клиент ходит и на серверы прежних
    // сборок, а там имя всё ещё требуется своим.
    if (reject.reason == shared::RejectReason::NicknameTaken) {
        spdlog::warn("Connection refused: {}", describe(reject.reason));
        fallBackToWaiting("that nickname is still taken");
        return;
    }

    state_ = ConnectionState::Rejected;
    disconnect_ = DisconnectReason::Refused;
    spdlog::error("Connection refused: {}", describe(reject.reason));
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

    const auto arrived = Clock::now();
    const bool known = !player.timeline.empty();

    // Где игрок показывался в это самое мгновение — до того, как лента
    // изменилась. Спрашивается до правки, потому что после неё этого уже не
    // узнать: расчёт пойдёт по другой паре снимков.
    const shared::Vec3 shown = known ? player.at(arrived).position : state.position;

    // Обогнанный снимок больше не выбрасывается: лента ставит его на своё место.
    // Выбрасывается только тот, что пришёл дважды или отстал за её начало, — с
    // ним и правда делать нечего.
    const shared::PlayerState before = known ? player.timeline.newest() : state;

    if (!player.timeline.accept(state)) {
        return;
    }

    const bool newest = player.timeline.newest().sentAt == state.sentAt;

    // Оценка отставания пополняется только со второго снимка: у первого нет ни
    // промежутка отправки, ни промежутка прихода — сравнивать его не с чем.
    // И только по снимку, который пришёл свежее всех: обогнанный говорит о
    // порядке доставки, а не о частоте отправки.
    //
    // И только по значащему. Снимок, в котором ничего не изменилось, шлётся
    // лишь потому, что молчать дольше нельзя, — раз в секунду вместо тридцати.
    // Прими мы его за мерило частоты, отставание у стоящего игрока уползло бы к
    // потолку, а спадало бы оттуда десяток секунд после того, как он пойдёт.
    if (known && newest && shared::differs(before, state)) {
        player.pace.notice(
            std::chrono::milliseconds{shared::elapsedSince(before.sentAt, state.sentAt)},
            arrived - player.latestAt);
    }

    // Часы показа двигаются по всякому свежему снимку, а не только по
    // значащему: опора отсчёта сместилась к нему в любом случае, и не сдвинь мы
    // часы — показываемое мгновение прыгнуло бы ровно на этот сдвиг.
    if (!known) {
        player.show.restart(player.pace.delay());
    } else if (newest) {
        player.show.slew(
            arrived - player.latestAt,
            std::chrono::milliseconds{shared::elapsedSince(before.sentAt, state.sentAt)},
            player.pace.delay());
    }

    // А вот время прихода обновляется по всякому свежему снимку, значащему или
    // нет: по нему получатель отличает молчащего от пропавшего.
    if (!known || newest) {
        player.latestAt = arrived;
    }

    ++player.snapshots;

    // Шов считается по чистому расчёту, поэтому прежний остаток сначала
    // снимается: иначе он вошёл бы в новый шов дважды.
    player.seam = shared::Vec3{};
    player.seam = interpolation::seamBetween(shown, player.at(arrived).position);
}

void Connection::handleRemoteVehicle(const shared::VehicleState& state) {
    // Снимок машины, о существовании которой нам не говорили, отбрасывается.
    // Заводить машину по снимку больше нельзя: о появлении машин объявляет
    // сервер, и он же говорит, кто их ведёт. Машина, заведённая здесь, осталась
    // бы без ведущего навсегда — и никогда не сдвинулась бы с места.
    //
    // Случай не выдуманный: снимок идёт по ненадёжному каналу и обгоняет
    // объявление, идущее по надёжному. Потеря такого снимка ничего не стоит —
    // следующий придёт в ближайшем такте, уже после объявления.
    const auto known = vehicles_.find(state.id);
    if (known == vehicles_.end()) {
        return;
    }

    SessionVehicle& vehicle = known->second;

    const auto arrived = Clock::now();
    const shared::Vec3 shown = vehicle.at(arrived).position;

    const bool counted = vehicle.snapshots > 0;
    const shared::VehicleState before =
        vehicle.timeline.empty() ? state : vehicle.timeline.newest();

    // Объявленное сервером состояние лежит на ленте первым, но снимком не
    // считается: часы его — не часы ведущего, и промежуток между ними ничего не
    // описывает. Поэтому лента заводится с него заново.
    if (!counted) {
        vehicle.timeline.restart(state);
    } else if (!vehicle.timeline.accept(state)) {
        return;
    }

    const bool newest = vehicle.timeline.newest().sentAt == state.sentAt;

    // Оценка отставания пополняется только по значащему снимку, как и у игрока,
    // и по той же причине: стоящая машина шлёт снимок раз в четверть секунды
    // лишь потому, что молчать дольше нельзя. Прими мы это за частоту — и
    // тронувшаяся с места машина показывалась бы на треть секунды позади ещё
    // десяток секунд после того, как поехала.
    if (counted && newest && shared::differs(before, state)) {
        vehicle.pace.notice(
            std::chrono::milliseconds{shared::elapsedSince(before.sentAt, state.sentAt)},
            arrived - vehicle.latestAt);
    }

    if (!counted) {
        vehicle.show.restart(vehicle.pace.delay());
    } else if (newest) {
        vehicle.show.slew(
            arrived - vehicle.latestAt,
            std::chrono::milliseconds{shared::elapsedSince(before.sentAt, state.sentAt)},
            vehicle.pace.delay());
    }

    if (!counted || newest) {
        vehicle.latestAt = arrived;
    }

    ++vehicle.snapshots;

    vehicle.seam = shared::Vec3{};
    vehicle.seam = interpolation::seamBetween(shown, vehicle.at(arrived).position);
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
    vehicle.timeline.restart(added.state);
    vehicle.latestAt = now;
    vehicle.owner = added.owner;

    // Настоящих снимков ещё не было: то, что пришло, — объявление, а не снимок.
    // Счёт нужен, чтобы не считать движение между двумя одинаковыми точками.
    vehicle.snapshots = 0;

    // Объявленная машина стоит там, где сказано, и ни от чего не отстаёт:
    // сглаживать нечего, а оставшийся от прошлой её жизни шов сдвинул бы её
    // мимо объявленного места. Часы показа по той же причине ставятся, а не
    // подтягиваются: подтягивать их не от чего.
    vehicle.seam = shared::Vec3{};
    vehicle.show.restart(vehicle.pace.delay());

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

        // Вместе с отсчётом забывается и оценка отставания: она описывала сеть
        // прежнего ведущего, а у нового своя. Оставленная, она заставила бы
        // машину показываться позади с чужой поправкой — и тем сильнее, чем
        // больше эти двое отличались.
        known->second.pace.forget();
        known->second.seam = shared::Vec3{};

        // И часы показа заодно: отсчитывали они от отметок прежнего ведущего, а
        // отметки нового с ними несравнимы — часы у них свои.
        known->second.show.restart(known->second.pace.delay());

        // От ленты остаётся один последний снимок: отметки нового ведущего с
        // отметками прежнего несравнимы — часы у них свои, — а показывать машину
        // до первого снимка нового ведущего иначе было бы нечем.
        known->second.timeline.keepNewest();
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

void Connection::reportVehicleDamage(shared::VehicleId vehicle, const shared::VehicleHarm& harm,
                                     std::uint32_t weapon) {
    if (vehicle == shared::kInvalidVehicleId || !harm.any()) {
        return;
    }

    shared::VehicleDamageReport report;
    report.vehicle = vehicle;
    report.harm = harm;
    report.weapon = weapon;

    outgoingVehicleDamage_.push_back(report);
}

void Connection::reportShot(std::uint32_t weapon, const shared::Vec3& target) {
    if (weapon == 0) {
        return;
    }

    shared::WeaponFired fired;
    fired.weapon = weapon;
    fired.target = target;

    outgoingShots_.push_back(fired);
}

std::vector<shared::WeaponFired> Connection::takeShots() {
    return std::exchange(shots_, {});
}

std::vector<shared::PlayerWeapon> Connection::takeWeaponLooks() {
    return std::exchange(weaponLooks_, {});
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

std::vector<shared::VehicleControl> Connection::takeVehicleControls() {
    return std::exchange(vehicleControls_, {});
}

std::vector<shared::VehicleDoors> Connection::takeVehicleDoors() {
    return std::exchange(vehicleDoors_, {});
}

std::vector<shared::VehicleTeleport> Connection::takeVehicleTeleports() {
    return std::exchange(vehicleTeleports_, {});
}

std::vector<shared::VehicleDamaged> Connection::takeVehicleDamage() {
    return std::exchange(vehicleDamage_, {});
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

std::vector<shared::PlayerAnimation> Connection::takeAnimations() {
    return std::exchange(animations_, {});
}

std::vector<shared::Explosion> Connection::takeExplosions() {
    return std::exchange(explosions_, {});
}

std::vector<shared::PedState> Connection::takePeds() {
    return std::exchange(peds_, {});
}

std::vector<shared::PedId> Connection::takeRemovedPeds() {
    return std::exchange(removedPeds_, {});
}

std::vector<shared::EntityAttachment> Connection::takeAttachments() {
    return std::exchange(attachments_, {});
}

std::vector<shared::MarkerState> Connection::takeMarkers() {
    return std::exchange(markers_, {});
}

std::vector<shared::MarkerId> Connection::takeRemovedMarkers() {
    return std::exchange(removedMarkers_, {});
}

std::vector<shared::CheckpointState> Connection::takeCheckpoints() {
    return std::exchange(checkpoints_, {});
}

std::vector<shared::CheckpointId> Connection::takeRemovedCheckpoints() {
    return std::exchange(removedCheckpoints_, {});
}

std::vector<shared::PlayerIntoVehicle> Connection::takeSeats() {
    return std::exchange(seats_, {});
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
    hello.hwidHash = settings_.hwidHash;
    hello.socialId = settings_.socialId;
    hello.socialName = settings_.socialName;

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
    // потоке. Разница есть: снимки снимаются каждый кадр, а уходит раз в такт
    // последний из них, и отметка должна описывать то, что ушло. Одна на всё,
    // что уходит в этот раз, — они и описывают одно мгновение.
    const auto sentAt = static_cast<shared::Timestamp>(nowMilliseconds());

    // Машины уходят раньше своего водителя, и это не мелочь: получатель сажает
    // игрока в машину, а посадить его некуда, пока о машине не сказано.
    for (shared::VehicleState vehicle : ownedVehicles_) {
        vehicle.sentAt = sentAt;

        // Снимок, в котором ничего не изменилось, не отправляется — но не
        // дольше, чем kStateKeepalive. Стоящая машина в сессии куда обычнее
        // едущей, а платит за её снимок каждый, кто её видит.
        const auto sent = sentVehicles_.find(vehicle.id);

        const bool overdue =
            sent == sentVehicles_.end() || Clock::now() - sent->second.at >= kStateKeepalive;

        if (!overdue && !shared::differs(sent->second.state, vehicle)) {
            continue;
        }

        const auto vehiclePacket = shared::encode(vehicle);
        host_->send(serverPeer_, shared::Channel::State, shared::ByteView{vehiclePacket});

        sentVehicles_.insert_or_assign(vehicle.id, SentVehicle{vehicle, Clock::now()});
    }

    // Машины, которые перестали быть нашими, забываются: вернувшись, они обязаны
    // объявиться заново — за то время, что её вёл другой, с ней могло случиться
    // что угодно.
    std::erase_if(sentVehicles_, [this](const auto& entry) {
        return std::ranges::none_of(ownedVehicles_, [&entry](const shared::VehicleState& owned) {
            return owned.id == entry.first;
        });
    });

    // Снимок, в котором ничего не изменилось, не отправляется вовсе — но не
    // дольше, чем kStateKeepalive. См. worthSending.
    const bool overdue = Clock::now() - stateSentAt_ >= kStateKeepalive;

    if (overdue || shared::differs(sentState_, localState_)) {
        localState_.sentAt = sentAt;

        const auto packet = shared::encode(localState_);
        host_->send(serverPeer_, shared::Channel::State, shared::ByteView{packet});

        sentState_ = localState_;
        stateSentAt_ = Clock::now();
    }

    nextStateAt_ = Clock::now() + stateInterval_;

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
        // снимок заменит следующий в ближайшем такте, а потерянный цвет
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
        outgoingVehicleDamage_.clear();
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

    // И попадания по машинам — тем же каналом и по той же причине.
    for (const shared::VehicleDamageReport& report : outgoingVehicleDamage_) {
        const auto packet = shared::encode(report);
        host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
    }
    outgoingVehicleDamage_.clear();

    // Тем же каналом и по той же причине: потерянный выстрел не повторится.
    for (const shared::WeaponFired& fired : outgoingShots_) {
        const auto packet = shared::encode(fired);
        host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
    }
    outgoingShots_.clear();

    // Тем же надёжным каналом: потерянное нажатие не повторится, а игрок
    // увидит, что оно пропало впустую.
    for (const shared::ClientEvent& event : outgoingEvents_) {
        const auto packet = shared::encode(event);
        host_->send(serverPeer_, shared::Channel::Control, shared::ByteView{packet});
    }
    outgoingEvents_.clear();
}

void Connection::fallBackToWaiting(std::string_view reason) {
    // Причина по-английски, как и весь журнал, и это не мелочь: строка уходит
    // уровнем warning, то есть попадает в тот журнал, который игрок присылает,
    // когда у него что-то не работает. Половина строк была здесь по-русски, а
    // вторая половина — «retrying in N ms» — по-английски, и присланная строка
    // читалась как поломка кодировки.
    if (state_ == ConnectionState::Rejected) {
        return;
    }

    spdlog::warn("{}, retrying in {} ms", reason, retryDelay_.count());

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
    control_ = 0;
    vehicleControls_.clear();
    vehicleDoors_.clear();
    serverEvents_.clear();

    // Предметы и снаряжение забываются вместе с соединением: их список
    // принадлежит серверу, и после переподключения он расскажет о них заново.
    objects_.clear();
    removedObjects_.clear();
    vehicleTeleports_.clear();
    vehicleRepairs_.clear();
    vehicleDamage_.clear();
    blips_.clear();
    removedBlips_.clear();
    animations_.clear();
    explosions_.clear();
    shots_.clear();
    outgoingShots_.clear();
    weaponLooks_.clear();

    // Последний отправленный снимок забывается вместе с соединением: по нему
    // решается, изменилось ли что-нибудь, и оставленный от прошлой сессии он
    // заставил бы промолчать в самой первой отправке новой.
    sentState_ = shared::PlayerState{};
    stateSentAt_ = Clock::time_point{};
    sentVehicles_.clear();
    attachments_.clear();
    peds_.clear();
    removedPeds_.clear();
    seats_.clear();
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
