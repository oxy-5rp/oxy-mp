#include "server.hpp"

#include <oxymp/shared/resource/source_kind.hpp>

#ifdef OXYMP_WITH_JS
#include "js_runtime.hpp"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
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

/// Сколько снимков в секунду довольно на среднем и дальнем круге.
///
/// Частотой, а не «каждый второй» и «каждый четвёртый», и это перемена. Пока
/// такт был один-единственный, разницы не было: тридцать тактов, поделённые на
/// два и на четыре, и давали эти пятнадцать и семь. Но такт стал настройкой, и
/// делитель перестал что-либо означать — на сотне тактов «каждый четвёртый»
/// это двадцать пять снимков в секунду человеку, который виден точкой.
///
/// Числа те же, что и были при тридцати тактах: пятнадцать снимков в секунду
/// фигуре за сотню метров и семь — точке за двести. Разглядеть на них разницу
/// нельзя никому, а платят за них все.
constexpr std::uint16_t kMidRate = 15;
constexpr std::uint16_t kFarRate = 7;

/// Через сколько тактов снимок уходит на круг, которому довольно такой частоты.
///
/// Не реже одного такта: частота, названная больше самого такта, означает
/// «каждый», а не «чаще, чем бывает».
[[nodiscard]] unsigned int everyTicks(std::uint16_t tickRate, std::uint16_t wanted) noexcept {
    return wanted >= tickRate ? 1U : static_cast<unsigned int>(tickRate / wanted);
}

/// Какой промежуток между снимками игрока считается молчанием.
///
/// Клиент не шлёт снимок, в котором ничего не изменилось, — стоящий на месте
/// человек шлёт его лишь раз в четверть секунды. Такие редкие снимки нельзя
/// прореживать по расстоянию: прореживание пропускает каждый четвёртый такт, и
/// снимку, приходящему раз в четверть секунды, случается не совпасть с
/// пропускаемым тактом несколько раз подряд. Получатель за это время успевает
/// счесть игрока пропавшим и убрать его персонажа — а тот всего лишь стоял.
///
/// Полторы десятых секунды: заметно больше такта на любой разумной частоте и
/// заметно меньше той четверти секунды, с которой шлёт стоящий.
constexpr auto kQuietGap = std::chrono::milliseconds{150};

/// Как часто снимок стоящего игрока уходит всё равно.
///
/// Стоящий не шлёт ничего нового, и пересылать его каждый такт незачем. Но
/// подошедший рядом с ним иначе не увидел бы его вовсе: снимков нет, а из чего
/// ещё взяться персонажу, получателю неоткуда узнать.
///
/// Полсекунды, а не секунда, и решает здесь не задержка, а запас. Получатель
/// убирает персонажа того, о ком молчат две секунды, — иначе игрок, ушедший за
/// горизонт видимости, стоял бы у всех до конца сессии. Канал же
/// ненадёжный: при секундной досылке хватило бы одной потерянной посылки
/// подряд с другой, чтобы стоящий человек мигнул и появился заново. При
/// половине секунды таких потерь нужно четыре кряду, и это уже не случай, а
/// оборванная связь.
///
/// Пересылку неподвижной толпы это всё равно снимает почти целиком: клиент шлёт
/// такой снимок четыре раза в секунду, а сервер пересылает два.
constexpr auto kStateKeepalive = std::chrono::milliseconds{500};

/// Как часто сервер пишет в журнал, во что ему обходится сессия.
///
/// В отладочный журнал и нечасто: числа эти нужны не хозяину сервера, а тому,
/// кто разбирается, выдержит ли сессия наплыв. Считаются они не транспортом, а
/// нами — заголовки UDP и ENet сюда не входят, и важно здесь отношение до и
/// после, а не абсолютный байт.
constexpr auto kTrafficInterval = std::chrono::seconds{10};

/// Какую долю обещанной частоты сервер обязан выдерживать.
///
/// Ниже неё он говорит об этом вслух: отставание от собственного такта
/// означает, что снимки уходят реже обещанного, и у всех разом дёргается всё.
/// Узнать это иначе нельзя ниоткуда.
constexpr double kTickShortfall = 0.9;

/// Шаг цикла обслуживания. Он же — срок, отведённый разбору событий: дольше
/// одного такта сервер не разбирает их ни при какой нагрузке.
/// Шаг цикла обслуживания при названной частоте.
///
/// Больше не постоянная: частоту такта задаёт хозяин сервера строкой
/// `tickrate`, и по ней же клиент решает, как часто слать свои снимки.
///
/// В микросекундах, а не в миллисекундах, и это не педантизм. Шестьдесят
/// тактов — это 16.67 миллисекунды; округлённые до шестнадцати, они дают
/// 62.5 такта в секунду вместо шестидесяти. Хозяин написал одно число, а
/// получил другое — и это ещё полбеды: клиент считает свой промежуток тем же
/// делением, и разойдись округления, снимки пошли бы вразнобой с тактами.
[[nodiscard]] std::chrono::microseconds tickInterval(std::uint16_t tickRate) noexcept {
    const std::uint16_t rate =
        std::clamp(tickRate, shared::kMinTickRate, shared::kMaxTickRate);

    return std::chrono::microseconds{1'000'000 / rate};
}

/// Сколько ждать одного события за раз, самое большее.
///
/// Меньше такта, и намеренно: ожидание длиной в такт означало бы, что срок
/// разбора истекает ровно тогда, когда сервер только проснулся. Двумя
/// миллисекундами он просыпается достаточно часто, чтобы уложиться в срок с
/// точностью, которой хватает и снимкам, и скриптам.
///
/// Самое большее — потому что последнее ожидание в такте укорачивается до
/// того, что от такта осталось. Без этого сервер систематически опаздывал:
/// проснувшись за миллисекунду до срока, он уходил ждать ещё на две и
/// возвращался с опозданием. На такте в тридцать три миллисекунды это
/// незаметно, а на шестидесяти тактах в секунду — целых три такта: измерено,
/// 57.3 вместо 60 на пустом ожидании.
constexpr auto kMaxPollSlice = std::chrono::milliseconds{2};

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

    spdlog::info("Server \"{}\" listening on port {}, slots: {}", config.name, config.port,
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

    // Ресурсы подняты все до одного — теперь можно сказать им об этом.
    //
    // Здесь, а не внутри startResources, и разница существенна: событие
    // означает «поднялись и соседи», и объявленное в середине обхода оно
    // застало бы половину ресурсов ещё не поднятыми. Ресурс, спросивший о
    // соседе из такого обработчика, получил бы пустоту — и виноватого искал бы
    // в себе.
    //
    // До первого подключения, как и у alt:V: подписка ресурса на вход игрока
    // должна стоять раньше первого входа.
    {
        script::Event started;
        started.kind = script::EventKind::ServerStarted;
        (void)server->events_.dispatch(started);
    }

    if (!server->resources_.empty()) {
        std::string httpError;

        // Тот же номер порта, что и у игры. Спорить им не о чем: игра общается
        // по UDP, раздача по TCP, и это разные пространства номеров.
        server->http_ = HttpServer::start(config.port, server->resources_, httpError);

        if (server->http_ == nullptr) {
            // Не повод не запускать сервер: без раздачи играть можно, просто без
            // добавленного хозяином содержимого.
            spdlog::error("resource delivery did not start: {}", httpError);
            spdlog::error("clients will not receive the content you added");
        }
    }

    return server;
}

