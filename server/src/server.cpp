#include "server.hpp"

#ifdef OXYMP_WITH_JS
#include "js_runtime.hpp"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <vector>

namespace oxymp::server {
namespace {

/// Сколько ждать рукопожатия от подключившегося соединения.
constexpr auto kHelloTimeout = std::chrono::seconds{10};

/// Где кончается ближний круг и начинается дальний, в квадратах метров.
///
/// Числа взяты из того, как игра показывает людей: до полусотни метров чужой
/// игрок виден в подробностях, до полутора сотен — фигурой, дальше — точкой.
/// Столько же ему и снимков: каждый, каждый второй, каждый четвёртый.
constexpr float kNearRing = 50.0F * 50.0F;
constexpr float kFarRing = 150.0F * 150.0F;

constexpr unsigned int kMidEvery = 2;
constexpr unsigned int kFarEvery = 4;

/// Как часто снимок стоящего игрока уходит всё равно.
///
/// Стоящий не шлёт ничего нового, и пересылать его каждый такт незачем. Но
/// подошедший рядом с ним иначе не увидел бы его вовсе: снимков нет, а из чего
/// ещё взяться персонажу, получателю неоткуда узнать. Секунда — та задержка,
/// которую не заметит вошедший, но заметит сервер: она снимает с него почти всю
/// пересылку неподвижной толпы.
constexpr auto kStateKeepalive = std::chrono::seconds{1};

/// Как часто сервер пишет в журнал, во что ему обходится сессия.
///
/// В отладочный журнал и нечасто: числа эти нужны не хозяину сервера, а тому,
/// кто разбирается, выдержит ли сессия наплыв. Считаются они не транспортом, а
/// нами — заголовки UDP и ENet сюда не входят, и важно здесь отношение до и
/// после, а не абсолютный байт.
constexpr auto kTrafficInterval = std::chrono::seconds{10};

/// Шаг цикла обслуживания. Он же — срок, отведённый разбору событий: дольше
/// одного такта сервер не разбирает их ни при какой нагрузке.
constexpr auto kTickInterval = std::chrono::milliseconds{1000 / shared::kDefaultTickRate};

/// Сколько ждать одного события за раз.
///
/// Меньше такта, и намеренно: ожидание длиной в такт означало бы, что срок
/// разбора истекает ровно тогда, когда сервер только проснулся. Двумя
/// миллисекундами он просыпается достаточно часто, чтобы уложиться в срок с
/// точностью, которой хватает и снимкам, и скриптам.
constexpr auto kPollSlice = std::chrono::milliseconds{2};

std::string_view describe(shared::RejectReason reason) {
    switch (reason) {
    case shared::RejectReason::ProtocolMismatch:
        return "версия протокола не совпадает";
    case shared::RejectReason::ServerFull:
        return "сервер заполнен";
    case shared::RejectReason::InvalidNickname:
        return "недопустимое имя";
    case shared::RejectReason::NicknameTaken:
        return "имя занято";
    }
    return "причина не указана";
}

bool nicknameLooksValid(const std::string& nickname) {
    if (nickname.empty() || nickname.size() > shared::kMaxNicknameLength) {
        return false;
    }

    // Управляющие символы в имени ломают вывод и ничего не добавляют.
    return std::ranges::none_of(nickname, [](unsigned char c) { return c < 0x20 || c == 0x7F; });
}

} // namespace

std::unique_ptr<Server> Server::start(const Config& config, std::string& error) {
    auto host = net::Host::listen(config.port, config.maxPlayers, error);
    if (!host) {
        return nullptr;
    }

    std::unique_ptr<Server> server{new Server};
    server->config_ = config;
    server->host_ = std::move(host);
    server->world_ = WorldClock{config.weather, config.startingHour, config.startingMinute};

    spdlog::info("сервер \"{}\" слушает порт {}, мест: {}", config.name, config.port,
                 config.maxPlayers);

    // Игровые файлы собираются до того, как кто-либо подключится: клиент
    // получает их список первым же делом, и собирать его на ходу значило бы
    // заставить первого вошедшего ждать шифрования всех файлов.
    server->resources_.load(config.gameFilesDirectory);

    // Ресурсы — следом, и порядок этот вынужденный: сбор игровых файлов
    // начинается с чистого листа, и клиентские файлы ресурсов, добавленные
    // раньше него, он стёр бы вместе со старым списком.
    //
    // До первого подключения — тоже: подписка ресурса на события должна стоять
    // раньше первого события, а первым будет вход игрока.
    server->startResources();

    if (!server->resources_.empty()) {
        std::string httpError;

        // Тот же номер порта, что и у игры. Спорить им не о чем: игра общается
        // по UDP, раздача по TCP, и это разные пространства номеров.
        server->http_ = HttpServer::start(config.port, server->resources_, httpError);

        if (server->http_ == nullptr) {
            // Не повод не запускать сервер: без раздачи играть можно, просто без
            // добавленного хозяином содержимого.
            spdlog::error("раздача ресурсов не поднялась: {}", httpError);
            spdlog::error("клиенты не получат добавленное вами содержимое");
        }
    }

    return server;
}

void Server::run(const std::atomic<bool>& stopRequested) {
    while (!stopRequested.load()) {
        // Разбор событий ограничен сроком такта, а не «пока приходят».
        //
        // Прежде здесь стоял цикл без срока, и выходил он только на промежутке
        // молчания длиной в целый такт. Под нагрузкой такого промежутка не
        // бывает: два десятка игроков шлют по двадцать снимков в секунду, и
        // события идут сплошь. Сервер оставался внутри этого цикла и не доходил
        // до всего остального — ни пересдачи машин, ни раздачи, ни тика
        // скриптов. Замечено на нагрузке: за семьдесят секунд такт не отработал
        // ни разу.
        const auto deadline = std::chrono::steady_clock::now() + kTickInterval;

        // Разбор идёт до срока — и ровно до срока, ни раньше, ни позже.
        //
        // Ждать тишины нельзя: под нагрузкой её не бывает, и сервер не выходил
        // из разбора вовсе — ни пересдачи машин, ни раздачи, ни тика скриптов.
        // Выходить же по первой тишине тоже нельзя: на пустом сервере такт
        // прокручивался бы пятьсот раз в секунду вместо тридцати, а рассылка
        // снимков вместе с ним — то есть неровно.
        while (std::chrono::steady_clock::now() < deadline) {
            auto event = host_->poll(kPollSlice);
            if (!event) {
                continue;
            }

            switch (event->type) {
            case net::Event::Type::Connected:
                handleConnected(event->peer);
                break;
            case net::Event::Type::Disconnected:
                handleDisconnected(event->peer);
                break;
            case net::Event::Type::Message:
                handleMessage(event->peer, event->payload);
                break;
            }
        }

        dropSilentPeers();
        broadcastStates();
        reassignVehicles();
        streamVehicles();
        streamObjects();
        broadcastWorld();
        reviveDead();

        reportTraffic();

        // Скрипты — последними в такте, когда сессия уже приведена в порядок.
        // Обработчик увидит мир таким, каким его увидят клиенты, а не застанет
        // его на середине пересдачи машин.
        events_.dispatch(script::Event{.kind = script::EventKind::Tick});
    }

    spdlog::info("остановка, игроков было: {}", players_.size());

    // Ресурсы останавливаются до того, как сервер вытолкнет последнее. Иначе
    // ресурс, прощающийся с игроками строкой в чат, говорил бы её в уже
    // закрытую дверь.
    stopResources();

    host_->flush();
}

void Server::handleConnected(net::PeerId peer) {
    spdlog::debug("соединение {} установлено, ждём представления", peer);
    awaitingHello_.emplace(peer, std::chrono::steady_clock::now());
}

void Server::handleDisconnected(net::PeerId peer) {
    awaitingHello_.erase(peer);

    // Скриптам — до уборки, а не после. Иначе обработчик получил бы ссылку на
    // того, кого уже нет: ни имени, ни того, где он стоял, ни на чём ехал. А
    // именно это и нужно тому, кто сохраняет игрока перед выходом.
    if (const Player* leaving = players_.findByPeer(peer); leaving != nullptr) {
        tellScripts(script::EventKind::PlayerDisconnect, leaving->id);
    }

    const auto player = players_.removeByPeer(peer);
    if (!player) {
        spdlog::debug("соединение {} закрыто до представления", peer);
        return;
    }

    spdlog::info("игрок \"{}\" (id {}) отключился, осталось: {}", player->nickname, player->id,
                 players_.size());

    // Где он сидел — забывается сразу. Машины при этом остаются: в них могли
    // остаться пассажиры, и убрать машину вместе с ушедшим значило бы высадить
    // их посреди дороги. Те, что он вёл, остаются без ведущего.
    vehicles_.forgetPlayer(player->id);

    // Пересмотр — немедленно, не дожидаясь очереди. Машины, которые вёл ушедший,
    // до него стоят замершими, и полсекунды неподвижной машины посреди дороги
    // видно всем.
    reassignedAt_ = {};

    shared::PlayerLeft left;
    left.playerId = player->id;
    broadcast(left);

    announce(shared::ChatKind::Leave, player->id, player->nickname,
             std::format("{} (id {}) вышел", player->nickname, player->id));
}

void Server::handleMessage(net::PeerId peer, const std::vector<std::uint8_t>& payload) {
    const shared::ByteView packet{payload};

    const auto id = shared::peekMessageId(packet);
    if (!id) {
        spdlog::warn("соединение {} прислало нераспознанный пакет ({} байт)", peer, payload.size());
        return;
    }

    switch (*id) {
    case shared::MessageId::ClientHello:
        if (const auto hello = shared::decode<shared::ClientHello>(packet)) {
            handleHello(peer, *hello);
        }
        return;

    case shared::MessageId::Ping:
        if (const auto ping = shared::decode<shared::Ping>(packet)) {
            handlePing(peer, *ping);
        }
        return;

    case shared::MessageId::PlayerState:
        if (const auto state = shared::decode<shared::PlayerState>(packet)) {
            handlePlayerState(peer, *state);
        }
        return;

    case shared::MessageId::VehicleState:
        if (const auto state = shared::decode<shared::VehicleState>(packet)) {
            handleVehicleState(peer, *state);
        }
        return;

    case shared::MessageId::VehicleAppearance:
        if (const auto appearance = shared::decode<shared::VehicleAppearance>(packet)) {
            handleVehicleAppearance(peer, *appearance);
        }
        return;

    case shared::MessageId::PlayerAppearance:
        if (const auto appearance = shared::decode<shared::PlayerAppearance>(packet)) {
            handlePlayerAppearance(peer, *appearance);
        }
        return;

    case shared::MessageId::ClientEvent:
        if (const auto event = shared::decode<shared::ClientEvent>(packet)) {
            handleClientEvent(peer, *event);
        }
        return;

    case shared::MessageId::ChatSay:
        if (const auto say = shared::decode<shared::ChatSay>(packet)) {
            handleChatSay(peer, *say);
        }
        return;

    case shared::MessageId::DamageReport:
        if (const auto report = shared::decode<shared::DamageReport>(packet)) {
            handleDamageReport(peer, *report);
        }
        return;

    // Эти сообщения посылает сервер, а не клиент. Получить их обратно означает
    // либо ошибку в клиенте, либо попытку что-то подделать.
    case shared::MessageId::ServerWelcome:
    case shared::MessageId::ServerReject:
    case shared::MessageId::Pong:
    case shared::MessageId::PlayerJoined:
    case shared::MessageId::PlayerLeft:
    case shared::MessageId::ChatLine:
    case shared::MessageId::DamageTaken:
    case shared::MessageId::PlayerTeleport:
    case shared::MessageId::MoneyChanged:
    case shared::MessageId::ResourceList:
    case shared::MessageId::VehicleAdded:
    case shared::MessageId::VehicleRemoved:
    case shared::MessageId::VehicleAuthority:
    case shared::MessageId::WorldState:
    case shared::MessageId::PlayerLoadout:
    case shared::MessageId::HealthChanged:
    case shared::MessageId::ObjectAdded:
    case shared::MessageId::ObjectRemoved:
    case shared::MessageId::ServerEvent:
        spdlog::warn("соединение {} прислало серверное сообщение", peer);
        return;
    }
}

void Server::handleHello(net::PeerId peer, const shared::ClientHello& hello) {
    if (players_.findByPeer(peer) != nullptr) {
        spdlog::warn("соединение {} представилось повторно", peer);
        return;
    }

    if (hello.protocolVersion != shared::kProtocolVersion) {
        spdlog::info("соединение {} отклонено: протокол {} против нашего {}", peer,
                     hello.protocolVersion, shared::kProtocolVersion);
        reject(peer, shared::RejectReason::ProtocolMismatch);
        return;
    }

    // Пароль сверяется раньше имени, и это не безразличный порядок: сервер под
    // паролем не должен рассказывать постороннему даже того, занято ли имя.
    //
    // Слишком длинный отвергается, не доходя до сравнения: строка в пакете
    // ограничена только его размером, а сравнивать мегабайт с восемью байтами
    // незачем.
    if (!config_.password.empty() && (hello.password.size() > shared::kMaxPasswordLength ||
                                      hello.password != config_.password)) {
        spdlog::info("соединение {} отклонено: пароль не сошёлся", peer);
        reject(peer, shared::RejectReason::WrongPassword);
        return;
    }

    if (!nicknameLooksValid(hello.nickname)) {
        reject(peer, shared::RejectReason::InvalidNickname);
        return;
    }

    // Занятое имя — не то же самое, что недопустимое, и разделены они не ради
    // порядка: игрок, переподключившийся раньше, чем сервер похоронил его
    // прошлое соединение, натыкается на собственное имя. Отказ здесь временный,
    // и клиент обязан узнать об этом, иначе повторять попытки он перестанет.
    if (players_.nicknameTaken(hello.nickname)) {
        reject(peer, shared::RejectReason::NicknameTaken);
        return;
    }

    if (players_.size() >= config_.maxPlayers) {
        reject(peer, shared::RejectReason::ServerFull);
        return;
    }

    awaitingHello_.erase(peer);

    // О новичке нужно рассказать остальным, а ему — про остальных. Порядок важен:
    // список существующих собирается до того, как он сам попадёт в реестр.
    std::vector<shared::PlayerJoined> existing;
    existing.reserve(players_.size());
    for (const auto& [otherPeer, other] : players_) {
        shared::PlayerJoined joined;
        joined.playerId = other.id;
        joined.nickname = other.nickname;
        existing.push_back(std::move(joined));
    }

    const Player& player = players_.add(peer, hello.nickname, config_.startingMoney);

    shared::ServerWelcome welcome;
    welcome.playerId = player.id;

    // Точка появления приходит от сервера, а не зашита в клиент. Она — свойство
    // сессии: хозяин вправе посадить игроков куда угодно, не пересобирая
    // ничего, и сменить это одной строкой в server.cfg.
    welcome.spawnPosition = config_.spawnPosition;
    welcome.tickRate = shared::kDefaultTickRate;

    // Имя сервера — то, что игрок увидит в меню вместо адреса. Знает его один
    // только сервер: в адресе его нет, а каталога, где спросить, у oxyMP тоже
    // нет.
    welcome.name = config_.name;

    sendTo(peer, welcome);

    // Деньги — сразу за приветствием: до них клиент показывает прочерк, и чем
    // короче это время, тем меньше похоже на неисправность.
    shared::MoneyChanged money;
    money.amount = player.money;
    sendTo(peer, money);

    // Список раздаваемого — следом, до всего остального. Клиент по нему решает,
    // что качать, а качать он начинает до появления в мире: докачивать
    // содержимое, когда игрок уже бегает, поздно.
    //
    // Отправляется всегда, даже пустым: пустой список это ответ «ничего не
    // нужно», а молчание клиент отличить от него не сможет и будет ждать.
    shared::ResourceList resources;

    // Какие из раздаваемых файлов — страницы интерфейса. Это client-main
    // ресурсов, названные составным именем: клиент по нему поймёт, какой из
    // скачанных файлов открыть в CEF, а не будет гадать.
    std::vector<std::string> pages;
    for (const ScriptResource& resource : catalog_.all()) {
        if (!resource.clientMain.empty()) {
            pages.push_back(std::format("{}/{}", resource.name, resource.clientMain));
        }
    }

    for (const ResourceStore::Item& item : resources_.items()) {
        resources.entries.push_back(shared::ResourceEntry{
            .name = item.name,
            .hash = item.hash,
            .size = item.size,
            .page = std::ranges::find(pages, item.name) != pages.end(),
        });
    }

    sendTo(peer, resources);

    for (const auto& joined : existing) {
        sendTo(peer, joined);
    }

    // Следом — как эти люди выглядят. Отдельным проходом, а не вместе с
    // объявлением: объявлен всякий, а внешность объявили не все — сервер её не
    // выдумывает, и до первого слова владельца рассказывать нечего.
    for (const auto& [otherPeer, other] : players_) {
        if (otherPeer != peer && other.appearance.has_value()) {
            sendTo(peer, *other.appearance);
        }
    }

    // Погода и время — сразу: мир, в который игрок вот-вот попадёт, обязан
    // выглядеть так же, как у остальных, уже в первом кадре.
    sendTo(peer, world_.snapshot());

    // Здоровье — тоже: клиент о нём больше не свидетельствует, он о нём узнаёт,
    // и до первого сообщения ему неоткуда знать, сколько у него жизни.
    sendHealth(player, shared::kInvalidPlayerId);

    // Машин здесь нет намеренно, хотя раньше они высылались все разом. Теперь
    // клиент узнаёт только о том, что вокруг него, а где он — станет известно с
    // первым же его снимком, через полсотни миллисекунд. Тогда их и вышлет
    // ближайшая раздача.
    streamedAt_ = {};

    shared::PlayerJoined announcement;
    announcement.playerId = player.id;
    announcement.nickname = player.nickname;
    broadcast(announcement, peer);

    spdlog::info("игрок \"{}\" (id {}) подключился, всего: {}", player.nickname, player.id,
                 players_.size());

    // После рассылки о входе, а не вместо неё: PlayerJoined ведёт список
    // игроков, а строка чата — лента событий. Одно другое не заменяет.
    announce(shared::ChatKind::Join, player.id, player.nickname,
             std::format("{} (id {}) зашёл на сервер", player.nickname, player.id));

    // Скриптам — в самом конце, когда игроку уже сказано всё, что ему полагается
    // при входе. Обработчик вправе тут же выдать ему оружие или поставить машину,
    // и его распоряжения должны лечь поверх наших, а не под них.
    const shared::PlayerId joined = player.id;

    tellScripts(script::EventKind::PlayerConnect, joined);

    // Появление — отдельным событием следом. Разделены они не для порядка: в
    // сессии игрок появляется всякий раз заново после смерти, а входит один раз,
    // и обработчики у этого разные. Здесь же они идут подряд просто потому, что
    // точку появления сервер назвал в приветствии.
    tellScripts(script::EventKind::PlayerSpawn, joined);
}

void Server::handlePing(net::PeerId peer, const shared::Ping& ping) {
    shared::Pong pong;
    pong.timestampMs = ping.timestampMs;

    const auto packet = shared::encode(pong);
    host_->send(peer, shared::Channel::State, shared::ByteView{packet});
}

void Server::handlePlayerState(net::PeerId peer, shared::PlayerState state) {
    Player* player = players_.findByPeer(peer);
    if (player == nullptr) {
        // Состояние до рукопожатия рассылать некому и незачем.
        return;
    }

    // Идентификатор проставляет сервер, а не клиент. Иначе достаточно было бы
    // подменить одно поле, чтобы двигать чужого игрока.
    state.playerId = player->id;

    // Патроны, наоборот, берутся у клиента и запоминаются: тратит их игра у
    // владельца, и узнать их число сервер может только от него. Записывается оно
    // в снаряжение, чтобы после смерти вернуть человеку то, с чем он ходил, а не
    // полный магазин.
    for (shared::WeaponSlot& slot : player->loadout) {
        if (slot.weapon == state.weapon) {
            slot.ammo = state.ammo;
            break;
        }
    }

    // Здоровье и броня — наоборот, проставляются сервером поверх присланного.
    // Это и есть та перемена, ради которой всё затевалось: клиент о своём
    // здоровье больше не свидетельствует, он о нём узнаёт.
    state.health = player->health;
    state.armour = player->armour;

    // Где игрок стоит — запоминается: по этому серверу решает, кому вести какую
    // машину. Верить здесь клиенту приходится, и это не беда: соврав о своём
    // положении, он получит во владение машину, которой у него нет перед
    // глазами, — то есть навредит себе.
    player->position = state.position;

    // Куда смотрит — тоже, и не для себя: сам сервер направлением взгляда не
    // пользуется. Нужно оно скриптам — поставить машину перед человеком нельзя,
    // не зная, куда он повёрнут.
    player->heading = state.heading;

    // Где игрок сидит — запоминается тоже, и вместе с местом: за рулём машину
    // ведёт он и никто другой, а пассажир такого преимущества не даёт.
    // Залезающий ещё не сидит — у него места нет.
    const bool seated = shared::has(state.flags, shared::PlayerFlag::InVehicle);

    const bool moved =
        vehicles_.setSeat(player->id, seated ? state.vehicleId : shared::kInvalidVehicleId,
                          seated ? state.seat : shared::kNoSeat);

    // Пересел — пересматриваем ведущих немедленно. Пока пересмотр не прошёл,
    // машина под севшим за руль остаётся чужой, то есть замороженной, и
    // полсекунды неподвижного руля игрок принимает за поломку.
    if (moved) {
        reassignedAt_ = {};
    }

    // Снимок не рассылается сразу, а откладывается до конца такта: там он
    // уйдёт вместе с остальными одной посылкой. Причина — заголовки: отдельным
    // пакетом каждый снимок платит за них по разу на получателя, и при сотне
    // игроков заголовки весят больше самих снимков.
    player->state = state;
    player->stateFresh = true;
}

void Server::reportTraffic() {
    const auto now = std::chrono::steady_clock::now();

    if (trafficAt_ == std::chrono::steady_clock::time_point{}) {
        trafficAt_ = now;
        (void)host_->takeTraffic();
        return;
    }

    const auto elapsed = now - trafficAt_;
    if (elapsed < kTrafficInterval) {
        return;
    }

    trafficAt_ = now;

    const net::Host::Traffic traffic = host_->takeTraffic();

    // Молчащий сервер не пишет ничего: пустая сессия и так видна по числу
    // игроков, а строка раз в десять секунд в пустом журнале — это шум.
    if (traffic.packets == 0) {
        return;
    }

    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

    spdlog::debug("отдано транспорту: {:.0f} посылок/с, {:.1f} КБ/с при {} игроках",
                  static_cast<double>(traffic.packets) / seconds,
                  static_cast<double>(traffic.bytes) / seconds / 1024.0, players_.size());
}

void Server::broadcastStates() {
    const auto now = std::chrono::steady_clock::now();

    // Расстояние сравнивается в квадратах: корень здесь считать незачем, а
    // считать его пришлось бы на каждую пару игроков в каждом такте.
    const float reach = config_.streamDistance * config_.streamDistance;

    ++tick_;

    for (const auto& [peer, listener] : players_) {
        shared::PlayerStates bundle;

        for (const auto& [otherPeer, other] : players_) {
            if (otherPeer == peer) {
                continue;
            }

            // Молчащего пересылать незачем: получатель держит его на месте сам.
            // Раз в секунду — всё же пересылаем: подошедший рядом со стоящим
            // иначе не увидел бы его вовсе.
            const bool stale = now - other.stateSentAt >= kStateKeepalive;
            if (!other.stateFresh && !stale) {
                continue;
            }

            // Только тем, кто рядом. Игроку незачем знать, как бежит человек за
            // полкилометра: он его всё равно не увидит, а снимков это половина
            // всего, что ходит по сети.
            const float distance = shared::distanceSquared(listener.position, other.state.position);
            if (distance > reach) {
                continue;
            }

            // Дальние обновляются реже ближних, и это не мелочь, а то, чем
            // держится наплыв. Человек в двух шагах должен двигаться плавно,
            // человек за триста метров — просто быть там, где он есть: на таком
            // расстоянии он размером с пиксель, и разницы между двадцатью
            // снимками в секунду и пятью не видно никому.
            //
            // Кого пропустить, решает не счётчик на каждую пару — их было бы
            // столько же, сколько игроков в квадрате, — а остаток от деления с
            // добавкой номера игрока. Добавка нужна, чтобы дальние обновлялись
            // вразнобой: без неё все они пришли бы одним тактом, и сеть шла бы
            // рывками.
            const unsigned int every = distance > kFarRing  ? kFarEvery
                                       : distance > kNearRing ? kMidEvery
                                                              : 1U;

            if (every > 1 && (tick_ + other.id) % every != 0) {
                continue;
            }

            if (bundle.players.size() >= shared::kMaxStatesInBundle) {
                break;
            }

            bundle.players.push_back(other.state);
        }

        if (!bundle.players.empty()) {
            // Ненадёжным каналом, как и прежде: потерянная связка дешевле
            // заменяется следующей, чем переотправляется устаревшей.
            const auto packet = shared::encode(bundle);
            host_->send(peer, shared::Channel::State, shared::ByteView{packet});
        }
    }

    // Отметки снимаются после всех связок, а не по ходу: снимок одного игрока
    // попадает к нескольким получателям, и сняв признак на первом из них, мы
    // лишили бы остальных.
    for (auto& [peer, player] : players_) {
        if (player.stateFresh || now - player.stateSentAt >= kStateKeepalive) {
            player.stateFresh = false;
            player.stateSentAt = now;
        }
    }
}

void Server::handleVehicleState(net::PeerId peer, shared::VehicleState state) {
    const Player* player = players_.findByPeer(peer);
    if (player == nullptr) {
        return;
    }

    // Снимок принимается только от ведущего. Всё остальное — отставший снимок
    // того, у кого машину уже забрали, или ошибка в клиенте; и то и другое
    // дёрнуло бы машину назад, будь оно принято.
    if (!vehicles_.applyState(player->id, state)) {
        return;
    }

    // От машины, а не от её ведущего: вести машину можно и не сидя в ней, и
    // считать видимость по тому, кто её ведёт, значило бы рассылать снимки не
    // тем, кто её видит.
    broadcastNear(state.position, shared::Channel::State, state, peer);
}

void Server::handlePlayerAppearance(net::PeerId peer, shared::PlayerAppearance appearance) {
    Player* player = players_.findByPeer(peer);
    if (player == nullptr) {
        return;
    }

    // Чей это вид, решает сервер, а не пришедший пакет. Поверив клиенту, мы
    // позволили бы ему переодеть кого угодно.
    appearance.playerId = player->id;

    // Запоминается, а не только пересылается: тот, кто войдёт позже, этого
    // сообщения уже не услышит, а человека увидит — и увидел бы его в том, что
    // игра надевает по умолчанию.
    player->appearance = appearance;

    // Всем, а не только ближним, и надёжным каналом. Внешность — не снимок: она
    // нужна получателю в тот миг, когда чужой игрок появится у него из-за
    // поворота, а объявления к тому времени давно не будет.
    broadcast(appearance, peer);
}

void Server::handleVehicleAppearance(net::PeerId peer, shared::VehicleAppearance appearance) {
    const Player* player = players_.findByPeer(peer);
    if (player == nullptr) {
        return;
    }

    // Запоминается, а не только пересылается: тот, кто войдёт позже, этого
    // сообщения уже не услышит, а машину увидит.
    if (!vehicles_.applyAppearance(player->id, appearance)) {
        return;
    }

    // Только тем, кто машину видит, и по надёжному каналу. Разослав её всем, мы
    // заставили бы дальних запомнить цвет машины, которой у них нет, — и держать
    // его до конца сессии, потому что забыть его им будет не по чему.
    const VehicleDirectory::Vehicle* vehicle = vehicles_.find(appearance.id);

    broadcastNear(vehicle->state.position, shared::Channel::Control, appearance, peer);
}

void Server::streamObjects() {
    const float appears = config_.streamDistance * config_.streamDistance;
    const float vanishes = appears * 1.21F;

    for (auto& [peer, player] : players_) {
        for (const auto& [id, object] : objects_.all()) {
            // Не near: так называется макрос из windows.h, оставшийся там с
            // шестнадцатиразрядных времён. Он пуст, и переменная с таким именем
            // просто исчезает — вместе с внятностью сообщения об ошибке.
            const bool nearby =
                shared::distanceSquared(player.position, object.position) <= appears;

            if (!nearby || player.streamedObjects.contains(id)) {
                continue;
            }

            shared::ObjectAdded added;
            added.id = id;
            added.model = object.model;
            added.position = object.position;
            added.rotation = object.rotation;
            sendTo(peer, added);

            player.streamedObjects.insert(id);
        }

        for (auto it = player.streamedObjects.begin(); it != player.streamedObjects.end();) {
            const ObjectDirectory::Object* object = objects_.find(*it);

            const bool keep = object != nullptr &&
                              shared::distanceSquared(player.position, object->position) <= vanishes;

            if (keep) {
                ++it;
                continue;
            }

            shared::ObjectRemoved removed;
            removed.id = *it;
            sendTo(peer, removed);

            it = player.streamedObjects.erase(it);
        }
    }
}

void Server::reviveDead() {
    const auto now = std::chrono::steady_clock::now();

    // Кого подняли за этот проход.
    //
    // Списком, а не объявлением на месте: обработчик события волен менять
    // сессию, а мы посреди обхода таблицы игроков — одна вставка, и обход
    // рассыплется.
    std::vector<shared::PlayerId> revived;

    for (auto& [peer, player] : players_) {
        if (player.health != 0) {
            // Живой отсчёта не ведёт: он начинается со смертью и кончается ею же.
            player.diedAt = {};
            continue;
        }

        if (player.diedAt == std::chrono::steady_clock::time_point{}) {
            player.diedAt = now;
            continue;
        }

        if (now - player.diedAt < kRespawnDelay) {
            continue;
        }

        // Возрождает сервер, а не клиент, и это следует из того, что здоровье
        // принадлежит серверу. Позволь мы клиенту воскресать самому — и любой
        // мог бы объявить себя живым, не дожидаясь ничьего разрешения.
        player.health = kFullHealth;
        player.armour = 0;
        player.diedAt = {};

        sendHealth(player, shared::kInvalidPlayerId);

        // Снаряжение возвращается вместе с жизнью: игра при смерти отбирает
        // оружие, и без этого воскресший поднимался бы с пустыми руками.
        sendLoadout(player, true);

        revived.push_back(player.id);
    }

    for (const shared::PlayerId id : revived) {
        tellScripts(script::EventKind::PlayerSpawn, id);
    }
}

void Server::reassignVehicles() {
    constexpr auto kReassignInterval = std::chrono::milliseconds{500};

    const auto now = std::chrono::steady_clock::now();
    if (now - reassignedAt_ < kReassignInterval) {
        return;
    }

    reassignedAt_ = now;

    std::vector<VehicleDirectory::PlayerPlacement> placements;
    placements.reserve(players_.size());

    for (const auto& [peer, player] : players_) {
        placements.push_back(VehicleDirectory::PlayerPlacement{
            .id = player.id,
            .position = player.position,
        });
    }

    for (const auto& change : vehicles_.reassign(placements)) {
        shared::VehicleAuthority authority;
        authority.id = change.id;
        authority.owner = change.owner;
        broadcast(authority);

        spdlog::debug("машину {} отныне ведёт {}", change.id,
                      change.owner == shared::kInvalidPlayerId ? -1
                                                              : static_cast<int>(change.owner));
    }
}

void Server::sendVehicleTo(net::PeerId peer, const VehicleDirectory::Vehicle& vehicle) {
    shared::VehicleAdded added;
    added.state = vehicle.state;
    added.owner = vehicle.owner;
    sendTo(peer, added);

    // Внешность — следом за самой машиной, и только если её объявляли. Раньше
    // неё нельзя: внешность накладывается на машину, а машины у получателя в
    // это мгновение ещё нет.
    if (vehicle.appearance) {
        sendTo(peer, *vehicle.appearance);
    }
}

void Server::streamVehicles() {
    constexpr auto kStreamInterval = std::chrono::milliseconds{500};

    const auto now = std::chrono::steady_clock::now();
    if (now - streamedAt_ < kStreamInterval) {
        return;
    }

    streamedAt_ = now;

    // Границы две, и они разные. С одной машина появляется, за другой пропадает,
    // и вторая дальше первой на десятую часть.
    //
    // Без этого запаса машина, оказавшаяся ровно на границе, заводилась бы и
    // убиралась каждые полсекунды, пока игрок переминается с ноги на ногу: у
    // клиента это непрерывное создание и удаление сущностей, а у игрока — машина,
    // мигающая на горизонте.
    const float appears = config_.streamDistance * config_.streamDistance;
    const float vanishes = appears * 1.21F; // (1.1)^2

    for (auto& [peer, player] : players_) {
        // Сперва то, что приблизилось: о машине рассказывают раньше, чем о ней
        // пойдут снимки, — иначе получатель отбросит их как принадлежащие
        // неизвестной машине.
        for (const auto& [id, vehicle] : vehicles_.all()) {
            const bool nearby =
                shared::distanceSquared(player.position, vehicle.state.position) <= appears;

            if (!nearby || player.streamed.contains(id)) {
                continue;
            }

            sendVehicleTo(peer, vehicle);
            player.streamed.insert(id);
        }

        // Затем то, что отдалилось или исчезло. Отдалившаяся машина убирается у
        // клиента тем же сообщением, что и убранная совсем: для него это одно и
        // то же — перестать её показывать.
        for (auto it = player.streamed.begin(); it != player.streamed.end();) {
            const VehicleDirectory::Vehicle* vehicle = vehicles_.find(*it);

            const bool keep =
                vehicle != nullptr &&
                shared::distanceSquared(player.position, vehicle->state.position) <= vanishes;

            if (keep) {
                ++it;
                continue;
            }

            shared::VehicleRemoved removed;
            removed.id = *it;
            sendTo(peer, removed);

            it = player.streamed.erase(it);
        }
    }
}

void Server::broadcastWorld() {
    if (!world_.advance()) {
        return;
    }

    broadcast(world_.snapshot());
}

void Server::handleChatSay(net::PeerId peer, const shared::ChatSay& say) {
    const Player* player = players_.findByPeer(peer);
    if (player == nullptr) {
        return;
    }

    // Пустое сообщение отправить нельзя, а слишком длинное — обрезается.
    // Отвергать его целиком незачем: длина ничего не ломает, а игрок, у
    // которого реплика пропала без следа, решит, что сломан чат.
    std::string text = say.text;
    if (text.size() > shared::kMaxChatLength) {
        text.resize(shared::kMaxChatLength);
    }

    if (text.empty()) {
        return;
    }

    // Имя и номер берутся до объявления скриптам, а не после. Обработчик волен
    // менять сессию, а список игроков живёт в таблице: одна вставка — и указатель
    // на запись показывает в пустоту.
    const shared::PlayerId author = player->id;
    std::string nickname = player->nickname;

    // Единственное событие, которое скрипт вправе отменить, — и ради него признак
    // отмены вообще заведён: команду вида «/kick» нужно уметь перехватить и не
    // пустить в общий чат.
    if (!tellScripts(script::EventKind::PlayerChat, author, text)) {
        return;
    }

    announce(shared::ChatKind::Say, author, std::move(nickname), std::move(text));
}

void Server::handleDamageReport(net::PeerId peer, const shared::DamageReport& report) {
    const Player* attacker = players_.findByPeer(peer);
    if (attacker == nullptr) {
        return;
    }

    // Себе урон через сервер не наносят: свой урон игрок применяет сам, и такое
    // сообщение означает либо ошибку, либо попытку что-то выгадать.
    if (report.victim == attacker->id) {
        return;
    }

    const shared::PlayerId attackerId = attacker->id;
    const std::string attackerName = attacker->nickname;
    const shared::Vec3 attackerAt = attacker->position;

    Player* victim = players_.findById(report.victim);

    if (victim == nullptr || victim->health == 0) {
        return;
    }

    // Урон обрезается, а не отвергается. Число могло испортиться по дороге или
    // прийти от ошибки в клиенте, и в обоих случаях выстрел был настоящим:
    // отбросить его целиком значило бы сделать стрелявшего безобидным.
    const std::uint16_t amount = std::min(report.amount, config_.maxDamagePerHit);
    if (amount == 0) {
        return;
    }

    // Попадание с другого конца карты не бывает. Проверка грубая и такой
    // задумана: точную линию выстрела сервер не построит — мира у него нет, — а
    // вот отличить перестрелку от доклада о попадании за километр может.
    const float reach = config_.streamDistance * config_.streamDistance;
    if (shared::distanceSquared(attackerAt, victim->position) > reach) {
        spdlog::warn("игрок \"{}\" (id {}) доложил о попадании издалека", attackerName, attackerId);
        return;
    }

    // Броня принимает удар первой и целиком. Так же считает и сама игра, и
    // расходиться с ней здесь незачем: игрок судит о своей броне по её счётчику.
    std::uint16_t left = amount;

    const std::uint16_t absorbed = std::min(left, victim->armour);
    victim->armour = static_cast<std::uint16_t>(victim->armour - absorbed);
    left = static_cast<std::uint16_t>(left - absorbed);

    victim->health = left >= victim->health ? 0 : static_cast<std::uint16_t>(victim->health - left);

    // Здоровье — самому пострадавшему: остальные узнают его из снимка, который
    // сервер и без того правит на своё значение перед рассылкой.
    sendHealth(*victim, attackerId);

    // Прежнее сообщение об уроне уходит следом и живёт по-прежнему. Здоровье оно
    // больше не меняет — его меняет сервер, — но остаётся тем, чем и было:
    // поводом показать игроку, кто и из чего по нему попал.
    shared::DamageTaken taken;
    taken.attacker = attackerId;
    taken.amount = amount;
    taken.weapon = report.weapon;

    // Надёжным каналом, в отличие от снимков состояния: потерянный снимок
    // заменит следующий, а потерянное попадание не повторится никогда.
    const auto packet = shared::encode(taken);
    host_->send(victim->peer, shared::Channel::Control, shared::ByteView{packet});

    if (victim->health != 0) {
        return;
    }

    const shared::PlayerId victimId = victim->id;

    announce(shared::ChatKind::System, shared::kInvalidPlayerId, {},
             std::format("{} убил игрока {}", attackerName, victim->nickname));

    // Смерть от чужой руки объявляет тот, кто её нанёс: только здесь известен
    // убийца и оружие. Смерть без убийцы — падение, утопление, воля скрипта —
    // объявляется там, где здоровье обнулилось, и стрелявшего в ней нет.
    script::Event death;
    death.kind = script::EventKind::PlayerDeath;
    death.player = script::Player{core_, victimId};
    death.killer = script::Player{core_, attackerId};
    death.weapon = report.weapon;

    events_.dispatch(death);
}

void Server::sendHealth(const Player& player, shared::PlayerId attacker) {
    shared::HealthChanged changed;
    changed.health = player.health;
    changed.armour = player.armour;
    changed.attacker = attacker;

    sendTo(player.peer, changed);
}

void Server::sendLoadout(const Player& player, bool replace) {
    shared::PlayerLoadout loadout;
    loadout.weapons = player.loadout;
    loadout.replace = replace;

    sendTo(player.peer, loadout);
}

void Server::handleClientEvent(net::PeerId peer, const shared::ClientEvent& event) {
    const Player* player = players_.findByPeer(peer);
    if (player == nullptr) {
        return;
    }

    if (event.name.empty()) {
        return;
    }

    // Дальше — дело ресурсов. Сервер о смысле события не судит и судить не
    // может: имена придумывает игровой режим, и знать их наперёд ему неоткуда.
    // Право просить о чём бы то ни было проверяет тот, кто событие слушает.
    script::Event delivered;
    delivered.kind = script::EventKind::ClientEvent;
    delivered.player = script::Player{core_, player->id};
    delivered.name = event.name;
    delivered.text = event.payload;

    events_.dispatch(delivered);
}

void Server::healthChanged(const Player& player) {
    // Без нападавшего: сюда попадает то, что сервер или скрипт сделали сами, —
    // лечение, броня, воскрешение. Урон от чужой руки уходит своим путём и
    // называет стрелявшего.
    sendHealth(player, shared::kInvalidPlayerId);
}

void Server::loadoutChanged(const Player& player, bool replace) {
    sendLoadout(player, replace);
}

void Server::teleported(const Player& player, const shared::Vec3& position) {
    shared::PlayerTeleport teleport;
    teleport.position = position;

    sendTo(player.peer, teleport);
}

void Server::emitted(const Player& player, std::string_view name, std::string_view payload) {
    shared::ServerEvent event;
    event.name = std::string{name};
    event.payload = std::string{payload};

    sendTo(player.peer, event);
}

void Server::vehicleAdded(shared::VehicleId /*id*/) {
    // Объявляет машину не это место, а ближайшая раздача — та же, что
    // рассказывает о подъехавших. Так объявление уходит ровно тем, кто машину
    // увидит, и ровно один раз: разошли мы его всем подряд, у дальних она
    // завелась бы, а из их списка виденного выпала.
    streamedAt_ = {};

    // И пересмотр ведущих следом: заведённая машина пока ничья, и до пересмотра
    // её никто не считает — она стоит неподвижно у просившего на глазах.
    reassignedAt_ = {};
}

void Server::vehicleRemoved(shared::VehicleId id) {
    shared::VehicleRemoved removed;
    removed.id = id;
    broadcast(removed);

    // Из списков виденного — тоже, и вместе с рассылкой. Иначе раздача сочла бы,
    // что машина «отдалилась», и послала бы второе такое же сообщение.
    for (auto& [peer, player] : players_) {
        player.streamed.erase(id);
    }
}

void Server::objectAdded(shared::ObjectId /*id*/) {
    streamedAt_ = {};
}

void Server::objectRemoved(shared::ObjectId id) {
    shared::ObjectRemoved removed;
    removed.id = id;
    broadcast(removed);

    for (auto& [peer, player] : players_) {
        player.streamedObjects.erase(id);
    }
}

void Server::worldChanged() {
    broadcast(world_.snapshot());
}

void Server::chatLine(shared::PlayerId to, std::string text) {
    shared::ChatLine line;

    // Отправителем значится сервер, а не тот, кто велел. Скрипт — часть сервера,
    // и приписывать его слова игроку значило бы позволить ему говорить чужим
    // голосом.
    line.kind = shared::ChatKind::System;
    line.text = std::move(text);

    if (to == shared::kInvalidPlayerId) {
        spdlog::info("чат: [сервер] {}", line.text);
        broadcast(line);
        return;
    }

    const Player* target = players_.findById(to);
    if (target == nullptr) {
        return;
    }

    sendTo(target->peer, line);
}

bool Server::tellScripts(script::EventKind kind, shared::PlayerId about, std::string text) {
    script::Event event;
    event.kind = kind;
    event.player = script::Player{core_, about};
    event.text = std::move(text);

    return events_.dispatch(event);
}

void Server::announce(shared::ChatKind kind, shared::PlayerId author, std::string nickname,
                      std::string text) {
    shared::ChatLine line;
    line.kind = kind;
    line.playerId = author;
    line.nickname = std::move(nickname);
    line.text = std::move(text);

    spdlog::info("чат: [{}] {}", line.nickname.empty() ? "сервер" : line.nickname, line.text);

    broadcast(line);
}

void Server::reject(net::PeerId peer, shared::RejectReason reason) {
    spdlog::info("соединение {} отклонено: {}", peer, describe(reason));

    shared::ServerReject message;
    message.reason = reason;
    sendTo(peer, message);

    // Отказ нужно вытолкнуть до разрыва, иначе клиент увидит молчаливое
    // закрытие и не поймёт причины.
    host_->flush();
    host_->disconnect(peer);
}

void Server::startResources() {
    for (const std::string& complaint : catalog_.load(config_.resourceDirectory,
                                                      config_.resources)) {
        spdlog::warn("ресурс: {}", complaint);
    }

    ensureRuntimes();

    for (const ScriptResource& resource : catalog_.all()) {
        // Машина выбирается по типу. О ресурсе на языке, которого сервер не
        // понимает, говорится прямо, а не молчанием: молча пропущенный игровой
        // режим хозяин будет искать долго.
        Runtime* const runtime = runtimeFor(resource.type);

        if (runtime == nullptr) {
            spdlog::error("ресурс \"{}\": машины для типа \"{}\" в сервере нет", resource.name,
                          resource.type);
            continue;
        }

        std::string error;

        if (!runtime->start(resource, error)) {
            spdlog::error("ресурс \"{}\" не поднят: {}", resource.name, error);
            continue;
        }

        // Клиентская половина уходит в раздачу под составным именем: одинаково
        // названные файлы разных ресурсов иначе сошлись бы в одно.
        for (const std::string& file : resource.clientFiles) {
            if (!resources_.add(resource.root / file, std::format("{}/{}", resource.name, file))) {
                spdlog::warn("ресурс \"{}\": файл \"{}\" прочитать не удалось", resource.name,
                             file);
            }
        }

        spdlog::info("ресурс \"{}\" поднят ({}), клиенту файлов: {}", resource.name, resource.type,
                     resource.clientFiles.size());
    }
}

void Server::stopResources() {
    for (const ScriptResource& resource : catalog_.all()) {
        if (Runtime* const runtime = runtimeFor(resource.type); runtime != nullptr) {
            runtime->stop(resource);
        }
    }
}

void Server::ensureRuntimes() {
    // Машины заводятся под то, что встретилось в перечне, а не про запас.
    //
    // Это не бережливость ради бережливости. Node поднимает изоляты, потоки и
    // свою кучу; голому серверу, у которого нет ни одного скрипта, всё это не
    // нужно, а платить за него он бы стал памятью и временем запуска.
    for (const ScriptResource& resource : catalog_.all()) {
        if (runtimeFor(resource.type) != nullptr) {
            continue;
        }

#ifdef OXYMP_WITH_JS
        if (resource.type == "js") {
            std::string error;

            if (std::unique_ptr<JsRuntime> js = JsRuntime::create(core_, events_, error);
                js != nullptr) {
                runtimes_.push_back(std::move(js));
                continue;
            }

            // Не повод не запускать сервер: без движка он лишится скриптов, но
            // не сессии. Жаловаться при этом обязательно — иначе хозяин будет
            // искать, почему его режим молчит.
            spdlog::error("движок JS не поднялся: {}", error);
            spdlog::error("ресурсы на JS работать не будут");
        }
#endif
    }
}

Runtime* Server::runtimeFor(std::string_view type) const {
    for (const std::unique_ptr<Runtime>& runtime : runtimes_) {
        if (runtime->type() == type) {
            return runtime.get();
        }
    }

    return nullptr;
}

void Server::dropSilentPeers() {
    const auto now = std::chrono::steady_clock::now();

    for (auto it = awaitingHello_.begin(); it != awaitingHello_.end();) {
        if (now - it->second < kHelloTimeout) {
            ++it;
            continue;
        }

        spdlog::info("соединение {} не представилось за отведённое время", it->first);
        host_->disconnect(it->first);
        it = awaitingHello_.erase(it);
    }
}

} // namespace oxymp::server
