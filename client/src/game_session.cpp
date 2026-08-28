#include "game_session.hpp"

#include "game/asset_kind.hpp"
#include "game/environment.hpp"

#include "game/native_table.hpp"

#include <oxymp/client/interpolation.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

namespace oxymp::client {
namespace {

/// Насколько далеко разрешено доводить состояние, посчитанное сетевым потоком.
///
/// Десятая доля секунды — заведомо больше обычного разнобоя между потоками
/// (тридцать миллисекунд в худшем случае) и заведомо меньше того, что означало
/// бы застрявший сетевой поток. Дальше доводить нечего: там уже не поправка, а
/// гадание, а само достраивание движения сетевая сторона делает и без нас.
constexpr float kMaxViewAge = 0.1F;

/// Сколько секунд прошло с того мгновения, на которое состояние посчитано.
[[nodiscard]] float ageOf(std::chrono::steady_clock::time_point computedAt,
                          std::chrono::steady_clock::time_point now) {
    if (computedAt == std::chrono::steady_clock::time_point{} || now <= computedAt) {
        return 0.0F;
    }

    return std::min(std::chrono::duration<float>{now - computedAt}.count(), kMaxViewAge);
}


/// Точка появления на случай, если сервер её не назвал.
///
/// Запасная, а не главная: где появляться, решает сервер — это свойство сессии,
/// и хозяин меняет её одной строкой в server.cfg. Здесь она нужна для того
/// мгновения, когда игрок уже в мире, а приветствие ещё не пришло: поставить его
/// в начало координат означало бы уронить под карту.
constexpr shared::Vec3 kFallbackSpawn{-1037.7F, -2738.0F, 20.2F};
constexpr float kSpawnHeading = 328.0F;

/// Как часто смотреть, во что одет свой игрок.
///
/// Раз в секунду с запасом: переодевает игрока сервер, и полсекунды задержки в
/// этом не заметит никто, а чтение стоит полусотни вызовов нативов.
constexpr auto kLookInterval = std::chrono::seconds{1};

/// Радиус, в котором убирается население при появлении и после смерти.
constexpr float kClearRadius = 400.0F;

/// Клавиша чата. Консоль теперь на странице меню и открывается перехватом
/// ввода, а не отсюда: своей консоли у клиента больше нет.
constexpr int kChatKey = 'T';

/// Как часто гасятся ненужные скрипты игры.
///
/// Однократной зачистки мало: сюжетные скрипты заводят друг друга, и убитый
/// пускатель миссий возвращается вместе со следующей попыткой игры продолжить
/// сюжет. Раз в секунду — достаточно редко, чтобы не быть заметным, и
/// достаточно часто, чтобы миссия не успела начаться.
constexpr auto kSweepInterval = std::chrono::seconds{1};

/// Сколько времени после появления гасить их каждый кадр.
constexpr auto kEagerSweepWindow = std::chrono::seconds{20};

/// Как часто отчитываться о том, что подавление сюжета живо.
///
/// Достаточно редко, чтобы не засорять журнал, и достаточно часто, чтобы по
/// нему было видно: молчание означает поломку, а не тишину.
constexpr auto kSweepReportInterval = std::chrono::seconds{30};

/// Сколько держать экран загрузки, дожидаясь сервера.
///
/// Ждать бесконечно нельзя: выключенный сервер оставил бы игрока на экране
/// загрузки навсегда, хотя сама игра к этому моменту уже готова. По истечении
/// срока игрок попадает в мир, а состояние соединения переезжает в уголок.
///
/// Отсчёт начинается там же, где и само подключение, — с появления игрока в
/// мире. Срок заметно больше того, после которого клиент называет молчание
/// бедой: игрок должен успеть прочитать «сервер не отвечает» на экране загрузки,
/// а не увидеть эту надпись мельком перед самым её уходом.
constexpr auto kConnectWaitLimit = std::chrono::seconds{12};

/// Как часто пересматривается внешность машины, которую мы ведём.
///
/// Цвет, номер и тюнинг за сессию меняются считанные разы, а стоит их опрос
/// полусотни вызовов нативов. Раз в две секунды — это незаметно для кадра и
/// заведомо быстрее, чем игрок успеет доехать от мастерской до чужих глаз.
constexpr auto kAppearanceInterval = std::chrono::seconds{2};

/// За сколько проявляется картинка игры, в миллисекундах.
constexpr int kFadeIn = 500;

/// Сколько выждать после того, как игрок оказался в мире.
///
/// Признаки готовности загораются раньше, чем игра доводит мир и персонажа до
/// рабочего состояния, и замена персонажа в этот промежуток её роняет.
constexpr auto kWorldSettle = std::chrono::seconds{3};

/// Сессия одна на процесс: перехват тика — простая функция без состояния, и
/// связать её с объектом иначе нельзя.
GameSession* g_session = nullptr;

void frameEntry(bool ownsResources) {
    if (g_session != nullptr) {
        g_session->onFrame(ownsResources);
    }
}

/// Сработало ли нажатие клавиши именно сейчас.
///
/// Игра опрашивается каждый кадр, а переключатель обязан срабатывать один раз
/// на нажатие, а не всё время, пока клавишу держат.
bool pressedOnce(int key) {
    static bool wasDown[256] = {};

    const bool down = (::GetAsyncKeyState(key) & 0x8000) != 0;
    const bool fired = down && !wasDown[key & 0xFF];

    wasDown[key & 0xFF] = down;
    return fired;
}

} // namespace

GameSession::GameSession(const game::EngineAddresses& addresses, const game::NativeTable& table,
                         Settings settings, const SessionStatus& status,
                         const RemoteRoster& roster, LocalState& localState, SessionMail& mail,
                         UiFeed& feed, FrameWatch& watch, game::FileDevice* files,
                         game::StreamingFiles* streamed, game::DataFiles* described,
                         game::Packfiles* archives, game::Manifests* manifests)
    : natives_(table),
      settings_(std::move(settings)),
      status_(status),
      roster_(roster),
      localState_(localState),
      mail_(mail),
      feed_(feed),
      watch_(watch),
      hud_(table),
      player_(table),
      screen_(table),
      story_(table),
      world_(table),
      frontend_(table),
      pedAnimation_(table),
      explosions_(table),
      vehicleEntry_(table),
      blips_(table),
      markers_(table),
      checkpoints_(table),
      attachments_(table),
      files_(files),
      streamed_(streamed),
      described_(described),
      archives_(archives),
      manifests_(manifests),
      placements_(table),
      gameFiles_(addresses),
      controls_(table),
      onlineMap_(table),
      appearance_(table),
      look_(table),
      respawn_(table),
      streaming_(table),
      vehicles_(table),
      remotePlayers_(table, vehicles_),
      objects_(table),
      peds_(table),
      nameplates_(table, hud_),
      sessionState_(addresses),
      networkGame_(addresses),
      netSession_(addresses, table) {
    // Попадания уходят в ту же почту, что и реплики чата: замечает их игровой
    // поток, а отправляет сетевой.
    remotePlayers_.reportDamageTo(
        [&mail = mail_](shared::PlayerId victim, std::uint16_t amount, std::uint32_t weapon) {
            mail.postDamage(victim, amount, weapon);
        });

    // Попадания по чужим машинам — той же почтой и по той же причине: замечает
    // их игровой поток, а отправляет сетевой.
    vehicles_.reportDamageTo([&mail = mail_](shared::VehicleId vehicle,
                                             const shared::VehicleHarm& harm,
                                             std::uint32_t weapon) {
        mail.postVehicleDamage(vehicle, harm, weapon);
    });
}

std::unique_ptr<GameSession> GameSession::create(const game::EngineAddresses& addresses,
                                                 Settings settings, const SessionStatus& status,
                                                 const RemoteRoster& roster,
                                                 LocalState& localState, SessionMail& mail,
                                                 UiFeed& feed, FrameWatch& watch,
                                                 game::FileDevice* files,
                                                 game::StreamingFiles* streamed,
                                                 game::DataFiles* described,
                                                 game::Packfiles* archives,
                                                 game::Manifests* manifests,
                                                 std::string& error) {
    if (g_session != nullptr) {
        error = "игровая сессия уже создана";
        return nullptr;
    }

    const game::NativeTable table{addresses};
    if (!table.valid()) {
        error = "таблица нативов недоступна";
        return nullptr;
    }

    std::unique_ptr<GameSession> session{new GameSession{
        addresses, table, std::move(settings), status, roster, localState, mail, feed, watch,
        files, streamed, described, archives, manifests}};

    // Ни одна из частей не является обязательной для остальных, поэтому
    // ненайденные нативы не отменяют сессию, а лишь отключают своё. Молчать при
    // этом нельзя: беззвучно пропавшая часть выглядит как необъяснимая пропажа
    // возможности, а не как ошибка в хешах.
    if (!session->hud_.ready()) {
        spdlog::error("drawing natives not found: the client will have no interface of its own");
    }
    if (!session->player_.ready()) {
        spdlog::error("player natives not found: spawning will not work");
    }
    if (!session->screen_.ready()) {
        spdlog::error("screen natives not found: the game splash will stay");
    }
    if (!session->story_.ready()) {
        spdlog::error("story natives not found: cutscenes and Online cannot be silenced");
    }
    if (!session->world_.ready()) {
        spdlog::error("world natives not found: pedestrians and wanted level will stay");
    }
    if (!session->appearance_.ready()) {
        spdlog::error("appearance natives not found: the ped will stay a story character");
    }
    if (!session->respawn_.ready()) {
        spdlog::error("death natives not found: the game will handle death on its own");
    }
    session->appearance_.describeHandlers();

    if (!session->frontend_.ready()) {
        spdlog::error("menu natives not found: the client cannot tell menu from gameplay");
    }
    if (!session->controls_.ready()) {
        spdlog::error("control natives not found: cheats will stay available");
    }
    if (!session->remotePlayers_.ready()) {
        spdlog::error("ped natives not found: other players will not be visible");
    }
    if (!session->vehicles_.ready()) {
        spdlog::error("vehicle natives not found: vehicles of others cannot be shown");
    }
    if (!session->nameplates_.ready()) {
        spdlog::error("world drawing natives not found: there will be no nameplates");
    }
    if (session->settings_.hostSession) {
        if (!session->netSession_.ready()) {
            spdlog::error("hosting a session was asked for, but its entry points did not resolve");
        }

        // Перехват ставится здесь, задолго до первой просьбы поднять сессию, и
        // это не запас времени ради запаса: без него сессия ложится через доли
        // секунды после подъёма, и поставить перехват в тот же кадр — значит
        // опоздать.
        std::string bailError;

        session->networkBail_ = game::NetworkBail::install(addresses, bailError);
        if (session->networkBail_ == nullptr) {
            spdlog::error("session bail was not hooked, the session will last a fraction of a second: {}",
                          bailError);
        }
    }
    if (session->settings_.forceNetworkGame != game::NetworkGame::Fake::None &&
        !session->networkGame_.ready()) {
        spdlog::error("faking the network state was asked for, but no flags were found — "
                      "the probe signatures did not resolve on this build");
    }

    // Порядок обязателен: перехват вправе вызвать обработчик сразу же, поэтому
    // сессия должна быть видна до его постановки.
    g_session = session.get();

    session->tick_ = game::ScriptTick::install(addresses, &frameEntry, error);
    if (session->tick_ == nullptr) {
        g_session = nullptr;
        return nullptr;
    }

    return session;
}

GameSession::~GameSession() {
    // Сперва снятие перехвата, и только потом отвязывание сессии: пока перехват
    // стоит, обработчик обязан оставаться рабочим.
    tick_.reset();
    g_session = nullptr;

    // Признаки возвращаются игре уже без перехвата, и это можно: они лежат в
    // обычной памяти, нативы для них не нужны, а значит и скриптовый контекст —
    // единственное, чего здесь уже нет, — не требуется.
    networkGame_.endHolding();
}

void GameSession::onFrame(bool ownsResources) {
    // Отметка ставится первой строкой кадра, а дальше уточняется по ходу: если
    // кадр встанет, сторож назовёт последнее пройденное место, а не «где-то в
    // клиенте».
    watch_.mark("frame start");

    // Работа с игрой — только от потока с обработчиком скрипта. Рисование же
    // идёт всегда: во время загрузки это единственное, что вообще можно
    // сделать, и ровно тогда свой экран и нужен.
    if (ownsResources) {
        // Первым делом и один раз за запуск. Позже переключать бессмысленно:
        // мир вокруг игрока к тому времени уже собран по одиночной разметке.
        if (settings_.onlineMap && !onlineMapEnabled_) {
            onlineMapEnabled_ = true;
            onlineMap_.enable();
        }

        // Прежде всего остального: ресурсы сервера подменяют файлы игры, и
        // подменённое должно стоять на месте раньше, чем игра его прочтёт.
        serveFiles();

        watch_.mark("suppressing the game's own rules");
        suppressGame();

        watch_.mark("advancing the session");
        advance();
    }

    // Признаки сетевого состояния — обычная память, скриптовый контекст им не
    // нужен, поэтому смотрим на них и на тех кадрах, где работать с игрой
    // нельзя. Это не мелочь: игра выставляет их как раз на загрузке, и момент
    // перехода виден только отсюда.
    networkGame_.reportChanges();

    // Отметка о живом кадре. По ней интерфейс понимает, что игра не стоит: при
    // открытом меню паузы тик не идёт вовсе, и уголок, нарисованный поверх меню,
    // застыл бы на последнем состоянии.
    feed_.beat();

    reportSessionState();
    reportPause();
    publishStage();

    watch_.mark("drawing");
    draw();

    watch_.mark("frame done");
}

void GameSession::serveFiles() {
    if (files_ == nullptr) {
        return;
    }

    // Вешать устройство вправе только этот поток: внутри игры монтирование
    // выделяет память её собственным аллокатором, а он у неё потоковый и в
    // чужих потоках попросту отсутствует. Вызов оттуда роняет игру внутри неё
    // самой — на разыменовании нуля в переходнике, достающем аллокатор.
    watch_.mark("mounting the file device");
    const std::size_t mounted = files_->pump();

    // Объявление стримингу — строго следом, и порядок этот обязателен:
    // объявляемый файл игра тут же открывает, чтобы узнать его размер и
    // раскладку страниц, а открыть его она может только через наше устройство.
    //
    // Пока игрок не в мире, объявление идёт крупной порцией: спешить приходится
    // наперегонки с самой игрой — она грузит мир и вправе завести свой файл под
    // тем же именем раньше нас, а на занятое имя отвечает отказом. Подробности
    // у kLoadingBudget.
    watch_.mark("offering files to streaming");
    const std::size_t streamed =
        streamed_ != nullptr ? streamed_->pump(stage_ == Stage::Playing) : 0;

    // Описания — последними, и порядок снова обязателен: описание машины
    // ссылается на её модель по имени, а имя к этому времени должно быть уже
    // объявлено стримингу.
    watch_.mark("loading data descriptions");
    const std::size_t described = described_ != nullptr ? described_->pump() : 0;

    // Описи карты — следом за описаниями, и порядок снова обязателен: опись
    // ссылается на архетипы, а они к этому времени должны быть уже загружены.
    // Без описи расстановка карты доезжает файлом и в мир не встаёт.
    if (manifests_ != nullptr) {
        watch_.mark("reading map manifests");
        (void)manifests_->pump(*files_);
    }

    for (std::string& name : mail_.takeMapPlacements()) {
        placements_.add(std::move(name));
    }

    // Архивы — после описаний и до отчёта: открытый архив тут же обходится, и
    // его содержимое встанет в те же очереди, что и россыпь файлов, только
    // следующим кадром.
    if (archives_ != nullptr) {
        watch_.mark("opening game archives");

        for (const std::string& prefix : archives_->pump(gameFiles_)) {
            declareArchive(prefix);
        }
    }

    // Расстановка просится последней и **только когда очереди опустели**.
    //
    // Это не осторожность, а условие: игра применяет расстановку, опираясь на
    // объявленные ей файлы, архетипы и опись. Попроси её раньше — она не найдёт
    // ничего и промолчит. Проверено вылазкой: просьба ушла на девятнадцать
    // секунд раньше, чем были объявлены файлы, и из четырёхсот двух расстановок
    // встала ровно ноль.
    if (mounted == 0 && streamed == 0 && described == 0) {
        watch_.mark("placing the map");
        (void)placements_.pump();
    }

    // Счёт копится, а строка пишется одна — когда очереди опустеют.
    //
    // Порознь было бы по строке на кадр: чужая карта объявляется порциями, и
    // порций у неё полсотни. Строка эта не для того, кто чинит клиент, а для
    // того, кто держит сервер: ресурс со своими машинами или картой либо доехал
    // до игры, либо нет, и узнать это иначе нечем — не доехавший проявится
    // пустым местом в мире, а пустое место молчит.
    servedFiles_ += streamed;
    servedDescriptions_ += described;

    if (streamed == 0 && described == 0 && (servedFiles_ != 0 || servedDescriptions_ != 0)) {
        // Отказы называются здесь же, и это не украшение строки. Прежде в ней
        // стояли одни принятые файлы, и хозяин сервера, прочитав «Custom
        // content: 3303 files», не имел ни единого повода заподозрить, что
        // десяти моделей из его карты в игре нет вовсе.
        const std::size_t total = streamed_ != nullptr ? streamed_->refused() : 0;
        const std::size_t refused = total - reportedRefusals_;

        reportedRefusals_ = total;

        if (refused == 0) {
            spdlog::info("Custom content: {} files, {} descriptions", servedFiles_,
                         servedDescriptions_);
        } else {
            spdlog::warn("Custom content: {} files, {} descriptions, {} files the game refused",
                         servedFiles_, servedDescriptions_, refused);
        }

        servedFiles_ = 0;
        servedDescriptions_ = 0;
    }

    if (mounted == 0 || filesChecked_) {
        return;
    }

    filesChecked_ = true;

    // Проверка нужна отдельно от самой подмены и вот почему. Устройство — это
    // таблица методов чужой раскладки: сдвинься в ней хоть одна запись, и игра
    // позовёт не ту функцию. Проявится это не строкой в журнале, а вылетом
    // внутри игры тогда, когда она соберётся читать подменённое. Дешевле
    // спросить у неё то же самое сразу и сверить ответ.
    //
    // Спрашиваем именно у игры: её собственный поиск устройства по пути, её
    // открытие, её чтение. Совпадение означает, что весь путь от неё до наших
    // байт пройден целиком.
    const std::string read = gameFiles_.read(game::FileDevice::kProbePath);

    if (read == game::FileDevice::kProbeContents) {
        spdlog::debug("our file device works: the game reads our bytes");
    } else {
        spdlog::error("our file device mounted, but the game read the wrong bytes: {} bytes",
                      read.size());
    }
}

void GameSession::reportPause() {
    const auto measurement = frontend_.measurePause();
    if (!measurement) {
        return;
    }

    if (!measurement->tickRan()) {
        spdlog::debug("меню паузы закрылось: наш тик за это время не пришёл ни разу — "
                      "снимать паузу изнутри тика некому");
        return;
    }

    spdlog::debug("меню паузы закрылось: наших кадров {}, игровое время продвинулось на {} мс — "
                  "мир {}",
                 measurement->frames, measurement->gameTime,
                 measurement->worldFroze() ? "стоял" : "шёл");
}

void GameSession::reportSessionState() {
    if (!sessionState_.ready()) {
        return;
    }

    // Пишется только изменение, а не значение каждый кадр. Признак меняется
    // считаное число раз за запуск, и именно моменты перехода — то, ради чего
    // всё это заведено: по ним видно, дошла ли игра до сессии вообще и когда
    // именно она это решила.
    const bool started = sessionState_.started();
    if (sessionKnown_ && started == sessionStarted_) {
        return;
    }

    sessionKnown_ = true;
    sessionStarted_ = started;

    spdlog::debug("игра считает, что сетевая сессия {}", started ? "начата" : "не начата");
}

void GameSession::publishStage() {
    // Ход входа в мир виден только изнутри игры, и рассказать о нём больше
    // некому. Показывает его меню — своей полосой «входим в игру».
    const shared::LoadStage stage = [this] {
        switch (stage_) {
        case Stage::WaitingForPlayer:
            // Не Booting: до сюда доходит только тот, у кого скриптовый тик уже
            // идёт, а значит движок опознан и скрипты работают. Откатить полосу
            // назад — значит показать игроку, что дело пошло вспять.
            return frontend_.showing() ? shared::LoadStage::Landing : shared::LoadStage::Scripts;
        case Stage::ReplacingModel:
        case Stage::Spawning:
            return shared::LoadStage::Preparing;
        case Stage::Playing:
            return shared::LoadStage::Spawned;
        }
        return shared::LoadStage::Scripts;
    }();

    feed_.setStage(stage);

    // Меню самой игры — то, что выходит по Escape. Свой слой под ним прячется
    // целиком: чат и страницы режима, нарисованные поверх игрового меню, — это
    // второй разговор поверх первого.
    //
    // Спрашивается это только у играющего, и вот почему. Признак у фронтенда
    // игры один на всё: и меню паузы, и страница выбора режима, которую игрок
    // видит на запуске, отвечают одинаково — «меню показано». До входа в мир
    // это означало бы «спрятать свой экран загрузки ровно тогда, когда он и
    // нужен»: под ним в это мгновение стоит чужая страница, ради которой он и
    // рисуется.
    feed_.setGameMenuOpen(stage_ == Stage::Playing && frontend_.showing());
}

void GameSession::suppressGame() {
    // Заставкой и затемнениями распоряжаемся мы: моментом появления игрока
    // тоже, и картинка не имеет права проявиться раньше.
    screen_.hideLoadingScreen();
    screen_.takeOverFades();

    // Затемнения экрана до появления игрока здесь нет, и это пока не вывод, а
    // осторожность.
    //
    // Закрыть эти секунды хочется: в них видны сюжетный персонаж и надпись
    // «задание не выполнено» от нашей же зачистки сюжета. Попытка была — вызов
    // затемнения каждый кадр, — и тот запуск действительно встал на «игрок в
    // мире». Но позже выяснилось, что вставал он по другой причине: модуль в
    // лаунчере Rockstar помнил ключ входа в сетевой режим с прошлого запуска и
    // уводил игру ждать сессию. Затемнение могло быть ни при чём.
    //
    // Поэтому здесь его нет, но и запрета на него в README не записано:
    // наблюдение, у которого нашлось другое объяснение, ничего не доказывает.
    // Проверять придётся заново и по одному.

    story_.clearMissionFlag();

    // Каждый кадр, а не вместе с зачисткой скриптов.
    //
    // Раньше это стояло в зачистке, то есть раз в секунду, и объяснялось тем,
    // что «сюжет успевает попросить снова между зачистками». Объяснение верное,
    // а вывод из него был сделан неполный: успевает — значит убирать надо чаще,
    // а не в том же ритме. Отсюда и «задание не выполнено», висящее на экране
    // сразу после появления: надпись эту заказывает умирающий сюжетный скрипт,
    // и до следующей зачистки она стоит целую секунду.
    //
    // Вызов дешёвый: это шесть нативов, которым нечего делать, когда убирать
    // нечего.
    story_.clearMessages();

    if (story_.cutsceneRunning()) {
        story_.stopCutscene();
    }

    respawn_.suppressGameHandling();
    world_.suppressPopulation();
    world_.suppressWanted(player_.id());

    // И вдогонку — уборка того, что просочилось. Запреты останавливают почти
    // всё, но не всё: машины с водителями заводят и сюжетные скрипты игры,
    // которые продолжают работать. По одной за кадр, вокруг игрока: перебирать
    // весь мир незачем, а лишняя машина, прожившая лишние полсекунды, никому не
    // мешает.
    if (const int ped = player_.ped(); ped != 0) {
        constexpr float kStrayRadius = 200.0F;

        world_.sweepStrayVehicle(player_.coords(ped), kStrayRadius);
    }

    // Счётчик денег игры прячется каждый кадр: он показывает баланс настоящего
    // GTA Online, а тот к нашей сессии отношения не имеет. Свои деньги считает
    // сервер, и показывает их наш интерфейс.
    hud_.hideMoney();

    // Ход времени возвращается каждый кадр. Игра замедляет его сама — на колесе
    // выбора персонажа, при смерти, при аресте, — и в одиночной игре это
    // украшение. В мультиплеере замедлившийся живёт в другом темпе, чем
    // остальные: его снимки приходят к ним вдвое реже, и со стороны он движется
    // рывками, ничего об этом не зная.
    world_.keepTimeFlowing();

    // Читкоды меняют мир только на этом компьютере, и остальные игроки увидят
    // что-то своё. В мультиплеере это не шалость, а расхождение состояний.
    controls_.suppressCheats();

    // Колесо выбора персонажа — сюжетная возможность, и переключать в
    // мультиплеере некого. Оно же — главный источник того самого замедления.
    controls_.suppressCharacterWheel();

    // Меню паузы игре больше не запрещается.
    //
    // Запрет здесь был, и от него пришлось отказаться. Он отбирал у игрока Esc
    // целиком, а взамен давал большую карту — не ту, к которой игрок привык, без
    // настроек, без сохранения, без ничего. Лечение оказалось хуже болезни: мир,
    // встающий на паузу, мешает в мультиплеере, но меню, которого нет, мешает
    // всегда.
    //
    // Настоящее лекарство одно — сетевая сессия: в ней игра не останавливает мир
    // по Esc сама. Ею и занимаемся.
}


void GameSession::advance() {
    switch (stage_) {
    case Stage::WaitingForPlayer: {
        // Пока на экране меню игры, трогать персонажа нельзя: игра уже считает
        // игрока играющим, хотя в мир он ещё не попал, и любое обращение к его
        // персонажу в этот момент роняет её.
        // Пока на экране меню игры, трогать персонажа нельзя, но и нажимать за
        // игрока больше не за чем: страницы выбора режима не существует, её
        // убирает правка в один байт ещё до первого кадра. Всё, что остаётся
        // здесь, — дождаться, пока меню уйдёт.
        if (frontend_.showing()) {
            worldReadyAt_ = Clock::time_point{};
            return;
        }

        if (!player_.playing() || player_.ped() == 0) {
            worldReadyAt_ = Clock::time_point{};
            return;
        }

        // Выдержка после того, как всё сошлось: признаки готовности загораются
        // раньше, чем игра доводит мир и персонажа до рабочего состояния.
        const Clock::time_point now = Clock::now();
        if (worldReadyAt_ == Clock::time_point{}) {
            worldReadyAt_ = now;
            spdlog::debug("игрок в мире, выжидаем");
            return;
        }
        if (now - worldReadyAt_ < kWorldSettle) {
            return;
        }

        stage_ = Stage::ReplacingModel;
        spdlog::debug("меняем модель игрока");
        return;
    }

    case Stage::ReplacingModel: {
        const game::Appearance::Progress progress = appearance_.advance(player_.id());
        if (progress == game::Appearance::Progress::Loading) {
            return;
        }

        ownModel_ = progress == game::Appearance::Progress::Done;
        if (!ownModel_) {
            spdlog::warn("player model was not replaced: the story character stays");
        }

        stage_ = Stage::Spawning;
        return;
    }

    case Stage::Spawning:
        spawn();
        stage_ = Stage::Playing;
        playingSince_ = Clock::now();

        // Отсюда начинается подключение: сетевой поток ждёт именно этой
        // отметки. Экран загрузки при этом остаётся — на нём теперь идёт ход
        // подключения, и уйдёт он по своему признаку, в draw.
        feed_.setWorldReady();
        return;

    case Stage::Playing:
        // Точка появления сервера доходит только теперь: до появления в мире мы
        // к нему ещё не подключались. Перенос идёт раньше всего прочего в этой
        // ветви — картинка проявляется тоже отсюда, и опоздать значило бы
        // показать игроку запасную точку и рывок из неё.
        applyServerSpawn();

        // Подделка сетевого состояния включается только здесь, позже всего
        // остального, и это не осторожность ради осторожности: на загрузке игра
        // распоряжается сессией сама, читает эти признаки чаще всего и ветвится
        // по ним в местах, где объекты сессии обязаны существовать. Раньше
        // этого места вмешательство — самый верный способ упасть.
        if (!networkGame_.holding()) {
            networkGame_.beginHolding(settings_.forceNetworkGame);
        }
        networkGame_.hold();

        // Просьба поднять сессию идёт здесь же и по той же причине, что и
        // подделка: раньше игра распоряжается сетью сама. Повторяется каждый
        // кадр не зря — распорядитель сети может появиться позже игрока, и
        // тогда первые просьбы уйдут в никуда, а эта дождётся.
        if (settings_.hostSession) {
            holdSession();
        }

        // Смена модели, назначенная сервером, доводится здесь же: она занимает
        // десятки кадров — модель нужно загрузить, персонажа пересоздать и дать
        // ему устояться, — и ждать этого внутри одного кадра нельзя.
        if (changingModel_) {
            const game::Appearance::Progress progress = appearance_.advance(player_.id());

            if (progress != game::Appearance::Progress::Loading) {
                changingModel_ = false;

                if (progress == game::Appearance::Progress::Done) {
                    // Одежда надевается после замены, а не до: замена
                    // пересоздаёт персонажа, и надетое до неё осталось бы на
                    // прежнем теле, которого уже нет.
                    look_.apply(player_.ped(), ownLook_);

                    // Объявить себя заново обязательно: остальные видят нас по
                    // тому, что мы им сказали, а сказали мы прежнюю модель.
                    lookPublished_ = false;
                } else {
                    spdlog::warn("the model assigned by the server did not apply");
                }
            }
        }

        // Каждый кадр, а не один раз после смерти. Игра гасит интерфейс сама и
        // не всегда возвращает: на смерти она убирает и радар, и полосы, считая,
        // что дальше её собственный порядок разбора смерти включит их обратно, —
        // а этот порядок мы у неё отобрали. Однократного включения при подъёме
        // оказалось мало.
        screen_.keepInterfaceUp();

        sweepScripts();
        story_.restoreControl();

        ensureTextEntry();
        handleTyping();

        const int player = player_.id();
        const int ped = player_.ped();
        const bool dead = respawn_.dead(player);

        // Мир вокруг игрока держится подгруженным всегда, а не только после
        // переноса: игра вправе выгрузить то, чего он сейчас не касается, — и
        // тогда под ним пропадает земля.
        streaming_.keepCollisionAround(ped);

        // Пока мир вокруг точки переноса не появился, персонажа держат
        // замороженным: отпущенный, он полетит сквозь незагруженную землю.
        //
        // Заморозку просят двое — подгрузка и сервер, — и спорить им нельзя.
        // Отпускает только тот, кто держал последним, и потому решение здесь
        // одно на обоих: держим, пока хоть один просит.
        const bool frozenByServer =
            shared::has(mail_.control(), shared::PlayerControlFlag::Frozen);

        if (streaming_.loading()) {
            player_.freeze(ped, true);

            if (streaming_.advance() && !frozenByServer) {
                player_.freeze(ped, false);
            }
        }

        applyControl(ped, frozenByServer);

        // Посадка разбирается до всего прочего в кадре: задача входа выдаётся
        // игрой в тот же кадр, когда игрок нажал клавишу, и перехватить её
        // нужно раньше, чем она успеет начать выдёргивать чужого водителя.
        vehicleEntry_.sync(ped);

        applyServerEvents(ped);
        runScripts();
        applyIncomingDamage(ped);
        applyServerState(ped);
        handleDeath(player);
        publishLocalState(player, ped, dead);
        publishAppearance(ped);
        showRemotePlayers(ped);
        return;
    }
}

void GameSession::ensureTextEntry() {
    if (textEntry_ != nullptr || textEntryFailed_) {
        return;
    }

    const HWND window = game::Window::findOwnWindow();
    if (window == nullptr) {
        // Окна ещё нет. Это не беда: до сюда мы доходим уже в мире, но окно
        // игра пересоздаёт на переходах, и пропустить его на кадр — обычное
        // дело.
        return;
    }

    std::string error;

    textEntry_ = game::TextEntry::install(window, error);
    if (textEntry_ == nullptr) {
        // Один раз: без перехвата не будет чата, и повторять попытку каждый
        // кадр значит записать в журнал одну и ту же строку тысячу раз.
        textEntryFailed_ = true;
        spdlog::error("keyboard hook not installed, chat will not work: {}", error);
    }
}

void GameSession::handleTyping() {
    // Пока открыто меню, игра не видит ни одного нажатия и ни одного щелчка.
    //
    // Съеденных сообщений окна для этого мало, и это выяснено на живой игре:
    // мышь и клавиатуру GTA читает напрямую, мимо очереди. Снаружи беда
    // выглядела так: игрок в мире открывает меню по F1, тычет в кнопку — и
    // персонаж под меню бьёт кулаком по воздуху.
    //
    // Раньше всего остального: пока меню открыто, ни чат, ни консоль клавиш
    // не разбирают — их разбирает страница.
    if (feed_.menuOpen()) {
        controls_.suppressEverything();

        // Одна запись за запуск. Проверить это глазами можно только одним
        // способом — открыть меню в игре и тыкать в кнопки, глядя, не машет ли
        // персонаж кулаками, — и без метки «не машет» одинаково означало бы
        // «запрет работает» и «запрет не дошёл сюда вовсе».
        if (!menuSuppressed_) {
            menuSuppressed_ = true;
            spdlog::debug("menu is open: input taken away from the game");
        }

        return;
    }

    if (textEntry_ == nullptr) {
        return;
    }

    // Порядок здесь обязателен: сперва итог прошлого набора, и только потом
    // начало нового. Иначе клавиша, которой набор закончили, в том же кадре
    // открыла бы его заново — и стёрла бы то, что успели набрать.
    if (!textEntry_->active()) {
        std::string typed;
        const game::TextEntry::Outcome outcome = textEntry_->takeOutcome(typed);

        if (outcome != game::TextEntry::Outcome::Typing) {
            if (outcome == game::TextEntry::Outcome::Submitted) {
                mail_.postChat(std::move(typed));
            }

            feed_.setInput(false, {});
        }
    }

    // Строку просит один только чат: больше её просить в клиенте некому.
    if (!textEntry_->active() && pressedOnce(kChatKey)) {
        textEntry_->begin();
    }

    if (textEntry_->active()) {
        // Пока набирается текст, игра не должна видеть ни одного нажатия.
        // Съеденных сообщений окна для этого мало: клавиатуру GTA читает
        // напрямую, мимо них.
        controls_.suppressEverything();
        feed_.setInput(true, textEntry_->text());
    }
}

void GameSession::forgetSessionOnLeaving() {
    const bool here = status_.snapshot().inSession();

    if (here == inSession_) {
        return;
    }

    inSession_ = here;

    if (here) {
        return;
    }

    // Следующий сервер назовёт свою точку появления, и перенести туда нужно
    // будет заново.
    serverSpawnApplied_ = false;

    // Метки, маркеры и точки — единственное из мира сессии, что не убирается
    // само собой. Люди и машины пропадают, потому что сверяются со списком, а
    // опустевший список убирает всех; нарисованное же приходит событиями, и
    // события эти после разрыва не придут никогда.
    //
    // Стоило это карты прежнего сервера поверх карты следующего.
    blips_.clear();
    markers_.clear();
    checkpoints_.clear();

    // Привязки — по той же причине: они тоже состояние, приходящее событием.
    // Отвязывать при этом нечего: тела, к которым они вели, к этому мгновению
    // уже разобраны вместе с сессией.
    attachments_.clear();

    // Прохожие — тоже. Они приходят событиями, и событий этих после разрыва не
    // будет никогда: куклы прежнего сервера остались бы стоять в мире
    // следующего.
    peds_.clear();

    // И память о том, как выглядят чужие игроки. Сами куклы убираются сами —
    // список игроков опустел, а он им и мерило, — но одежда, модель и сборка
    // оружия помнятся отдельно и переживают разрыв. Ключ у этой памяти — номер
    // игрока, а номера выдаёт сервер: на следующем сервере под номером три
    // будет уже другой человек, и до своего объявления внешности он вышел бы в
    // мир одетым в чужое и с чужим телом.
    remotePlayers_.clear();

    // Клиентские половины ресурсов останавливаются здесь же, и это не уборка
    // ради опрятности. Движок отказывается поднимать ресурс, чьё имя уже
    // занято, — а занято оно тем, что подняли в прошлой сессии. Оставленные
    // жить, они не давали подняться ни одному ресурсу после переподключения, и
    // выглядело это как «сервер перезапустился, и режим больше не работает».
    //
    // Вместе с ресурсом уходят и его окна: их убирает он сам, у себя.
    if (scripts_ != nullptr) {
        // Разрыв объявляется до остановки, а не после: обработчик застаёт свой
        // ресурс ещё живым — с окнами, таймерами и подписками. Объяви мы это
        // после, сохранять и убирать за собой ресурсу было бы уже нечем. То же
        // правило и у серверного `playerDisconnect`.
        if (connectionAnnounced_) {
            scripts_->sessionEvent("disconnect");
        }

        for (const std::string& name : startedResources_) {
            scripts_->stopResource(name);
            spdlog::info("Resource \"{}\" stopped: the session ended", name);
        }
    }

    connectionAnnounced_ = false;
    startedResources_.clear();
}

/// Сколько ждать движение, прежде чем забыть его.
///
/// Столько же, сколько PedAnimation ждёт набор движений: дольше ждать нечего —
/// набор, не загрузившийся за это время, не загрузится вовсе, а движение,
/// сыгранное минутой позже, уже не то движение.
constexpr auto kAnimationPatience = std::chrono::seconds{5};

void GameSession::applyAnimations(int ped) {
    for (shared::PlayerAnimation& animation : mail_.takeAnimations()) {
        pendingAnimations_.push_back(
            PendingAnimation{.animation = std::move(animation), .since = Clock::now()});
    }

    if (pendingAnimations_.empty()) {
        return;
    }

    const shared::PlayerId self = status_.snapshot().playerId;
    const Clock::time_point now = Clock::now();

    std::erase_if(pendingAnimations_, [&](const PendingAnimation& pending) {
        // Своё движение играет собственный персонаж, чужое — кукла, и дороги у
        // них разные не для порядка.
        //
        // Своим персонажем не распоряжается никто, кроме игры и нас: выданное
        // ему движение идёт, пока не кончится. Куклу же мы ведём сами — задачей
        // ходьбы, прицелом, поворотом, — и всё это отменило бы движение в тот же
        // кадр. Поэтому чужое движение заводится через RemotePlayers: тот, кто
        // выдаёт задачи, обязан знать, что сейчас идёт движение сервера.
        const bool own =
            pending.animation.playerId == self && self != shared::kInvalidPlayerId;

        // Пустой набор означает «снять задачи»: у alt:V это отдельный вызов
        // clearTasks, а по сети — то же самое распоряжение.
        const bool clearing = pending.animation.dictionary.empty();

        if (own) {
            if (ped != 0) {
                if (clearing) {
                    pedAnimation_.clearTasks(ped);
                    return true;
                }

                if (pedAnimation_.playNamed(ped, pending.animation)) {
                    return true;
                }
            }
        } else if (clearing) {
            if (remotePlayers_.stopAnimating(pending.animation.playerId)) {
                return true;
            }
        } else if (remotePlayers_.animate(pending.animation.playerId, pending.animation)) {
            return true;
        }

        return now - pending.since >= kAnimationPatience;
    });
}

void GameSession::applyAttachments(int ped) {
    const shared::PlayerId self = status_.snapshot().playerId;

    // Тела ищутся по роду и номеру. Своё — отдельной веткой: наш персонаж живёт
    // не в списке кукол, а в самой игре, и RemotePlayers его не касается вовсе.
    attachments_.sync([&](shared::EntityKind kind, std::uint32_t id) -> int {
        switch (kind) {
        case shared::EntityKind::Player:
            return id == self && self != shared::kInvalidPlayerId
                       ? ped
                       : remotePlayers_.handleFor(id);
        case shared::EntityKind::Vehicle:
            return vehicles_.handleFor(id);
        case shared::EntityKind::Object:
            return objects_.handleFor(id);
        case shared::EntityKind::Ped:
            return peds_.handleFor(id);
        case shared::EntityKind::None:
            break;
        }

        return 0;
    });
}

void GameSession::applyServerEvents(int ped) {
    forgetSessionOnLeaving();
    applyAnimations(ped);

    // Взрывы — сразу и без откладывания, в отличие от движений: у движения есть
    // цель, которой может ещё не быть в игре, а взрыв случается на пустом месте.
    for (const shared::Explosion& explosion : mail_.takeExplosions()) {
        explosions_.play(explosion);
    }

    // Чужие выстрелы — тем же порядком. Куклы, из которой стрелять, может ещё
    // не быть: выстрел идёт надёжным каналом и обгоняет снимки. Тогда он просто
    // пропадёт, и это верно — показывать его негде.
    for (const shared::WeaponFired& fired : mail_.takeShots()) {
        remotePlayers_.fire(fired.playerId, fired.weapon, fired.target);
    }

    // Как у чужих собрано оружие. Запоминается, а не применяется сразу: насадки
    // ставятся на ствол, который уже в руках, а выдаётся он кукле по снимку.
    for (const shared::PlayerWeapon& look : mail_.takeWeaponLooks()) {
        remotePlayers_.arm(look.playerId, look);
    }

    // Перенос — единственное распоряжение сервера, которое исполняет игра:
    // персонаж живёт здесь, и переставить его больше некому.
    if (ped != 0) {
        for (const shared::Vec3& destination : mail_.takeTeleports()) {
            player_.teleport(ped, destination);
            spdlog::debug("the server teleported us");
        }
    }

    // Посадка в машину: сервер велел, исполняет игра. Только если персонаж
    // уже есть — до появления сажать некого, а распоряжение до тех пор ждёт в
    // почте.
    if (ped != 0) {
        for (const shared::PlayerIntoVehicle& command : mail_.takeSeats()) {
            if (!vehicles_.seat(ped, command.vehicle, command.seat)) {
                // Машины здесь нет — обычное дело: она могла не дойти до нас
                // вовсе либо уехать за горизонт.
                spdlog::debug("сажать некуда: машины {} здесь нет", command.vehicle);
            }
        }
    }

    // Метки на карте — тем же порядком: сервер их назначает, рисует игра.
    // Убранные раньше назначенных: метка, снятая и поставленная в одном такте,
    // должна остаться поставленной.
    for (const shared::BlipId id : mail_.takeRemovedBlips()) {
        blips_.remove(id);
    }

    for (const shared::BlipState& blip : mail_.takeBlips()) {
        blips_.apply(blip);
    }

    // Маркеры и контрольные точки — тем же порядком и по той же причине:
    // убранные раньше назначенных.
    for (const shared::MarkerId id : mail_.takeRemovedMarkers()) {
        markers_.remove(id);
    }

    for (const shared::MarkerState& marker : mail_.takeMarkers()) {
        markers_.apply(marker);
    }

    for (const shared::CheckpointId id : mail_.takeRemovedCheckpoints()) {
        checkpoints_.remove(id);
    }

    for (const shared::CheckpointState& checkpoint : mail_.takeCheckpoints()) {
        checkpoints_.apply(checkpoint);
    }

    // Прохожие: убранные раньше объявленных, по той же причине, что и у меток.
    // Кукла, снятая и поставленная в одном такте, должна остаться поставленной.
    for (const shared::PedId id : mail_.takeRemovedPeds()) {
        peds_.remove(id);
    }

    for (const shared::PedState& state : mail_.takePeds()) {
        peds_.apply(state);
    }

    peds_.sync();

    // Привязки: запоминаются здесь, накладываются ниже. Разделено потому, что
    // накладывать их приходится не тогда, когда о них сказали: тел может не быть
    // ни одного — машина ещё не доехала, модель предмета грузится.
    for (const shared::EntityAttachment& attachment : mail_.takeAttachments()) {
        attachments_.apply(attachment);
    }

    applyAttachments(ped);

    // Распоряжения о машинах исполняет тоже игра, и тоже потому, что больше
    // некому: машина живёт здесь, у своего ведущего. Забирать их из почты нужно
    // безусловно — даже если машины у нас уже нет: иначе они копились бы там до
    // конца сессии.
    for (const shared::VehicleTeleport& command : mail_.takeVehicleTeleports()) {
        if (!vehicles_.place(command.id, command.position, command.heading)) {
            // Машины здесь нет — обычное дело: ведущего у неё могли сменить
            // между отправкой распоряжения и его приходом.
            spdlog::debug("переставить машину {} нечем: её здесь нет", command.id);
        }
    }

    for (const shared::VehicleRepair& command : mail_.takeVehicleRepairs()) {
        if (!vehicles_.repair(command.id)) {
            spdlog::debug("починить машину {} нечем: её здесь нет", command.id);
        }
    }

    // Клавиши уходят туда же и раньше событий: ресурс, открывающий по клавише
    // окно, должен успеть это сделать прежде, чем сервер что-нибудь ему в это
    // окно пришлёт. Забирать их из почты нужно в любом случае — даже когда
    // машины нет: иначе они копились бы там до конца сессии.
    for (const SessionMail::KeyEvent& key : mail_.takeKeys()) {
        if (scripts_ != nullptr) {
            scripts_->key(key.code, key.down);
        }
    }

    // Именованные события уходят клиентским половинам ресурсов.
    //
    // Толковать их здесь не будет никто и никогда. Что значит имя и что значит
    // нагрузка, знает написавший ресурс; клиент только доставляет. Забирать их
    // из почты нужно в любом случае — даже когда машины нет: иначе они
    // копились бы там до конца сессии.
    for (const shared::ServerEvent& event : mail_.takeIncomingEvents()) {
        if (scripts_ != nullptr) {
            scripts_->serverEvent(event.name, event.payload);
        }
    }
}

namespace {

/// Приставка, которой alt:V помечает файлы своего ресурса.
constexpr std::string_view kResourceScheme = "http://resource/";

/// Превращает адрес ресурса в тот, который поймёт схема клиента.
///
/// Ресурс пишет `http://resource/client/ui/index.html`, подразумевая **свой**
/// каталог: у alt:V имя узла означает «тот ресурс, который завёл это окно».
/// Схема же клиента отдаёт весь кеш разом, и без имени ресурса два режима с
/// одинаково названными страницами показали бы друг другу чужое.
///
/// Поэтому имя вставляется здесь: `http://resource/main/client/ui/index.html`.
/// Ресурс об этом не знает и знать не должен — он писался под alt:V.
[[nodiscard]] std::string resolveViewUrl(std::string_view resource, std::string_view url) {
    if (!url.starts_with(kResourceScheme)) {
        // Обычная ссылка — наружу или на данные. Отдаётся как есть: запрещать
        // режиму открыть свою страницу в сети незачем, он и так пришёл оттуда.
        return std::string{url};
    }

    std::string resolved{kResourceScheme};
    resolved += resource;
    resolved += '/';
    resolved += url.substr(kResourceScheme.size());

    return resolved;
}

} // namespace

void GameSession::runScripts() {
    // Машина поднимается один раз и по требованию: до появления ресурсов она не
    // нужна, а сто сорок мегабайт движка при входе в сессию стоят заметного
    // времени там, где игра и так занята загрузкой.
    std::vector<SessionMail::ClientResource> waiting = mail_.takeClientResources();

    if (!waiting.empty() && scripts_ == nullptr && !scriptsTried_) {
        scriptsTried_ = true;

        ScriptHost::Hooks hooks;

        hooks.natives = &natives_;

        hooks.emitServer = [this](std::string_view name, std::string_view payload) {
            mail_.postEvent(std::string{name}, std::string{payload});
        };

        hooks.localPlayerId = [this] {
            const shared::PlayerId id = status_.snapshot().playerId;

            // −1, а не наш недействительный номер: у alt:V «нас ещё нет»
            // выражается именно так, и ресурсы сравнивают с ним.
            return id == shared::kInvalidPlayerId ? -1 : static_cast<std::int32_t>(id);
        };

        // Переводчик между номерами сессии и дескрипторами игры.
        //
        // Всё, что ему нужно, у клиента уже есть и всегда было: реестр сессии
        // знает, кто в ней, а RemotePlayers и Vehicles знают, каким телом каждый
        // из них показан здесь. Недоставало не сведений, а окошка к ним — и
        // потому здесь нет ни нового сообщения протокола, ни нового поля в
        // состоянии. Связь эта у каждого игрока своя, и сервер её не знает.
        //
        // Зовётся всё это из такта скриптовой машины, то есть из потока игры и
        // изнутри обработчика скрипта. Другого потока здесь не бывает: движок
        // крутится только в tick(), а tick() зовётся отсюда же.
        hooks.entities.players = [this] {
            std::vector<ScriptHost::Hooks::Entity> found;

            // Себя — первым и всегда, даже до того, как в мире появится тело:
            // ресурс, обходящий alt.Player.all первыми же строками, обязан найти
            // там себя. Тело в этот миг может быть ещё нулевым, и это правда, а
            // не пробел.
            const shared::PlayerId self = status_.snapshot().playerId;

            if (self != shared::kInvalidPlayerId) {
                found.push_back(ScriptHost::Hooks::Entity{
                    .id = static_cast<std::int32_t>(self),
                    .handle = player_.ped(),
                });
            }

            // Все, о ком сказал сервер, — а не только те, кому уже нашлось тело.
            // Так же поступает и alt:V: у него `Player.all` — это все игроки
            // сессии, а `streamedIn` — те, кто рядом. Игрок без тела всё равно
            // нужен: у него есть имя и метаданные, и по ним рисуют список.
            for (const RemoteView& player : roster_.snapshot()) {
                found.push_back(ScriptHost::Hooks::Entity{
                    .id = static_cast<std::int32_t>(player.id),
                    .handle = remotePlayers_.handleFor(player.id),
                });
            }

            return found;
        };

        hooks.entities.vehicles = [this] {
            std::vector<ScriptHost::Hooks::Entity> found;

            for (const SessionVehicleView& vehicle : roster_.vehicles()) {
                found.push_back(ScriptHost::Hooks::Entity{
                    .id = static_cast<std::int32_t>(vehicle.state.id),
                    .handle = vehicles_.handleFor(vehicle.state.id),
                });
            }

            return found;
        };

        hooks.entities.pedOf = [this](std::int32_t id) {
            const shared::PlayerId self = status_.snapshot().playerId;

            // Своё тело спрашивается у игры, а не ищется среди болванчиков: нас
            // среди них нет и быть не может. К тому же игра выдаёт своему
            // персонажу новый дескриптор после каждой смерти, и запомненный
            // однажды устарел бы в первом же бою.
            if (self != shared::kInvalidPlayerId && id == static_cast<std::int32_t>(self)) {
                return player_.ped();
            }

            return remotePlayers_.handleFor(static_cast<shared::PlayerId>(id));
        };

        hooks.entities.playerAt = [this](std::int32_t ped) -> std::int32_t {
            if (ped == 0) {
                return -1;
            }

            const shared::PlayerId self = status_.snapshot().playerId;

            if (self != shared::kInvalidPlayerId && ped == player_.ped()) {
                return static_cast<std::int32_t>(self);
            }

            const shared::PlayerId owner = remotePlayers_.ownerOf(ped);

            return owner == shared::kInvalidPlayerId ? -1 : static_cast<std::int32_t>(owner);
        };

        hooks.entities.carOf = [this](std::int32_t id) {
            return vehicles_.handleFor(static_cast<shared::VehicleId>(id));
        };

        hooks.entities.vehicleAt = [this](std::int32_t car) -> std::int32_t {
            const shared::VehicleId id = vehicles_.idOf(car);

            return id == shared::kInvalidVehicleId ? -1 : static_cast<std::int32_t>(id);
        };

        hooks.entities.nameOf = [this](std::int32_t id) -> std::string {
            const shared::PlayerId self = status_.snapshot().playerId;

            if (self != shared::kInvalidPlayerId && id == static_cast<std::int32_t>(self)) {
                return settings_.nickname;
            }

            for (const RemoteView& player : roster_.snapshot()) {
                if (static_cast<std::int32_t>(player.id) == id) {
                    return player.nickname;
                }
            }

            return {};
        };

        // Файлы ресурса читаются из свёртка, а не с диска: своего файла на
        // диске у них нет вовсе. Обработчик ставит сеть — она эти свёртки и
        // скачала.
        hooks.readResourceFile = mail_.resourceReader();

        if (!hooks.readResourceFile) {
            spdlog::warn("no resource reader: client resources will not run");
        }

        // Мостик к слою интерфейса берётся один раз: слой живёт до конца
        // процесса, и спрашивать о нём заново на каждое окно незачем.
        const SessionMail::ViewBridge views = mail_.viewBridge();

        if (views.create) {
            hooks.createView = [views](std::string_view resource, std::string_view url) {
                return views.create(resolveViewUrl(resource, url));
            };

            hooks.destroyView = views.destroy;

            hooks.emitView = [views](std::uint32_t view, std::string_view name,
                                     std::string_view payload) {
                views.emit(view, std::string{name}, std::string{payload});
            };

            hooks.showView = views.show;
            hooks.focusView = views.focus;
        } else {
            spdlog::warn("no interface layer: resource windows will not open");
        }

        std::string error;
        scripts_ = ScriptHost::load(game::clientDirectory(), std::move(hooks), error);

        if (scripts_ == nullptr) {
            spdlog::warn("the client script engine did not start: {}", error);
            spdlog::warn("server resources will not run; everything else works as usual");
        }
    }

    if (scripts_ == nullptr) {
        return;
    }

    for (const SessionMail::ClientResource& resource : waiting) {
        if (scripts_->startResource(resource.name, resource.root, resource.entry)) {
            spdlog::info("Resource \"{}\" started", resource.name);
            startedResources_.push_back(resource.name);
        } else {
            spdlog::error("Resource \"{}\" failed to start", resource.name);
        }
    }

    // Сказать серверу, что мы готовы, нужно и когда ни один ресурс не поднялся:
    // иначе сервер ждал бы нас вечно, и режим не получил бы события входа вовсе.
    if (!waiting.empty()) {
        mail_.postEvent(std::string{shared::kClientReadyEvent}, {});

        // Тем же мгновением ресурсам объявляется вход.
        //
        // **Событие это не объявлялось ни разу, хотя дорога для него была
        // построена.** `ScriptHost::sessionEvent` существовал и не звался
        // ниоткуда; у alt:V `connectionComplete` — то, с чего начинается вся
        // клиентская половина режима, и у проверенного режима на него подписаны
        // три места. Не получив его, он не показывал ни интерфейса, ни курсора
        // — и понять по такому зрелищу, что не пришло одно событие, было нельзя.
        //
        // Объявляется здесь, а не раньше, и это существенно: раньше ресурсов
        // нет вовсе, и объявлять было бы некому. Один раз за сессию — второй
        // заход в этот блок случается, только когда сервер прислал ещё ресурсы,
        // а входим мы по-прежнему однажды.
        if (!connectionAnnounced_) {
            connectionAnnounced_ = true;
            scripts_->sessionEvent("connectionComplete");
        }
    }

    // Событие от страницы уходит тому ресурсу, который эту страницу завёл.
    for (const SessionMail::ViewEvent& event : mail_.takeViewEvents()) {
        scripts_->viewEvent(event.view, event.name, event.arguments);
    }

    // Прокрутка раз в кадр: без неё не сработает ни один таймер и не разрешится
    // ни одно обещание.
    scripts_->tick();

    // И следом — кадровое событие ресурсам.
    //
    // **Его не объявлял никто, и это стоило всей кадровой работы клиентского
    // слоя разом.** На нём висят три вещи: `alt.everyTick` — то, чем всякий
    // клиентский ресурс рисует поверх игры; `alt.drawFrame` — маркеры,
    // чекпоинты, указатель и запрет управления, которые слой рисует сам; и
    // сверка появившихся тел, из которой берутся `worldObjectStreamIn` и
    // `gameEntityCreate`. Не приходя ни разу, оно оставляло всё это мёртвым —
    // молча, без единой строки в журнале.
    //
    // После прокрутки, а не до, и это то же правило, что и у такта сервера:
    // обработчик, поставивший `setTimeout(fn, 0)`, увидит его сработавшим
    // следующим кадром, а не через два.
    //
    // Звать можно только отсюда: обработчики зовут нативы, а нативы живут
    // ровно в этом кадре игры.
    scripts_->sessionEvent("render");
}

void GameSession::applyControl(int ped, bool frozenByServer) {
    if (ped == 0) {
        return;
    }

    // Накладывается каждый кадр, а не при изменении, и это не расточительство.
    // `RESURRECT_PED` сбрасывает признаки персонажа: наложенные однажды, они
    // пропали бы у поднятого игрока — и вернуть их было бы нечем, потому что
    // прошлое распоряжение сервера с новым совпадает и «изменением» не будет.
    // То же правило, что и у запрета падать у кукол.
    //
    // Стоит это двух нативов на кадр, и оба дешёвые: они пишут признак в
    // структуру персонажа, а не считают ничего.
    player_.freeze(ped, frozenByServer || streaming_.loading());

    player_.setInvincible(
        ped, shared::has(mail_.control(), shared::PlayerControlFlag::Invincible));
}

void GameSession::applyIncomingDamage(int ped) {
    if (ped == 0) {
        return;
    }

    for (const shared::DamageTaken& taken : mail_.takeIncomingDamage()) {
        // Здоровье отсюда больше не отнимается, и это главная перемена. Сколько
        // снять, решил сервер, и он же уже снял — его число придёт отдельным
        // сообщением. Отними мы урон ещё и здесь, попадание стоило бы вдвое
        // дороже, чем стоит.
        //
        // Само сообщение при этом никуда не делось и делось быть не могло: из
        // него видно, кто попал и из чего, а из числа здоровья — не видно.
        spdlog::debug("player {} hit us for {}", taken.attacker, taken.amount);
    }
}

void GameSession::teleportSafely(int ped, shared::Vec3 destination, float heading) {
    if (ped == 0) {
        return;
    }

    player_.teleport(ped, destination);
    player_.setHeading(ped, heading);

    // Подгрузка начинается после переноса, а не до: сфера строится вокруг точки,
    // где игрок уже стоит, и игра подгружает её в первую очередь.
    streaming_.beginLoad(destination);
}

void GameSession::holdSession() {
    // Держим ровно пока есть что держать, и это правка по журналу.
    //
    // Держать начинаем до первой просьбы — игра выходит из сессии сама, и первый
    // же её выход случается раньше, чем мы успеем спросить, поднялась ли она. Но
    // и отпускать надо: сессия бывает, что поднимается и ложится совсем, и
    // попытки кончаются. Дальше запертая дверь не держит уже ничего.
    //
    // А стоит она дорого. За дверью — не «выход из сессии», а безымянная
    // функция игры, которую зовут каждый кадр: доводами ей идут счётчики, а не
    // причины, и что она делает на самом деле, неизвестно (это записано и в
    // каталоге сигнатур). Запертая навсегда, она не исполняется шестьдесят с
    // лишним тысяч раз подряд — и примерно на этом счёте игра умирает.
    //
    // Счёт сходится по журналам, и сходится жёстко. Восемь запусков, доживших
    // до этого мгновения: от подъёма сессии до краха 1053, 1054, 1072, 1077,
    // 1083, 1098 секунд — то есть восемнадцать с половиной минут с разбросом в
    // полминуты. Ни один запуск короче не упал, ни один длиннее не выжил. От
    // чужих игроков, от карты сервера и от custom-контента это не зависело
    // вовсе: половина этих сессий была пустой и голой. Падало при этом сразу
    // несколько потоков игры в одну миллисекунду, и один из них — звуковой;
    // отсюда и «страшные громкие звуки».
    //
    // Поэтому: сессия жива — держим, сессии больше не будет — отпускаем.
    if (networkBail_ != nullptr) {
        networkBail_->hold(sessionState_.started() || !netSession_.gaveUp());
    }

    netSession_.host(settings_.sessionMode);

    // Повторная просьба, если сессия всё-таки легла. Раньше она была главным
    // средством и работала против себя: сессию просили поднять каждый кадр, пока
    // та поднималась, и от этого она и разваливалась. С выдержкой она стала тем,
    // чем должна быть, — запасным выходом, который почти никогда не нужен.
    netSession_.rehostIfDropped(settings_.sessionMode, sessionState_.started());
}

void GameSession::spawn() {
    const int ped = player_.ped();
    if (ped == 0) {
        return;
    }

    // Персонаж достаётся нам из сюжетной сцены замороженным и с её заданиями.
    // Пока это не снято, он стоит на месте, что бы игрок ни нажимал.
    player_.release(ped);

    const shared::Vec3 point = spawnPoint();

    teleportSafely(ped, point, kSpawnHeading);

    // Запрет населения касается только новых прохожих и машин: созданные до
    // него остаются на местах, и убрать их нужно отдельно.
    world_.clearArea(point, kClearRadius);

    spdlog::debug("spawned at the server point: {:.1f} {:.1f} {:.1f}", point.x, point.y, point.z);
}

void GameSession::applyServerSpawn() {
    if (serverSpawnApplied_) {
        return;
    }

    const std::optional<shared::Vec3> named = status_.snapshot().spawn;
    if (!named) {
        return;
    }

    const int ped = player_.ped();
    if (ped == 0) {
        return;
    }

    serverSpawnApplied_ = true;

    // Совпало с запасной — переносить некуда, и лишний перенос стоил бы
    // подгрузки мира заново на пустом месте.
    if (*named == kFallbackSpawn) {
        return;
    }

    teleportSafely(ped, *named, kSpawnHeading);
    world_.clearArea(*named, kClearRadius);

    spdlog::debug("moved to the spawn point named by the server: {:.1f} {:.1f} {:.1f}", named->x,
                  named->y, named->z);
}

void GameSession::declareArchive(const std::string& prefix) {
    watch_.mark("walking a game archive");

    const std::vector<std::string> inside = gameFiles_.filesUnder(prefix);

    if (inside.empty()) {
        // Пустой архив — почти наверняка не пустой, а неоткрытый: игра ответила
        // согласием, но обход ничего не нашёл. Молчать нельзя, иначе это
        // выглядит как «архив приехал и ничего не привёз».
        spdlog::warn("archive at {} turned out empty", prefix);
        return;
    }

    std::size_t models = 0;
    std::size_t archetypes = 0;
    std::size_t nested = 0;

    for (const std::string& path : inside) {
        const std::string_view name = game::baseName(path);

        // Вложенный архив вешается своей приставкой и обходится следующим
        // кадром: у чужих сборок машина внутри карты внутри архива — обычное
        // дело. Приставка выводится из его пути, а путь внутри архива
        // неповторим.
        if (game::isArchive(name)) {
            ++nested;
            continue;
        }

        if (!game::worthStreaming(name)) {
            continue;
        }

        if (streamed_ != nullptr) {
            streamed_->add(path, std::string{name});
            ++models;
        }

        if (described_ != nullptr && game::isArchetypeList(name)) {
            described_->add(path, std::string{name}, game::kArchetypeType);
            ++archetypes;
        }
    }

    // Строка для хозяина сервера, а не для того, кто чинит клиент: архив либо
    // привёз игре добро, либо нет, и узнать это иначе нечем.
    spdlog::info("Archive {} brought {} files and {} archetype lists", prefix, models, archetypes);

    if (nested != 0) {
        // Вложенные архивы пока не открываются, и об этом говорится вслух:
        // молча пропущенная половина чужой сборки — это ровно тот случай, когда
        // человек ищет причину часами. Открывать их — отдельная работа: игре
        // нужен путь на диске, а вложенный архив лежит внутри другого.
        spdlog::warn("archive {} carries {} nested archives; those are not opened yet", prefix,
                     nested);
    }
}

void GameSession::sweepScripts() {
    // Два условия, и оба обязательны.
    //
    // Своя модель — потому что сюжетный персонаж принадлежит сюжетным скриптам
    // и уходит вместе с ними.
    //
    // Обычная игра — потому что до неё завершать нечего, а вред есть: на старте
    // игра сама распоряжается своими скриптами, и вмешательство в этот момент
    // роняет её по 0xC0000005. Проверено дважды, оба раза через несколько
    // секунд после запуска стартовых скриптов.
    if (!ownModel_) {
        return;
    }

    const Clock::time_point now = Clock::now();

    // Первое время — каждый кадр. Сюжет в эти секунды ещё сопротивляется:
    // погашенный пускатель миссий заводится заново, и за секунду между
    // зачистками миссия успевает начаться и показать своё название. Дальше
    // такой частоты не нужно, а тратить на неё кадр постоянно незачем.
    const bool settling = now - playingSince_ < kEagerSweepWindow;

    if (!settling && sweptAt_ != Clock::time_point{} && now - sweptAt_ < kSweepInterval) {
        return;
    }
    sweptAt_ = now;

    ++sweeps_;

    // Отчёт время от времени, а не однажды.
    //
    // Раньше здесь была одна строка на весь запуск, и по журналу нельзя было
    // отличить работающее подавление от давно отвалившегося: и там и там
    // молчание. А вопрос «сюжет вернулся — а гасим ли мы его ещё» возникает
    // ровно тогда, когда ответ нужен немедленно.
    if (!sweepLogged_ || now - sweepReportedAt_ >= kSweepReportInterval) {
        sweepLogged_ = true;
        sweepReportedAt_ = now;

        spdlog::debug("подавление работает: зачисток {}, гасим сюжет и телефон{}", sweeps_,
                     settings_.hostSession ? "; сетевые оставлены — они ведут сессию"
                                           : " и GTA Online");
    }

    story_.terminateStory();

    // Сетевые скрипты гасятся, только пока мы не поднимаем свою сессию.
    //
    // Их гашение заводилось по делу: изменённому клиенту в GTA Online делать
    // нечего. Но с поднятием сессии довод переворачивается — сессию ведут ровно
    // эти скрипты, и maintransition прежде всего. Погасив их, мы убиваем то, что
    // сами же попросили начать.
    //
    // Так и вышло на первой пробе, и это видно по журналу поминутно: сессия
    // поднялась, игра выставила оба признака и ответила «начата», а через 336
    // миллисекунд всё легло обратно. Между этими двумя записями стояла наша
    // зачистка, идущая каждый кадр первые двадцать секунд. Со стороны это
    // выглядело как улетевшая висеть над городом камера — вступительная камера
    // сетевой сессии, оставшаяся без скрипта, который её вёл.
    if (!settings_.hostSession) {
        story_.terminateOnline();
    }

    story_.terminatePhone();

}

void GameSession::handleDeath(int player) {
    const bool isDead = respawn_.dead(player);
    if (!respawn_.due(isDead)) {
        return;
    }

    const shared::Vec3 point = spawnPoint();

    respawn_.resurrect(point, kSpawnHeading, player);
    streaming_.beginLoad(point);
    world_.clearArea(point, kClearRadius);

    player_.release(player_.ped());

    // Игра гасит экран и убирает интерфейс на смерти, а возвращать их положено
    // тому порядку разбора смерти, который мы у неё отобрали.
    screen_.fadeIn(kFadeIn);
    screen_.keepInterfaceUp();

    spdlog::debug("player raised at the spawn point");
}

void GameSession::publishLocalState(int player, int ped, bool dead) {
    if (ped == 0) {
        return;
    }

    // Отсюда положение уходит на сервер, а с него — остальным игрокам. Без
    // этого мы для них не существуем: сервер рассылает лишь то, что ему
    // прислали.
    shared::PlayerState state = player_.snapshot(player, ped, dead);

    // Машина, до которой нам есть дело: та, в которой сидим, а если ни в какой,
    // то та, в которую лезем. Второе объявляется наравне с первым и не для
    // красоты: остальные по этому признаку показывают вход целиком — как
    // персонаж подходит к двери, открывает её и садится, — а показывать его
    // некуда, пока машины у них нет.
    int handle = 0;

    if (const auto seat = vehicles_.seatOf(ped)) {
        state.flags |= static_cast<std::uint32_t>(shared::PlayerFlag::InVehicle);
        state.seat = seat->index;

        handle = seat->vehicle;
    } else if (const auto climbing = player_.entering(ped); climbing.vehicle != 0) {
        state.flags |= static_cast<std::uint32_t>(shared::PlayerFlag::EnteringVehicle);
        state.seat = climbing.seat;

        handle = climbing.vehicle;
    }

    // Номер машины не выдаётся, а узнаётся: машины заводит сервер, и у той, в
    // которую мы сели, номер уже есть. Ноль означает, что мы сидим в машине, о
    // которой сессия не знает, — такого быть не должно, но сказать об этом
    // серверу честнее, чем выдумать номер.
    state.vehicleId = handle != 0 ? vehicles_.idOf(handle) : shared::kInvalidVehicleId;

    // Какие машины рассылать, здесь больше не решается — это решил сервер,
    // назначив нас ведущим. Раньше правило считалось на месте: «рассылает тот,
    // кто занял в машине младшее место». Оно было верным ровно до тех пор, пока
    // в машине кто-то сидел, а брошенную машину не вёл никто.
    // Свой выстрел замечается по этому же снимку и уходит отдельным сообщением:
    // признак «стреляет» говорит лишь «жмёт на спуск», а сколько раз он
    // выстрелил и куда, из снимков не вывести — между двумя снимками помещается
    // три выстрела из автомата.
    if (const auto fired = player_.shot(state)) {
        mail_.postShot(fired->weapon, fired->target);
    }

    localState_.set(state, vehicles_.describeOwned(ped),
                    vehicles_.describeOwnedAppearances(ped, appearanceDue()));
}

bool GameSession::appearanceDue() {
    // Внешность снимается редко и намеренно: полсотни вызовов нативов ради
    // цвета, который не менялся с начала сессии, — это дорого за кадр и дёшево
    // раз в пару секунд. Отправит её сеть всё равно только при изменении.
    const auto now = Clock::now();

    if (now - vehicleAppearanceAt_ < kAppearanceInterval) {
        return false;
    }

    vehicleAppearanceAt_ = now;
    return true;
}

shared::Vec3 GameSession::spawnPoint() const {
    // Названная сервером, а не зашитая здесь. Пока он молчит — запасная: игрок,
    // уже оказавшийся в мире, должен где-то стоять, и начало координат для этого
    // не годится.
    return status_.snapshot().spawn.value_or(kFallbackSpawn);
}

void GameSession::applyServerState(int ped) {
    // Здоровье назначает сервер, и клиент о нём больше не свидетельствует — он о
    // нём узнаёт. Ставится оно прямо, а не отниманием урона: урон мог потеряться
    // по дороге, а число, пришедшее от сервера, верно само по себе.
    if (const auto health = mail_.takeIncomingHealth()) {
        player_.applyHealth(ped, health->health, health->armour);
    }

    // Снаряжение приходит редко — при входе, при выдаче и после смерти. Игра при
    // смерти отбирает оружие, и без этого воскресший поднимался бы с пустыми
    // руками.
    if (const auto loadout = mail_.takeIncomingLoadout()) {
        player_.applyLoadout(ped, loadout->weapons, loadout->replace);
    }

    // Попадания по машинам, которые ведём мы. Отнимает прочность ведущий, и
    // только он: у остальных эта машина — кукла, и её прочность им расскажет
    // наш же следующий снимок.
    for (const shared::VehicleDamaged& hit : mail_.takeIncomingVehicleDamage()) {
        vehicles_.harm(hit.vehicle, hit.harm);
    }

    // Предметы: сперва появившиеся, потом пропавшие. Порядок здесь не важен —
    // они друг с другом не связаны никак, — но пропавшие идут вторыми, чтобы
    // предмет, объявленный и убранный в одном пакете, не остался в мире.
    for (const shared::ObjectAdded& object : mail_.takeIncomingObjects()) {
        objects_.add(object);
    }
    for (const shared::ObjectId id : mail_.takeRemovedObjects()) {
        objects_.remove(id);
    }

    objects_.sync();
}

void GameSession::wearOwn(const shared::PlayerAppearance& appearance) {
    ownLook_ = appearance;

    // Модель меняется отдельно от одежды и не всегда: `want` сам решает, нужна
    // ли замена вовсе. Пересоздание персонажа теряет всё, что на нём было, и
    // делать его на каждое объявление внешности значило бы раздевать человека.
    if (appearance_.want(appearance.model)) {
        changingModel_ = true;
        return;
    }

    // Модель та же — остаётся одежда.
    look_.apply(player_.ped(), appearance);
    lookPublished_ = false;
}

void GameSession::publishAppearance(int ped) {
    if (ped == 0 || !look_.ready()) {
        return;
    }

    // Раз в секунду, а не каждый кадр: чтение стоит полусотни вызовов нативов, а
    // переодевается человек раз в час.
    const Clock::time_point now = Clock::now();
    if (lookPublished_ && now - lookCheckedAt_ < kLookInterval) {
        return;
    }
    lookCheckedAt_ = now;

    shared::PlayerAppearance look = look_.read(ped);

    if (lookPublished_ && look == publishedLook_) {
        return;
    }

    const bool first = !lookPublished_;

    publishedLook_ = look;
    lookPublished_ = true;

    // Первое объявление — в общий журнал, остальные в отладочный. Проверить
    // внешность глазами можно только вдвоём, а по этой строке видно, что своя
    // прочиталась и ушла, ещё до того, как найдётся второй игрок.
    if (first) {
        spdlog::debug("own appearance published: model {:#x}, top {}, legs {}, feet {}",
                     look.model, look.components[11].drawable, look.components[4].drawable,
                     look.components[6].drawable);
    } else {
        spdlog::debug("внешность изменилась, объявляем заново");
    }

    mail_.postAppearance(std::move(look));
}

void GameSession::showRemotePlayers(int ped) {
    // Погода и время — раньше всего остального: они касаются мира целиком, а не
    // того, кто в нём стоит, и ставить их после расстановки людей незачем.
    if (const auto world = mail_.takeIncomingWorld()) {
        world_.applyWorldState(*world);
    }

    // Внешность — раньше самих машин: она может прийти до первого снимка, и
    // машина, созданная в этом же кадре, должна оказаться уже покрашенной.
    for (const shared::VehicleControl& control : mail_.takeVehicleControls()) {
        vehicles_.lock(control);
    }

    for (const shared::VehicleAppearance& appearance : mail_.takeIncomingVehicleAppearances()) {
        vehicles_.applyAppearance(appearance);
    }

    // Внешность людей — по той же причине и тем же порядком: она приходит
    // надёжным каналом и обгоняет снимки, а персонаж создаётся по снимку.
    // Пришедшая раньше, она дождётся его в памяти чужих игроков.
    const shared::PlayerId self = status_.snapshot().playerId;

    for (const shared::PlayerAppearance& appearance : mail_.takeIncomingPlayerAppearances()) {
        // Своя внешность приходит только тогда, когда её назначил сервер: ту,
        // что мы объявили сами, он нам обратно не шлёт. Значит, спорить не о
        // чем — это распоряжение, и его нужно исполнить у себя.
        if (appearance.playerId == self && self != shared::kInvalidPlayerId) {
            wearOwn(appearance);
            continue;
        }

        remotePlayers_.dress(appearance.playerId, appearance);
    }

    // Машины появляются раньше людей: чужого игрока некуда сажать, пока его
    // машины нет. А убираются позже них, и это не симметрия ради красоты:
    // машина, убранная раньше сидящего в ней, оставляет игре персонажа внутри
    // несуществующей сущности. Падает она при этом не здесь, а на ближайшей
    // отрисовке, и след ведёт в d3d11 — туда, где искать нечего.
    // Состояние посчитано сетевым потоком, а показывает его этот, и мгновения
    // эти разные: сетевой просыпается на приходе пакетов — вразнобой и реже
    // кадров, — а мы рисуем свой кадр сейчас. Разница доводится по скорости,
    // иначе вся плавность, добытая интерполяцией, тратится впустую: кривая
    // считается ровно, а снимается с неё в неровные мгновения.
    const auto shownAt = Clock::now();

    std::vector<game::Vehicles::View> vehicles;

    for (const SessionVehicleView& vehicle : roster_.vehicles()) {
        game::Vehicles::View view{
            .state = vehicle.state,
            .owner = vehicle.owner,
        };

        // Ничью машину доводить нечего: она стоит там, где её оставили, а
        // скорость в её последнем снимке — та, с которой из неё вышли.
        if (const float age = ageOf(vehicle.computedAt, shownAt);
            age > 0.0F && view.owner != shared::kInvalidPlayerId) {
            view.state.position =
                interpolation::advance(view.state.position, view.state.velocity, age);
            view.state.rotation = interpolation::advanceAngles(
                view.state.rotation, view.state.angularVelocity, age);
        }

        vehicles.push_back(view);
    }

    // Список полный, включая машины, которые ведём мы: сервер знает обо всех, и
    // отличить свои от чужих можно по ведущему. Раньше свою приходилось называть
    // отдельно — её не присылали, и без напоминания её сочли бы пропавшей.
    vehicles_.sync(vehicles, status_.snapshot().playerId, ped);

    const std::vector<RemoteView> players = roster_.snapshot();

    std::vector<game::RemotePlayerView> views;
    views.reserve(players.size());

    for (const RemoteView& player : players) {
        game::RemotePlayerView view{
            .id = player.id,
            .nickname = player.nickname,
            .state = player.state,
        };

        if (const float age = ageOf(player.computedAt, shownAt); age > 0.0F) {
            view.state.position =
                interpolation::advance(view.state.position, view.state.velocity, age);
        }

        views.push_back(std::move(view));
    }

    remotePlayers_.sync(views, ped);

    // И только теперь — уборка машин, которых не стало. Люди уже расставлены:
    // те, кто вышел из сессии, убраны, а оставшиеся сидят там, где сидят. Машина
    // с седоком до следующего кадра не тронется — она помечена и подождёт.
    vehicles_.sweep();

    nameplates_.draw(views, remotePlayers_, player_.coords(ped));

    // Маркеры — последними в кадре, и это не порядок ради порядка: игра их у
    // себя не помнит, и нарисованное держится ровно один кадр. Пропустив вызов,
    // мы получим не сдвинувшуюся фигуру, а мигание.
    markers_.draw(player_.coords(ped));
}

bool GameSession::running() const noexcept {
    return tick_ != nullptr && tick_->frames() != 0;
}

bool GameSession::loadingScreenDone() const {
    if (stage_ != Stage::Playing) {
        return false;
    }

    // Раз убранный, экран загрузки не возвращается. Обрыв посреди игры — дело
    // сессии, а не загрузки: о нём говорит своё окно, и закрывать им уже
    // загруженный мир незачем.
    if (loadingScreenGone_) {
        return true;
    }

    if (status_.snapshot().inSession()) {
        loadingScreenGone_ = true;
        return true;
    }

    // **Пока сессии не было ни разу, экран загрузки остаётся, сколько бы ни
    // прошло.** Прежде он уходил по сроку в двенадцать секунд, и игрок, до
    // которого сервер не достучался, оказывался в одиночной GTA: ходил по
    // пустому Лос-Сантосу и считал, что мод сломан. Это хуже любого ожидания —
    // у ожидания хотя бы видна причина, а здесь её нет вовсе.
    //
    // Ждать при этом молча нельзя, и не приходится: страница к этому времени
    // уже сказала, что до сервера не достучаться, и чем это лечится.
    return false;
}

void GameSession::draw() {
    // Интерфейс рисует не игра, а слой поверх неё, и он же рисовал экран
    // загрузки. Изнутри игры экран загрузки появился бы уже после заставки
    // Rockstar и страницы выбора режима — то есть после всего, что должен был
    // собой закрыть; а уголок с состоянием сервера, который раньше рисовался
    // нативами игры, переехал туда же — к чату и консоли, в один правый угол.
    //
    // Здесь остаётся только решить, когда слою пора менять экран загрузки на
    // игровой интерфейс.
    if (!loadingScreenDone()) {
        return;
    }

    if (!readyReported_) {
        readyReported_ = true;

        // Проявить картинку обязаны мы: игре это запрещено — иначе она показала
        // бы недогруженный мир и чужого персонажа раньше, чем мы успеем
        // заменить одно и перенести другое. Забыть про это — значит оставить
        // игрока перед вечным чёрным экраном.
        screen_.fadeIn(kFadeIn);
        feed_.setReady();

        spdlog::info("Joined the world");
    }
}

} // namespace oxymp::client