void Server::run(const std::atomic<bool>& stopRequested) {
    // Срок следующего такта отсчитывается от срока прошлого, а не от того
    // мгновения, когда мы до него дошли. Разница здесь не тонкость, и стоила
    // она трёх тактов из шестидесяти.
    //
    // Работа такта — рассылка, раздача, скрипты — идёт после того, как срок
    // разбора истёк, и на полусотне игроков занимает полмиллисекунды. Отсчитывай
    // мы следующий срок от «сейчас», эти полмиллисекунды прибавлялись бы к
    // каждому такту: шестьдесят обещанных превращались в пятьдесят восемь.
    // Отсчитанный же от прошлого срока, он их поглощает — окно разбора просто
    // становится на столько же короче.
    auto nextTick = std::chrono::steady_clock::now();

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
        nextTick += tickInterval(config_.tickRate);

        // Не поспели — не копим долг. Сервер, догоняющий прошлое, не догонит
        // его никогда: такты пошли бы один за другим без единого ожидания, и
        // разбор событий не получил бы ни миллисекунды.
        if (const auto now = std::chrono::steady_clock::now(); nextTick < now) {
            nextTick = now;
        }

        const auto deadline = nextTick;

        // Разбор идёт до срока — и ровно до срока, ни раньше, ни позже.
        //
        // Ждать тишины нельзя: под нагрузкой её не бывает, и сервер не выходил
        // из разбора вовсе — ни пересдачи машин, ни раздачи, ни тика скриптов.
        // Выходить же по первой тишине тоже нельзя: на пустом сервере такт
        // прокручивался бы пятьсот раз в секунду вместо тридцати, а рассылка
        // снимков вместе с ним — то есть неровно.
        while (true) {
            const auto left = deadline - std::chrono::steady_clock::now();

            if (left <= std::chrono::steady_clock::duration::zero()) {
                break;
            }

            // Ждём не дольше, чем осталось от такта: последнее ожидание в
            // такте обязано кончиться ровно на сроке, а не за ним.
            const auto slice = std::min(
                kMaxPollSlice, std::chrono::duration_cast<std::chrono::milliseconds>(left));

            auto event = host_->poll(slice);
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
        broadcastVehicleStates();
        reassignVehicles();
        streamVehicles();
        streamObjects();
        streamPeds();
        broadcastWorld();
        reviveDead();

        reportTraffic();
        runConsole();

        // Скрипты — последними в такте, когда сессия уже приведена в порядок.
        // Обработчик увидит мир таким, каким его увидят клиенты, а не застанет
        // его на середине пересдачи машин.
        events_.dispatch(script::Event{.kind = script::EventKind::Tick});
    }

    spdlog::info("Stopping, players online: {}", players_.size());

    // Ресурсы останавливаются до того, как сервер вытолкнет последнее. Иначе
    // ресурс, прощающийся с игроками строкой в чат, говорил бы её в уже
    // закрытую дверь.
    stopResources();

    host_->flush();
}

void Server::handleConnected(net::PeerId peer) {
    // Уровнем info и с адресом, а не отладочной строкой без него.
    //
    // Это единственное, чем хозяин сервера отличает «до меня не доходит» от «до
    // меня доходит, а дальше рвётся», и вопрос этот возникает при всякой жалобе
    // на подключение. Пока строка жила на уровне debug, сервер по умолчанию
    // молчал, и отличить одно от другого было нечем: пусто в журнале и пусто.
    //
    // Игра ходит по UDP, а раздача ресурсов — по TCP, и оба на одном порту.
    // Проверяльщики портов проверяют только TCP, поэтому «порт открыт» ничего не
    // говорит о том, дойдёт ли игра. Эта строка говорит.
    spdlog::info("Connection from {} accepted, waiting for the handshake",
                 host_->addressOf(peer));

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

    spdlog::info("Player \"{}\" (id {}) disconnected, {} left", player->nickname, player->id,
                 players_.size());

    // Где он сидел — забывается сразу. Машины при этом остаются: в них могли
    // остаться пассажиры, и убрать машину вместе с ушедшим значило бы высадить
    // их посреди дороги. Те, что он вёл, остаются без ведущего.
    vehicles_.forgetPlayer(player->id);

    // Вместе с ним пропадает и всё, что на нём висело: предмет, привязанный к
    // ушедшему, остался бы у остальных висеть в пустоте — отвязать его после
    // было бы уже некому.
    core_.forgetAttachments(
        AttachmentDirectory::Ref{.kind = shared::EntityKind::Player, .id = player->id});

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
        spdlog::warn("connection {} sent an unrecognised packet ({} bytes)", peer, payload.size());
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

    case shared::MessageId::WeaponFired:
        if (const auto fired = shared::decode<shared::WeaponFired>(packet)) {
            handleWeaponFired(peer, *fired);
        }
        return;

    case shared::MessageId::VehicleDamageReport:
        if (const auto hit = shared::decode<shared::VehicleDamageReport>(packet)) {
            handleVehicleDamage(peer, *hit);
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
    case shared::MessageId::VehicleStates:
    case shared::MessageId::Explosion:
    case shared::MessageId::VehicleDamaged:
        spdlog::warn("connection {} sent a server-only message", peer);
        return;
    }
}

void Server::handleHello(net::PeerId peer, const shared::ClientHello& hello) {
    if (players_.findByPeer(peer) != nullptr) {
        spdlog::warn("connection {} introduced itself twice", peer);
        return;
    }

    if (hello.protocolVersion != shared::kProtocolVersion) {
        spdlog::info("connection {} refused: protocol {} against our {}", peer,
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
        spdlog::info("connection {} refused: wrong password", peer);
        reject(peer, shared::RejectReason::WrongPassword);
        return;
    }

    if (!nicknameLooksValid(hello.nickname)) {
        reject(peer, shared::RejectReason::InvalidNickname);
        return;
    }

    // Занятое имя больше не отказ, и это перемена в правилах, а не упрощение.
    //
    // Прежде сервер требовал имена разными и отвечал NicknameTaken. Правило это
    // ничего не защищало: игрок в сессии называется номером, а не именем, — по
    // номеру его находит и режим, и чат, и всякая рассылка, — а имя приходит из
    // `oxymp.toml` и по умолчанию у всех одинаковое. Двое, поставившие игру и
    // не тронувшие настройку, попросту не попадали друг к другу.
    //
    // Хуже того, отказ бил и по одному человеку. Переподключившийся раньше, чем
    // транспорт похоронил его прошлое соединение, натыкался на собственное имя
    // и ждал сорок пять секунд, пока оно освободится.
    //
    // Что теряется, стоит знать: двоих с одним именем в чате и над головами не
    // различить. Номер при этом называется и там и там — строки входа и выхода
    // несут его наравне с именем, — а решать, требовать ли имена разными,
    // отныне дело режима: имя он видит при входе и вправе отказать сам.
    //
    // Номер причины остаётся закреплённым за ней навсегда, и клиент по-прежнему
    // умеет её понять: сервер прежней сборки всё ещё может её прислать.

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
    // Частоту называем свою, а не постоянную: по ней клиент решает, как часто
    // слать снимки, и разойтись этим двум числам нельзя — иначе у сервера
    // остаются пустые такты либо приходит больше снимков, чем он разошлёт.
    welcome.tickRate = config_.tickRate;

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

    // Какие из раздаваемых файлов игра обязана прочесть описанием и каким.
    // Имена здесь составные — те же, под какими файлы уходят клиенту.
    std::unordered_map<std::string, std::string> described;

    for (const ScriptResource& resource : catalog_.all()) {
        if (!resource.clientMain.empty()) {
            pages.push_back(std::format("{}/{}", resource.name, resource.clientMain));
        }

        for (const auto& [file, type] : resource.dataFiles) {
            described.emplace(std::format("{}/{}", resource.name, file), type);
        }
    }

    for (const ResourceStore::Item& item : resources_.items()) {
        const auto description = described.find(item.name);
        const auto packed = bundled_.find(item.name);

        resources.entries.push_back(shared::ResourceEntry{
            .name = item.name,
            .hash = item.hash,
            .size = item.size,
            .page = std::ranges::find(pages, item.name) != pages.end(),
            .dataFile = description == described.end() ? std::string{} : description->second,
            .bundle = packed == bundled_.end() ? std::string{} : packed->second,
        });
    }

    // Файлы, уехавшие в свёрток, в списке остаются — со ссылкой на свёрток
    // вместо своего отпечатка. Клиенту нужно и то и другое: свёрток он качает
    // один раз, а имена и виды описаний берёт у каждого файла отдельно.
    for (const auto& [name, hash] : bundled_) {
        const auto description = described.find(name);

        resources.entries.push_back(shared::ResourceEntry{
            .name = name,
            .hash = hash,
            .size = 0,
            .page = std::ranges::find(pages, name) != pages.end(),
            .dataFile = description == described.end() ? std::string{} : description->second,
            .bundle = hash,
        });
    }

    // Список, не влезший в предел, — это молча недоданные файлы.
    //
    // Молчать здесь нельзя ни в коем случае: клиент скачает начало, откроет
    // страницу, которой нет, и покажет ошибку браузера. Связать её с числом в
    // протоколе будет не по чему — а хозяину сервера нужно понять, что резать
    // надо не тут.
    if (resources.entries.size() > shared::kMaxResources) {
        spdlog::error("{} files are being served, but the protocol holds {}: the rest will not arrive",
                      resources.entries.size(), shared::kMaxResources);
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

        // И как у них собрано оружие: вошедший этих объявлений не слышал, а
        // человека с глушителем увидит.
        if (otherPeer != peer && other.shownWeapon.weapon != 0) {
            sendTo(peer, other.shownWeapon);
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

    spdlog::info("Player \"{}\" (id {}) connected, {} online", player.nickname, player.id,
                 players_.size());

    // После рассылки о входе, а не вместо неё: PlayerJoined ведёт список
    // игроков, а строка чата — лента событий. Одно другое не заменяет.
    announce(shared::ChatKind::Join, player.id, player.nickname,
             std::format("{} (id {}) зашёл на сервер", player.nickname, player.id));

    // Скриптам сказать пока нельзя: у игрока ещё не поднялась клиентская
    // половина ресурсов.
    //
    // Разница видна сразу, стоит режиму послать что-нибудь клиенту из
    // playerConnect — а так делает почти каждый: «покажи окно входа», «вот твоё
    // состояние». Событие уходит в мгновение, когда на той стороне ещё некому
    // его услышать, и страница остаётся пустой. Искать причину при этом негде:
    // и сервер, и клиент отработали безупречно, просто в разном порядке.
    //
    // Поэтому ждём слова клиента. Так же поступает и alt:V: его playerConnect
    // объявляется, когда клиент готов целиком, а не когда установилось
    // соединение.
    (void)player;
}

template<typename Directory>
void Server::sendKindTo(const Directory& directory, net::PeerId peer, std::int32_t dimension) {
    for (const auto& [id, entry] : directory.all()) {
        if (script::dimensionsMeet(dimension, entry.dimension)) {
            sendTo(peer, entry.state);
        }
    }
}

template<typename Directory>
void Server::broadcastDrawn(const Directory& directory, typename Directory::Id id) {
    const auto* const entry = directory.find(id);
    if (entry == nullptr) {
        return;
    }

    // Всем, кто в том же слое мира, и без оглядки на расстояние. Метка на то и
    // метка, что видна на карте целиком; маркер и точку отбирает у себя тот,
    // кто их рисует, — по их собственному полю видимости.
    //
    // Надёжным каналом: потерянная картинка не заменится следующей — она больше
    // не изменится и останется несуществующей до конца сессии.
    for (const auto& [peer, player] : players_) {
        if (script::dimensionsMeet(player.dimension, entry->dimension)) {
            sendTo(peer, entry->state);
        }
    }
}

/// Рассказывает вошедшему обо всём нарисованном, что ему видно.
///
/// Раздачей, как машины и предметы, картинки не ходят: они не отбираются
/// расстоянием на сервере, и рассказывать о них по мере приближения не о чем.
/// Поэтому — один раз, целиком, при входе.
void Server::sendDrawnTo(net::PeerId peer, std::int32_t dimension) {
    sendKindTo(blips_, peer, dimension);
    sendKindTo(markers_, peer, dimension);
    sendKindTo(checkpoints_, peer, dimension);
}

void Server::sendAttachmentsTo(net::PeerId peer) {
    for (const auto& [key, attachment] : attachments_.all()) {
        sendTo(peer, attachment);
    }
}

void Server::attachmentChanged(AttachmentDirectory::Ref entity) {
    const shared::EntityAttachment* const attachment = attachments_.find(entity);

    if (attachment != nullptr) {
        broadcast(*attachment);
        return;
    }

    // Привязки больше нет — рассылается то же сообщение с пустой целью. Отдельного
    // сообщения на отвязку нет и не нужно: получателю важно не «убери привязку», а
    // «вот как эта сущность привязана теперь», а «никак» — такой же ответ.
    shared::EntityAttachment loosened;
    loosened.kind = entity.kind;
    loosened.id = entity.id;

    broadcast(loosened);
}

void Server::announcePlayerReady(Player& player) {
    if (player.scriptsReady) {
        return;
    }

    player.scriptsReady = true;

    const shared::PlayerId joined = player.id;

    // Нарисованное — раньше обработчиков входа: те вправе поставить своё, и
    // поставленное ими должно лечь поверх уже имеющегося, а не быть перекрыто
    // рассылкой старого.
    sendDrawnTo(player.peer, player.dimension);

    // Привязки — по той же причине и в том же месте: они тоже состояние, а не
    // событие, и вошедший обязан застать мир таким, каким его видят остальные.
    sendAttachmentsTo(player.peer);

    // Обработчик вправе тут же выдать оружие или поставить машину, и его
    // распоряжения должны лечь поверх наших, а не под них.
    tellScripts(script::EventKind::PlayerConnect, joined);

    // Появление — отдельным событием следом. Разделены они не для порядка: в
    // сессии игрок появляется всякий раз заново после смерти, а входит один раз,
    // и обработчики у этого разные.
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
    //
    // И не всякий откладывается: снимок, в котором ничего не изменилось,
    // пересылать незачем. Клиент такой не шлёт вовсе — но не дольше четверти
    // секунды, а потом шлёт всё равно, чтобы не сойти за пропавшего. Приняв это
    // за новость, сервер пересылал бы одно и то же четыре раза в секунду
    // каждому соседу — то есть числом игроков в квадрате.
    //
    // Молчащий при этом не пропадает: раз в секунду его снимок уходит всё
    // равно (kStateKeepalive), и прореживание по расстоянию его не трогает.
    if (shared::differs(player->state, state)) {
        player->stateFresh = true;
    }

    // Разница считается по прошлому снимку, а объявляется по свежему: пока
    // обработчик работает, игрок в реестре обязан быть уже новым — режим первым
    // делом спрашивает у него `seat` и `vehicle`. Отсюда порядок: запомнить
    // прошлое, положить свежее, объявить.
    const shared::PlayerState before = player->state;
    player->state = state;

    tellScriptsAboutChanges(*player, before, state);

    // Оружие в руках сменилось — рассказать остальным, как оно выглядит.
    // Сравнение с прошлым объявлением живёт внутри: снимок приходит каждый
    // такт, а насадки меняются раз в несколько минут.
    announceWeapon(*player);
}

void Server::runConsole() {
    for (const std::string& line : console_.take()) {
        std::vector<std::string> parts = splitCommand(line);

        if (parts.empty()) {
            continue;
        }

        script::Event typed;
        typed.kind = script::EventKind::ConsoleCommand;
        typed.name = parts.front();
        typed.arguments.assign(parts.begin() + 1, parts.end());

        // Строка объявляется скриптам и больше ничего с ней не делается: своих
        // команд у голого сервера нет и не будет — как нет их и у клиента.
        //
        // Слушателя нет вовсе — говорим об этом хозяину. Иначе набранное молча
        // пропадает, и он решит, что сервер не отвечает. Дальше этого проверка
        // не идёт: подписан ли кто-нибудь именно на consoleCommand, отсюда не
        // видно — список подписок ведёт каждый ресурс у себя.
        if (events_.size() == 0) {
            spdlog::info("Command \"{}\" went nowhere: nothing is listening for commands",
                         typed.name);
            continue;
        }

        (void)events_.dispatch(typed);
    }
}

void Server::reportTraffic() {
    const auto now = std::chrono::steady_clock::now();

    if (trafficAt_ == std::chrono::steady_clock::time_point{}) {
        trafficAt_ = now;
        reportedTick_ = tick_;
        (void)host_->takeTraffic();
        return;
    }

    const auto elapsed = now - trafficAt_;
    if (elapsed < kTrafficInterval) {
        return;
    }

    const std::uint64_t ticks = tick_ - reportedTick_;

    trafficAt_ = now;
    reportedTick_ = tick_;

    const net::Host::Traffic traffic = host_->takeTraffic();

    // Молчащий сервер не пишет ничего: пустая сессия и так видна по числу
    // игроков, а строка раз в десять секунд в пустом журнале — это шум.
    if (traffic.packets == 0) {
        return;
    }

    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

    const double achieved = static_cast<double>(ticks) / seconds;

    spdlog::debug("отдано транспорту: {:.0f} посылок/с, {:.1f} КБ/с при {} игроках",
                  static_cast<double>(traffic.packets) / seconds,
                  static_cast<double>(traffic.bytes) / seconds / 1024.0, players_.size());

    spdlog::debug("такт: {:.1f} из {} в секунду", achieved, config_.tickRate);

    // Отставание от собственной частоты — не мелочь и не отладочная
    // подробность: снимки при нём уходят реже обещанного, и у всех разом
    // дёргается всё. Поэтому уровнем выше и с числами, по которым видно
    // насколько.
    //
    // Девять десятых — та граница, за которой отставание перестаёт быть
    // округлением: такт длиной в тридцать три миллисекунды, просевший на
    // десятую, опаздывает на три с лишним.
    if (achieved < static_cast<double>(config_.tickRate) * kTickShortfall) {
        spdlog::warn("the server runs at {:.1f} of the {} ticks per second it was told to",
                     achieved, config_.tickRate);
    }
}

void Server::broadcastStates() {
    const auto now = std::chrono::steady_clock::now();

    // Расстояние сравнивается в квадратах: корень здесь считать незачем, а
    // считать его пришлось бы на каждую пару игроков в каждом такте.
    const float reach = config_.streamDistance * config_.streamDistance;

    ++tick_;

    // Первый проход: выжимка из реестра и снимки, записанные подряд.
    //
    // Снимок каждого пишется в байты ровно один раз за такт. Прежде он
    // собирался заново для каждого, кто его увидит, — то есть столько раз,
    // сколько у человека соседей. При тысяче игроков и сотне соседей у каждого
    // это сто тысяч сборок за такт вместо тысячи.
    slots_.clear();
    slots_.reserve(players_.size());
    blob_.clear();

    for (const auto& [peer, player] : players_) {
        StateSlot slot;
        slot.peer = peer;
        slot.id = player.id;
        slot.position = player.position;

        // Молчащего писать незачем: получатель держит его на месте сам. Раз в
        // секунду — всё же пишем: подошедший рядом со стоящим иначе не увидел
        // бы его вовсе.
        const bool stale = now - player.stateSentAt >= kStateKeepalive;

        // Молчал ли он до этого снимка. Считается по тому же времени, по
        // которому решается и всё прочее: stateSentAt — это когда снимок этого
        // игрока писался в последний раз.
        slot.quiet = now - player.stateSentAt >= kQuietGap;

        if (player.stateFresh || stale) {
            const std::size_t before = blob_.bytes().size();
            player.state.write(blob_);

            slot.offset = static_cast<std::uint32_t>(before);
            slot.length = static_cast<std::uint32_t>(blob_.bytes().size() - before);
        }

        slots_.push_back(slot);
    }

    const std::vector<std::uint8_t>& blob = blob_.bytes();

    // Через сколько тактов снимок уходит на средний и дальний круг. Считается
    // один раз на такт, а не на каждую пару: пар столько же, сколько игроков в
    // квадрате.
    // Не mid и far: far — это макрос из windows.h, оставшийся там с
    // шестнадцатиразрядных времён. Он пуст, и переменная с таким именем просто
    // исчезает, оставляя после себя невнятную ошибку разбора.
    const unsigned int midEvery = everyTicks(config_.tickRate, kMidRate);
    const unsigned int farEvery = everyTicks(config_.tickRate, kFarRate);

    // Второй проход: каждому получателю — его связка.
    for (const StateSlot& listener : slots_) {
        const auto send = [this, &listener](shared::ByteView bundle) {
            // Ненадёжным каналом, как и прежде: потерянная связка дешевле
            // заменяется следующей, чем переотправляется устаревшей.
            host_->send(listener.peer, shared::Channel::State, bundle);
        };

        bundler_.reset(shared::PlayerStates::kId);

        for (const StateSlot& other : slots_) {
            if (other.peer == listener.peer || other.length == 0) {
                continue;
            }

            // Только тем, кто рядом. Игроку незачем знать, как бежит человек за
            // полкилометра: он его всё равно не увидит, а снимков это половина
            // всего, что ходит по сети.
            const float distance = shared::distanceSquared(listener.position, other.position);
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
            const unsigned int every =
                distance > kFarRing ? farEvery : distance > kNearRing ? midEvery : 1U;

            // Молчавшего прореживать нельзя: его снимок и без того редок, и
            // пропущенный он оставит получателя без вестей на столько, что тот
            // сочтёт игрока пропавшим и уберёт его персонажа.
            if (every > 1 && !other.quiet && (tick_ + other.id) % every != 0) {
                continue;
            }

            bundler_.add(shared::ByteView{blob.data() + other.offset, other.length}, send);
        }

        bundler_.finish(send);
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

void Server::broadcastVehicleStates() {
    const auto now = std::chrono::steady_clock::now();

    // Первый проход: снимки машин, о которых с прошлого такта что-то пришло.
    //
    // Каждый пишется в байты ровно один раз. Прежде снимок пересылался
    // немедленно и отдельным пакетом каждому, кто машину видит: в пробке из
    // двадцати машин это четыреста пакетов в секунду на человека, и заголовки
    // весили больше половины всего этого.
    vehicleSlots_.clear();
    vehicleBlob_.clear();

    for (const auto& [id, vehicle] : vehicles_.all()) {
        if (vehicle.stateAt <= vehiclesSweptAt_) {
            continue;
        }

        VehicleSlot slot;
        slot.id = id;
        slot.position = vehicle.state.position;
        slot.owner = vehicle.owner;

        const std::size_t before = vehicleBlob_.bytes().size();
        vehicle.state.write(vehicleBlob_);

        slot.offset = static_cast<std::uint32_t>(before);
        slot.length = static_cast<std::uint32_t>(vehicleBlob_.bytes().size() - before);

        vehicleSlots_.push_back(slot);
    }

    vehiclesSweptAt_ = now;

    if (vehicleSlots_.empty()) {
        return;
    }

    const std::vector<std::uint8_t>& blob = vehicleBlob_.bytes();

    // Те же круги и та же частота, что и у игроков.
    const unsigned int midEvery = everyTicks(config_.tickRate, kMidRate);
    const unsigned int farEvery = everyTicks(config_.tickRate, kFarRate);

    // Второй проход: каждому получателю — его связка.
    for (const auto& [peer, player] : players_) {
        const auto send = [this, listener = peer](shared::ByteView bundle) {
            host_->send(listener, shared::Channel::State, bundle);
        };

        bundler_.reset(shared::VehicleStates::kId);

        for (const VehicleSlot& slot : vehicleSlots_) {
            // Ведущему его же снимок не нужен: он его и прислал, и у него
            // машина живая, а не показанная.
            if (slot.owner == player.id) {
                continue;
            }

            // Только те машины, о которых получателю уже рассказано. Раздача
            // ведёт этот список сама, с расстоянием и слоем мира внутри, — а
            // снимок машины, о которой не объявляли, получатель всё равно
            // отбросит.
            if (!player.streamed.contains(slot.id)) {
                continue;
            }

            // Круги по расстоянию — те же, что у игроков, и по той же причине.
            // Машина в двух шагах должна ехать плавно, машина за триста метров
            // — просто быть там, где она есть. Прежняя пересылка «немедленно и
            // всем, кто видит» проредить снимки не давала вовсе.
            const float distance = shared::distanceSquared(player.position, slot.position);

            const unsigned int every =
                distance > kFarRing ? farEvery : distance > kNearRing ? midEvery : 1U;

            // Вразнобой, как и у игроков: добавка номера машины разводит
            // дальних по разным тактам. Без неё все они пришли бы одним, и сеть
            // шла бы рывками.
            if (every > 1 && (tick_ + slot.id) % every != 0) {
                continue;
            }

            bundler_.add(shared::ByteView{blob.data() + slot.offset, slot.length}, send);
        }

        bundler_.finish(send);
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

    // Разослан снимок будет в ближайшем такте, связкой вместе с остальными
    // машинами (broadcastVehicleStates), а не отсюда и не немедленно.
    //
    // Прежде он уходил ровно здесь — отдельным пакетом каждому, кто машину
    // видит. Это стоило сорока с лишним байт заголовков UDP и ENet на каждую
    // пару «машина — зритель» двадцать раз в секунду и не давало ни отложить
    // снимок, ни проредить его по расстоянию: пересылка знала только про ту
    // машину, что пришла, и ничего — про такт.
}

void Server::handleWeaponFired(net::PeerId peer, shared::WeaponFired fired) {
    const Player* const player = players_.findByPeer(peer);
    if (player == nullptr) {
        return;
    }

    // Кто выстрелил, решает сервер, а не пришедший пакет. Поверив клиенту, мы
    // позволили бы ему стрелять от чужого имени.
    fired.playerId = player->id;

    // Всем, кто рядом и в том же слое мира, кроме самого стрелявшего: у него
    // выстрел уже случился по-настоящему, и повторять его — значит выстрелить
    // дважды.
    //
    // Надёжным каналом, в отличие от снимков: потерянный снимок заменит
    // следующий, а потерянный выстрел не повторится — у одного зрителя будет
    // воронка от ракеты, у другого целая машина.
    broadcastNear(player->position, shared::Channel::Control, fired, player->dimension, peer);
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

    broadcastNear(vehicle->state.position, shared::Channel::Control, appearance,
                  vehicle->dimension, peer);
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
                shared::distanceSquared(player.position, object.position) <= appears &&
                script::dimensionsMeet(player.dimension, object.dimension);

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

            const bool keep =
                object != nullptr &&
                shared::distanceSquared(player.position, object->position) <= vanishes &&
                script::dimensionsMeet(player.dimension, object->dimension);

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

void Server::streamPeds() {
    const float appears = config_.streamDistance * config_.streamDistance;
    const float vanishes = appears * 1.21F;

    for (auto& [peer, player] : players_) {
        for (const auto& [id, ped] : peds_.all()) {
            const bool nearby =
                shared::distanceSquared(player.position, ped.state.position) <= appears &&
                script::dimensionsMeet(player.dimension, ped.dimension);

            if (!nearby || player.streamedPeds.contains(id)) {
                continue;
            }

            sendTo(peer, ped.state);
            player.streamedPeds.insert(id);
        }

        for (auto it = player.streamedPeds.begin(); it != player.streamedPeds.end();) {
            const PedDirectory::Ped* const ped = peds_.find(*it);

            const bool keep =
                ped != nullptr &&
                shared::distanceSquared(player.position, ped->state.position) <= vanishes &&
                script::dimensionsMeet(player.dimension, ped->dimension);

            if (keep) {
                ++it;
                continue;
            }

            shared::PedRemoved removed;
            removed.id = *it;
            sendTo(peer, removed);

            it = player.streamedPeds.erase(it);
        }
    }
}

void Server::pedChanged(shared::PedId id) {
    const PedDirectory::Ped* const ped = peds_.find(id);
    if (ped == nullptr) {
        return;
    }

    // Только тем, кому прохожий уже объявлен. Остальным его объявит раздача — и
    // объявит вместе с этим же состоянием: оно у прохожего одно, второго нет.
    // Разослав его всем, мы завели бы куклу у того, кто до неё ещё не дошёл, а
    // раздача потом объявила бы её второй раз.
    for (const auto& [peer, player] : players_) {
        if (player.streamedPeds.contains(id)) {
            sendTo(peer, ped->state);
        }
    }
}

void Server::pedRemoved(shared::PedId id) {
    shared::PedRemoved removed;
    removed.id = id;

    // Только тем, кто его видел: остальным нечего убирать, а сообщение о
    // незнакомом номере они молча проглотят — и это тем более незачем.
    for (auto& [peer, player] : players_) {
        if (player.streamedPeds.erase(id) != 0) {
            sendTo(peer, removed);
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
                shared::distanceSquared(player.position, vehicle.state.position) <= appears &&
                script::dimensionsMeet(player.dimension, vehicle.dimension);

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

            // Ушедшая в другой слой мира убирается тем же порядком, что и
            // отдалившаяся: для получателя это одно и то же — перестать её
            // показывать.
            const bool keep =
                vehicle != nullptr &&
                shared::distanceSquared(player.position, vehicle->state.position) <= vanishes &&
                script::dimensionsMeet(player.dimension, vehicle->dimension);

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

void Server::handleVehicleDamage(net::PeerId peer, const shared::VehicleDamageReport& report) {
    const Player* attacker = players_.findByPeer(peer);
    if (attacker == nullptr || !report.harm.any()) {
        return;
    }

    const VehicleDirectory::Vehicle* vehicle = vehicles_.find(report.vehicle);
    if (vehicle == nullptr) {
        return;
    }

    // Ведущего нет — машину никто не считает, и отнять у неё прочность некому.
    // Это не потеря: стоящая без присмотра машина никому и не мешает, а первый
    // же подошедший станет её ведущим и получит следующее попадание.
    if (vehicle->owner == shared::kInvalidPlayerId) {
        return;
    }

    // Стрелявший не может быть ведущим той же машины: свою машину он бьёт у
    // себя по-настоящему и рассказывает о новой прочности снимком. Пришедшее
    // сюда означало бы, что прочность отнимут дважды.
    if (vehicle->owner == attacker->id) {
        return;
    }

    // Попадание с другого конца карты не бывает. Проверка та же, что и у людей,
    // и такая же грубая: точную линию выстрела сервер не построит — мира у него
    // нет, — а отличить перестрелку от доклада за километр может.
    const float reach = config_.streamDistance * config_.streamDistance;
    if (shared::distanceSquared(attacker->position, vehicle->state.position) > reach) {
        spdlog::warn("player \"{}\" (id {}) reported a hit on a vehicle too far away",
                     attacker->nickname, attacker->id);
        return;
    }

    // Убыль обрезается тем же пределом, что и урон человеку, и по той же
    // причине: число могло испортиться по дороге или прийти от ошибки в
    // клиенте, а выстрел при этом был настоящим.
    shared::VehicleDamaged passed;
    passed.vehicle = report.vehicle;
    passed.harm.body = std::min(report.harm.body, config_.maxDamagePerHit);
    passed.harm.engine = std::min(report.harm.engine, config_.maxDamagePerHit);
    passed.harm.tank = std::min(report.harm.tank, config_.maxDamagePerHit);

    const Player* owner = players_.findById(vehicle->owner);
    if (owner == nullptr) {
        return;
    }

    sendTo(owner->peer, passed);

    // И скриптам — здесь же, а не у ведущего. Тем же порядком, что и смерть от
    // чужой руки: объявляет тот, кто знает обоих участников, а знает их только
    // сервер. Ведущему стрелявший не назван вовсе — ему он и не нужен.
    script::Event damage;
    damage.kind = script::EventKind::VehicleDamage;
    damage.vehicle = script::Vehicle{core_, report.vehicle};
    damage.killer = script::Player{core_, attacker->id};
    damage.weapon = report.weapon;
    damage.harm = passed.harm;

    events_.dispatch(damage);
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
        spdlog::warn("player \"{}\" (id {}) reported a hit from too far away", attackerName, attackerId);
        return;
    }

    // Скриптам попадание объявляется до того, как оно применено, и это
    // единственное место, где его ещё можно отменить. Отсюда и порядок: сперва
    // проверки правдоподобия — врать о попадании за километр не позволено и
    // скрипту, — потом слово режима, и лишь затем сам урон.
    //
    // Отказ здесь означает «попадания не было»: ни здоровья, ни брони, ни
    // сообщения жертве. Так же поступает и alt:V.
    {
        script::Event shot;
        shot.kind = script::EventKind::WeaponDamage;
        shot.player = script::Player{core_, victim->id};
        shot.killer = script::Player{core_, attackerId};
        shot.weapon = report.weapon;
        shot.healthHarm = amount;

        if (!events_.dispatch(shot)) {
            return;
        }
    }

    // Броня принимает удар первой и целиком. Так же считает и сама игра, и
    // расходиться с ней здесь незачем: игрок судит о своей броне по её счётчику.
    std::uint16_t left = amount;

    const std::uint16_t absorbed = std::min(left, victim->armour);
    victim->armour = static_cast<std::uint16_t>(victim->armour - absorbed);
    left = static_cast<std::uint16_t>(left - absorbed);

    const std::uint16_t healthHarm =
        left >= victim->health ? victim->health : left;

    victim->health = left >= victim->health ? 0 : static_cast<std::uint16_t>(victim->health - left);

    // Здоровье — самому пострадавшему: остальные узнают его из снимка, который
    // сервер и без того правит на своё значение перед рассылкой.
    sendHealth(*victim, attackerId);

    // Урон объявляется здесь, а не там, где приходит сообщение, и это
    // существенно: доводы alt:V разводят убыль здоровья и убыль брони, а развела
    // их только что броня — приняв удар первой и целиком. До этого места
    // известна одна общая цифра, и по ней режим не отличил бы спасший
    // бронежилет от пробитого.
    {
        script::Event hurt;
        hurt.kind = script::EventKind::PlayerDamage;
        hurt.player = script::Player{core_, victim->id};
        hurt.killer = script::Player{core_, attackerId};
        hurt.weapon = report.weapon;
        hurt.healthHarm = healthHarm;
        hurt.armourHarm = absorbed;

        (void)events_.dispatch(hurt);
    }

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

void Server::announceWeapon(Player& player) {
    const std::uint32_t weapon = player.state.weapon;

    // Что навинчено на ствол, знает сервер: снаряжение принадлежит ему целиком.
    // Оружия, которого он не выдавал, в списке нет — и это верно: собрать его
    // мог только он.
    const auto slot = std::ranges::find_if(player.loadout, [weapon](const auto& carried) {
        return carried.weapon == weapon;
    });

    const bool known = weapon != 0 && slot != player.loadout.end();

    const std::uint8_t tint = known ? slot->tint : 0;

    // Сравниваем, ничего не собирая. Зовут это на каждый снимок каждого игрока —
    // то есть сотню раз в секунду на сотню человек, — а собранное объявление
    // несёт список насадок, и всякая сборка означала бы выделение памяти в
    // самом горячем месте сервера. Меняется же оно раз в несколько минут.
    const bool same = player.shownWeapon.weapon == weapon &&
                      player.shownWeapon.tint == tint &&
                      (known ? player.shownWeapon.components == slot->components
                             : player.shownWeapon.components.empty());

    if (same) {
        return;
    }

    shared::PlayerWeapon look;
    look.playerId = player.id;
    look.weapon = weapon;
    look.tint = tint;

    if (known) {
        look.components = slot->components;
    }

    player.shownWeapon = look;

    // Всем, а не только тем, кто игрока видит, и надёжным каналом. Собранное
    // оружие живёт минутами: вышедший из-за угла обязан увидеть его сразу, а не
    // после ближайшей смены ствола. Самому игроку — тоже: у него оно уже такое,
    // но лишнее сообщение раз в несколько минут дешевле, чем второе правило о
    // том, кому его слать.
    broadcast(look);
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

    // Клиент говорит, что поднял свою половину ресурсов.
    //
    // Служебное имя, а не обычное событие: его посылает сам клиент, а не режим,
    // и подменить его чужим ресурс не должен. Приставка с двумя подчёркиваниями
    // говорит читающему то же самое.
    if (event.name == shared::kClientReadyEvent) {
        Player* const ready = players_.findByPeer(peer);

        if (ready != nullptr) {
            announcePlayerReady(*ready);
        }

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

    // Заодно и остальным: скрипт мог навинтить глушитель на то самое оружие,
    // которое игрок сейчас держит.
    if (Player* const carrier = players_.findById(player.id); carrier != nullptr) {
        announceWeapon(*carrier);
    }
}

void Server::appearanceChanged(const Player& player) {
    // Всем без исключения, включая самого игрока, — и в этом отличие от
    // внешности, пришедшей от клиента. Ту он объявил сам и у себя уже надел, и
    // возвращать её ему значило бы переодевать его в то же самое каждый раз.
    // Эту ему назначил скрипт, и узнать о ней ему больше неоткуда.
    //
    // Без except — то есть и отправителю тоже: у broadcast это и означает
    // «всем без изъятия».
    if (player.appearance) {
        broadcast(*player.appearance);
    }
}

void Server::teleported(const Player& player, const shared::Vec3& position) {
    shared::PlayerTeleport teleport;
    teleport.position = position;

    sendTo(player.peer, teleport);
}

void Server::seated(const Player& player, shared::VehicleId vehicle, std::int8_t seat) {
    shared::PlayerIntoVehicle message;
    message.vehicle = vehicle;
    message.seat = seat;

    sendTo(player.peer, message);
}

void Server::kicked(const Player& player, std::string_view reason) {
    spdlog::info("Player {} kicked: {}", player.nickname,
                 reason.empty() ? std::string_view{"no reason given"} : reason);

    // Причина уходит строкой чата, а не своим сообщением, и это осознанный
    // выбор, а не откладывание работы. Своё сообщение потребовало бы номера в
    // протоколе, обработчика у клиента и показа где-то поверх кадра — то есть
    // ещё одного способа сказать игроку одну строку там, где такой способ уже
    // есть и работает у всех сборок клиента.
    if (!reason.empty()) {
        shared::ChatLine line;
        line.kind = shared::ChatKind::System;
        line.text = std::string{reason};

        sendTo(player.peer, line);
    }

    // Выталкивание до разрыва обязательно: закрытое соединение уносит с собой всё
    // не успевшее уйти, и игрок увидел бы молчаливый обрыв вместо объяснения.
    // Тем же порядком поступает и отказ в подключении.
    host_->flush();
    host_->disconnect(player.peer);
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

/// Кому адресовать просьбу о машине: её ведущему.
///
/// Пусто — вести машину некому, и просить не о чем: сервер уже поправил её у
/// себя, а спорить с этим некому.
net::PeerId Server::ownerPeerOf(shared::VehicleId id) const {
    const VehicleDirectory::Vehicle* const vehicle = vehicles_.find(id);
    if (vehicle == nullptr || vehicle->owner == shared::kInvalidPlayerId) {
        return net::kInvalidPeerId;
    }

    const Player* const owner = players_.findById(vehicle->owner);
    return owner == nullptr ? net::kInvalidPeerId : owner->peer;
}

void Server::vehicleTeleported(shared::VehicleId id, const shared::Vec3& position, float heading) {
    const net::PeerId peer = ownerPeerOf(id);
    if (peer == net::kInvalidPeerId) {
        return;
    }

    shared::VehicleTeleport message;
    message.id = id;
    message.position = position;
    message.heading = heading;

    sendTo(peer, message);
}

void Server::vehicleRepaired(shared::VehicleId id) {
    const net::PeerId peer = ownerPeerOf(id);
    if (peer == net::kInvalidPeerId) {
        return;
    }

    shared::VehicleRepair message;
    message.id = id;

    sendTo(peer, message);
}

void Server::vehicleAppearanceChanged(shared::VehicleId id) {
    const VehicleDirectory::Vehicle* const vehicle = vehicles_.find(id);
    if (vehicle == nullptr || !vehicle->appearance) {
        return;
    }

    // Всем, кто машину видит, и никого не исключая: наложить внешность обязан
    // каждый у себя. Ведущий здесь не в особом положении — у него машина живёт
    // по-настоящему, но перекрашивает её он тем же вызовом, что и остальные.
    //
    // Тому, у кого машины ещё нет, она придёт вместе с ней: раздача шлёт
    // внешность следом за появлением. А тому, кому внешность пришла раньше
    // машины, клиент запомнит её и наденет, когда машина появится.
    broadcastNear(vehicle->state.position, shared::Channel::Control, *vehicle->appearance,
                  vehicle->dimension);
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

void Server::objectMoved(shared::ObjectId id) {
    const ObjectDirectory::Object* const object = objects_.find(id);
    if (object == nullptr) {
        return;
    }

    // Тем, у кого предмет уже стоит, — новое объявление того же предмета. Клиент
    // толкует повторное объявление как «поправь», а не «заведи второй»
    // (Objects::add), и потому своего сообщения о переезде не понадобилось.
    shared::ObjectAdded moved;
    moved.id = id;
    moved.model = object->model;
    moved.position = object->position;
    moved.rotation = object->rotation;

    for (const auto& [peer, player] : players_) {
        if (player.streamedObjects.contains(id)) {
            sendTo(peer, moved);
        }
    }

    // А тем, у кого его нет, он мог как раз приехать в поле зрения. Раздача
    // ходит по расстоянию и разберётся сама — её нужно только поторопить.
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

void Server::blipChanged(shared::BlipId id) {
    broadcastDrawn(blips_, id);
}

void Server::blipRemoved(shared::BlipId id) {
    shared::BlipRemoved message;
    message.id = id;

    // Всем без разбора слоёв: у тех, кто метки не видел, её и так нет, а
    // выяснять, кто видел, значило бы помнить это на каждого. То же и у двух
    // следующих.
    broadcast(message);
}

void Server::markerChanged(shared::MarkerId id) {
    broadcastDrawn(markers_, id);
}

void Server::markerRemoved(shared::MarkerId id) {
    shared::MarkerRemoved message;
    message.id = id;
    broadcast(message);
}

void Server::checkpointChanged(shared::CheckpointId id) {
    broadcastDrawn(checkpoints_, id);
}

void Server::checkpointRemoved(shared::CheckpointId id) {
    shared::CheckpointRemoved message;
    message.id = id;
    broadcast(message);
}

template<typename Directory, typename Removed>
void Server::redrawKindFor(const Directory& directory, const Player& player,
                           std::int32_t previous) {
    for (const auto& [id, entry] : directory.all()) {
        const bool saw = script::dimensionsMeet(previous, entry.dimension);
        const bool sees = script::dimensionsMeet(player.dimension, entry.dimension);

        if (saw == sees) {
            continue;
        }

        if (sees) {
            sendTo(player.peer, entry.state);
            continue;
        }

        Removed removed;
        removed.id = id;
        sendTo(player.peer, removed);
    }
}

void Server::exploded(const shared::Explosion& explosion, std::int32_t dimension) {
    // Надёжным каналом, в отличие от снимков. Потерянный снимок заменит
    // следующий; потерянный взрыв не повторится никогда, и один из зрителей
    // остался бы с целой машиной там, где у остальных воронка.
    //
    // Всем, кто до этого места достаёт, включая того, у кого рвануло: взрыв
    // заводит ресурс, а не игрок, и «отправителя», которого стоило бы
    // пропустить, здесь нет.
    broadcastNear(explosion.position, shared::Channel::Control, explosion, dimension);
}

void Server::animationPlayed(const Player& player, const shared::PlayerAnimation& animation) {
    // Всем, кто рядом и в том же слое, включая самого игрока: у него движение
    // играет настоящий персонаж, у остальных — кукла.
    //
    // Надёжным каналом: движение — это событие, и потерянное не повторится.
    // Дальним не шлём вовсе — куклы у них нет, а начатое движение к их приезду
    // всё равно кончится.
    broadcastNear(player.position, shared::Channel::Control, animation, player.dimension,
                  net::kInvalidPeerId);
}

void Server::dimensionChanged(const Player& player, std::int32_t previous) {
    redrawKindFor<BlipDirectory, shared::BlipRemoved>(blips_, player, previous);
    redrawKindFor<MarkerDirectory, shared::MarkerRemoved>(markers_, player, previous);
    redrawKindFor<CheckpointDirectory, shared::CheckpointRemoved>(checkpoints_, player, previous);
}

std::string Server::addressOf(const Player& player) const {
    return host_ == nullptr ? std::string{} : host_->addressOf(player.peer);
}

std::uint32_t Server::latencyOf(const Player& player) const {
    return host_ == nullptr ? 0 : host_->latencyOf(player.peer);
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
        spdlog::info("chat: [server] {}", line.text);
        broadcast(line);
        return;
    }

    const Player* target = players_.findById(to);
    if (target == nullptr) {
        return;
    }

    sendTo(target->peer, line);
}

void Server::tellScriptsAboutChanges(const Player& player, const shared::PlayerState& before,
                                     const shared::PlayerState& after) {
    // Событий здесь пять, и все они — разница, а не сообщение. Если разницы
    // нет, не должно быть и работы: снимок приходит тридцать раз в секунду на
    // каждого игрока, и лишний обход списка подписчиков тут стоит дороже, чем
    // где бы то ни было ещё.
    const bool satBefore = shared::has(before.flags, shared::PlayerFlag::InVehicle);
    const bool satAfter = shared::has(after.flags, shared::PlayerFlag::InVehicle);

    const bool enteringBefore = shared::has(before.flags, shared::PlayerFlag::EnteringVehicle);
    const bool enteringAfter = shared::has(after.flags, shared::PlayerFlag::EnteringVehicle);

    const auto tell = [&](script::EventKind kind, shared::VehicleId vehicle, std::int8_t seat,
                          std::int8_t seatWas) {
        script::Event event;
        event.kind = kind;
        event.player = script::Player{core_, player.id};
        event.vehicle = script::Vehicle{core_, vehicle};
        event.seat = seat;
        event.seatWas = seatWas;

        (void)events_.dispatch(event);
    };

    // Полез в машину. Объявляется на подъёме признака, а не на каждом снимке,
    // пока он стоит: вход длится полторы секунды, то есть полсотни снимков.
    if (enteringAfter && !enteringBefore) {
        tell(script::EventKind::PlayerEnteringVehicle, after.vehicleId, after.seat,
             shared::kNoSeat);
    }

    if (satAfter && !satBefore) {
        tell(script::EventKind::PlayerEnteredVehicle, after.vehicleId, after.seat,
             shared::kNoSeat);
    } else if (!satAfter && satBefore) {
        // Вылез. Машина и место берутся из прошлого снимка, и по-другому нельзя:
        // к этому мгновению у него уже нет ни того ни другого.
        tell(script::EventKind::PlayerLeftVehicle, before.vehicleId, before.seat,
             shared::kNoSeat);
    } else if (satAfter && satBefore && after.seat != before.seat &&
               after.vehicleId == before.vehicleId) {
        // Пересел, не выходя. Проверка на ту же машину обязательна: пересадка в
        // другую машину — это выход и вход, и объявлять её пересадкой значило
        // бы соврать дважды.
        tell(script::EventKind::PlayerChangedVehicleSeat, after.vehicleId, after.seat,
             before.seat);
    }

    // Оружие в руках. Ноль — безоружен, и переход в ноль такое же событие, как
    // и всякий другой: убрать ствол значит его сменить.
    if (after.weapon != before.weapon) {
        script::Event event;
        event.kind = script::EventKind::PlayerWeaponChange;
        event.player = script::Player{core_, player.id};
        event.weaponWas = before.weapon;
        event.weapon = after.weapon;

        (void)events_.dispatch(event);
    }
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

    spdlog::info("chat: [{}] {}", line.nickname.empty() ? "server" : line.nickname, line.text);

    broadcast(line);
}

void Server::reject(net::PeerId peer, shared::RejectReason reason) {
    spdlog::info("connection {} refused: {}", peer, describe(reason));

    shared::ServerReject message;
    message.reason = reason;
    sendTo(peer, message);

    // Отказ нужно вытолкнуть до разрыва, иначе клиент увидит молчаливое
    // закрытие и не поймёт причины.
    host_->flush();
    host_->disconnect(peer);
}

/// Сколько может весить файл, чтобы уехать в свёрток.
///
/// Шестнадцать мегабайт. Не больше — свёрток лежит у сервера в памяти целиком, а
/// клиент качает его одним куском с потолком в две сотни мегабайт, и один
/// толстый файл внутри означал бы, что не приедет ничего. Не меньше — собранный
/// бандл режима со встроенными картинками легко берёт несколько мегабайт, и
/// выставить его наружу было бы ровно тем, чего свёрток должен не допускать.
constexpr std::uintmax_t kMaxBundledFile = 16ULL * 1024 * 1024;

void Server::startResources() {
    for (const std::string& complaint : catalog_.load(config_.resourceDirectory,
                                                      config_.resources)) {
        spdlog::warn("resource: {}", complaint);
    }

    ensureRuntimes();

    for (const ScriptResource& resource : catalog_.all()) {
        // Ресурс без main ничего не исполняет — он только раздаёт файлы.
        //
        // Так устроены ресурсы с моделями: каталог со `stream` внутри и
        // описаниями рядом. Машину для него искать незачем, а найдя, она
        // отказала бы «не задан main» — то есть пожаловалась бы на то, что не
        // поломка.
        const bool runs = !resource.main.empty();

        if (runs) {
            // Машина выбирается по типу. О ресурсе на языке, которого сервер не
            // понимает, говорится прямо, а не молчанием: молча пропущенный
            // игровой режим хозяин будет искать долго.
            Runtime* const runtime = runtimeFor(resource.type);

            if (runtime == nullptr) {
                spdlog::error("resource \"{}\": the server has no runtime for type \"{}\"",
                              resource.name, resource.type);
                continue;
            }

            std::string error;

            if (!runtime->start(resource, error)) {
                spdlog::error("resource \"{}\" did not start: {}", resource.name, error);
                continue;
            }
        }

        // Клиентская половина делится надвое, и делится не по вкусу.
        //
        // Модели и текстуры (`stream/`) уходят отдельными файлами: прятать их
        // незачем — они и так расходятся по интернету, — а свёрток из них вышел
        // бы в гигабайты и лёг бы клиенту в память целиком.
        //
        // Всё остальное — код режима и страницы интерфейса — уходит одним
        // свёртком. Иначе исходники лежат у игрока обычным текстом и
        // открываются блокнотом; у alt:V так же, и это то, ради чего свёрток
        // заведён.
        std::vector<std::string> packable;

        for (const std::string& file : resource.clientFiles) {
            std::error_code failed;
            const std::uintmax_t size = std::filesystem::file_size(resource.root / file, failed);

            if (goesIntoBundle(file, resource.dataFiles.contains(file),
                               failed ? 0 : size)) {
                packable.push_back(file);
                continue;
            }

            // О том, что осталось видимым, говорится в журнал: хозяин сервера
            // вправе знать, какие его файлы лежат у игрока обычным текстом.
            spdlog::debug("resource \"{}\": {} is served as a plain file", resource.name, file);

            if (!resources_.add(resource.root / file,
                                std::format("{}/{}", resource.name, file))) {
                spdlog::warn("resource \"{}\": file \"{}\" could not be read", resource.name,
                             file);
            }
        }

        if (!packable.empty()) {
            const std::string hash = resources_.addBundle(resource.root, packable);

            if (hash.empty()) {
                spdlog::warn("resource \"{}\": the bundle was not built", resource.name);
            } else {
                for (const std::string& file : packable) {
                    // Имя составное, как и у отдельных файлов: клиент знает
                    // ресурс по нему же.
                    bundled_.emplace(std::format("{}/{}", resource.name, file), hash);
                }
            }
        }

        if (runs) {
            spdlog::info("Resource \"{}\" started ({}), {} files for the client", resource.name,
                         resource.type, resource.clientFiles.size());
        } else {
            spdlog::info("Resource \"{}\" serves files without running anything: {}", resource.name,
                         resource.clientFiles.size());
        }
    }
}

bool Server::goesIntoBundle(std::string_view file, bool described, std::uintmax_t size) {
    // Описание игры не прячется никогда, и это первая проверка, а не последняя.
    // Читает такие файлы загрузчик данных игры, а подать ему свёрток нечем: он
    // умеет путь на диске и больше ничего.
    if (described || shared::isGameDescription(file)) {
        return false;
    }

    // Крупное остаётся снаружи независимо от рода.
    //
    // Свёрток лежит у сервера в памяти целиком и качается клиентом одним куском,
    // с потолком в две сотни мегабайт. Один огромный файл внутри — и не приедет
    // весь свёрток, то есть режим не поднимется вовсе. Снаружи он в худшем
    // случае виден; это несравнимо меньшая беда.
    if (size > kMaxBundledFile) {
        return false;
    }

    // Исходник режима прячется всегда — где бы он ни лежал.
    //
    // **Каталог `stream/` здесь ничего не решает, и это исправление.** Прежде он
    // решал всё: под ним по уговору alt:V и FiveM лежат модели, и оттуда не
    // бралось ничего. Но чужие режимы кладут туда и свой интерфейс — у режима,
    // на котором это проверялось, под `stream/browsers/` нашлось 504 файла кода
    // и разметки, три четверти всей его клиентской логики, — и все они ложились
    // игроку на диск открытым текстом. Место файла не говорит о его роде.
    //
    // Безопасно это потому, что до игры файл ресурса доходит ровно двумя
    // дорогами, и обе для исходника закрыты: стримингу клиент их не объявляет
    // сам (`worthStreaming` спрашивает тот же общий список), а загрузчику данных
    // попадает лишь названное в `[meta]` — оно отсечено выше.
    if (shared::isSourceOrMarkup(file)) {
        return true;
    }

    // Дальше — не исходники, а всё прочее: картинки, звуки, шрифты, модели.
    // Здесь `stream/` по-прежнему решает: под ним лежит то, что читает игра.
    if (file.starts_with("stream/") || file.find("/stream/") != std::string_view::npos) {
        return false;
    }

    // Список — белый, и это выбрано нарочно. Прячем то, что читает наш же
    // клиент; всё, чего мы здесь не узнали, остаётся отдельным файлом, потому
    // что его может читать сама игра. Ошибка в белом списке — файл, оставшийся
    // видимым; ошибка в чёрном была бы игрой без машины и без карты.
    static constexpr std::array kMedia = std::to_array<std::string_view>(
        {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".ico", ".woff", ".woff2", ".ttf",
         ".otf", ".eot", ".mp3", ".ogg", ".wav"});

    const std::size_t dot = file.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }

    std::string suffix{file.substr(dot)};
    std::ranges::transform(suffix, suffix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return std::ranges::find(kMedia, suffix) != kMedia.end();
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
            spdlog::error("the JS engine did not start: {}", error);
            spdlog::error("JS resources will not work");
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

        spdlog::info("connection {} did not introduce itself in time", it->first);
        host_->disconnect(it->first);
        it = awaitingHello_.erase(it);
    }
}

} // namespace oxymp::server
