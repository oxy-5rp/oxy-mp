#include "client_app.hpp"

#include "console_sink.hpp"
#include "discord_presence.hpp"
#include "session_mail.hpp"
#include "ui_feed.hpp"

#include "game/crash_log.hpp"
#include "game/engine_addresses.hpp"
#include "game/hook.hpp"
#include "game/intro.hpp"
#include "game/landing_page.hpp"
#include "game/loaded_image.hpp"
#include "game/native_call.hpp"
#include "game/native_hashes.hpp"
#include "game/native_table.hpp"
#include "game/script_startup.hpp"
#include "game/ui_layer.hpp"
#include "game/window.hpp"
#include "game_session.hpp"
#include "session_status.hpp"

#include <oxymp/client/connection.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace oxymp::client {
namespace {

std::atomic<bool> g_stopRequested{false};

/// Описатель рабочего потока.
///
/// Намеренно голый HANDLE, а не std::thread: поток заводится из точки входа
/// модуля, то есть под блокировкой загрузчика, а std::thread — это ещё и
/// глобальный объект с нетривиальным разрушением, которое придётся на выгрузку
/// модуля. CreateThread здесь достаточно и не тянет ничего лишнего.
HANDLE g_worker = nullptr;

/// Значение переменной окружения в UTF-8 либо пусто.
///
/// Настройки передаются именно так: это не требует ни файлов рядом с игрой,
/// ни отдельного канала связи с лаунчером.
///
/// Читается широкая версия, а не getenv: лаунчер выставляет переменные как
/// UTF-16, и однобайтовое чтение испортило бы всё, что вне текущей кодовой
/// страницы, — например имя игрока кириллицей.
std::string environmentValue(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }

    std::wstring wide(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(name, wide.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    wide.resize(written);

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string value(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), value.data(),
                          size, nullptr, nullptr);

    return value;
}

/// Разбирает "адрес:порт". Порт необязателен.
void applyServerAddress(std::string_view text, Connection::Settings& settings) {
    if (text.empty()) {
        return;
    }

    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        settings.address = text;
        return;
    }

    const std::string_view port = text.substr(colon + 1);

    std::uint16_t parsed = 0;
    const auto* end = port.data() + port.size();
    if (std::from_chars(port.data(), end, parsed).ptr == end && parsed != 0) {
        settings.address = text.substr(0, colon);
        settings.port = parsed;
    } else {
        settings.address = text;
    }
}

std::filesystem::path logFilePath() {
    const std::string localAppData = environmentValue(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        return "oxymp-client.log";
    }

    return std::filesystem::path{localAppData} / "oxyMP" / "logs" / "client.log";
}

/// Направляет журнал в файл.
///
/// Консоли у процесса игры нет, поэтому файл — единственный способ узнать, что
/// происходило внутри.
void setUpLogging() {
    try {
        const std::filesystem::path path = logFilePath();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        // true: журнал каждого запуска начинается с чистого листа, иначе
        // разбираться в нём после нескольких попыток невозможно.
        auto logger = spdlog::basic_logger_mt("oxymp", path.string(), true);
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        // Сбрасывается на диск и отладочный уровень тоже: последняя строка перед
        // вылетом игры — единственное, что о нём известно, и оставлять её в
        // буфере значит терять именно ту запись, ради которой всё и ведётся.
        logger->flush_on(spdlog::level::debug);

        spdlog::set_default_logger(std::move(logger));

        // Подробности — только в отладочной сборке.
        //
        // Журнал попадает к людям: его присылают, когда что-то не работает. В
        // нём не должно быть ни адресов внутренностей игры, ни разбора её кода,
        // ни строчки на каждый кадр — всё это нужно тому, кто чинит, и никому
        // больше. Игроку в нём нужны ровно две вещи: дошло ли дело до игры и что
        // сломалось, если не дошло.
#ifdef NDEBUG
        spdlog::set_level(spdlog::level::info);
#else
        spdlog::set_level(spdlog::level::debug);
#endif
    } catch (const spdlog::spdlog_ex&) {
        // Без журнала работать можно, падать из-за него — нельзя.
    }
}

/// Настройки из файла сессии.
///
/// Запасной путь для случая, когда модуль внедрён в уже запущенную игру
/// (режим --attach лаунчера): окружения такой процесс от лаунчера не получал,
/// поэтому адрес сервера и имя берутся из файла, который лаунчер кладёт рядом
/// с журналом.
void applySessionFile(Connection::Settings& settings) {
    const std::string localAppData = environmentValue(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        return;
    }

    std::ifstream file(std::filesystem::path{localAppData} / "oxyMP" / "session.cfg");
    if (!file) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);

        if (key == "server") {
            applyServerAddress(value, settings);
        } else if (key == "nickname" && !value.empty()) {
            settings.nickname = value;
        }
    }
}

Connection::Settings readSettings() {
    Connection::Settings settings;

    // Файл читается первым, окружение — вторым и имеет приоритет: когда игру
    // запускает лаунчер, окружение свежее файла, оставшегося от прошлого раза.
    applySessionFile(settings);

    applyServerAddress(environmentValue(L"OXYMP_SERVER"), settings);

    if (std::string nickname = environmentValue(L"OXYMP_NICKNAME"); !nickname.empty()) {
        settings.nickname = std::move(nickname);
    }

    return settings;
}

/// Сколько добиваться опознания движка.
///
/// Модуль внедряется через доли секунды после старта процесса, а секции с кодом
/// к этому времени ещё зашифрованы: игра разворачивает их сама, и до этого
/// момента искать в них нечего. Ждать при этом нечего конкретного, поэтому
/// признаком готовности служит само удавшееся разрешение.
constexpr auto kEngineTimeout = std::chrono::seconds{120};

/// Номер приложения oxyMP в Discord.
///
/// Он же решает, какая картинка и какое имя показываются: и то и другое лежит на
/// стороне Discord, у приложения с этим номером.
constexpr const char* kDiscordClientId = "1526620827586658455";

/// Как называется окно игры, пока в ней работает oxyMP.
constexpr const char* kWindowTitle = "oxy:Multiplayer";

/// Сколько сервер вправе молчать, прежде чем об этом стоит сказать игроку.
///
/// Подключение занимает доли секунды, но первая попытка приходится на тот же
/// миг, что и её начало, и до ответа состояние честно называется «ожидание».
/// Показывать его как беду в эти секунды — значит писать «сервер не отвечает»
/// каждому входящему.
constexpr auto kSilenceBeforeAlarm = std::chrono::seconds{5};

/// Пауза между попытками опознать движок.
constexpr auto kEngineRetryDelay = std::chrono::milliseconds{500};

/// Разрешает каталог сигнатур, дожидаясь, пока игра развернёт свой код.
std::unique_ptr<game::EngineAddresses> resolveEngine() {
    const auto startedAt = std::chrono::steady_clock::now();
    const auto deadline = startedAt + kEngineTimeout;

    std::string error;
    std::string lastError;

    for (;;) {
        game::EngineAddresses::Failure failure = game::EngineAddresses::Failure::NotReady;

        if (const auto image = game::LoadedImage::open(error)) {
            if (auto addresses = game::EngineAddresses::resolveCatalog(*image, error, failure)) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt);
                spdlog::info("движок опознан за {} мс", elapsed.count());
                return addresses;
            }
        }

        // Игра другой версии — повторять бессмысленно.
        //
        // Раньше клиент две минуты долбился в заведомо безнадёжную попытку, а
        // потом молча продолжал работать без движка: сеть при этом жила, сервер
        // показывал игрока, а сюжет никто не гасил — и человек оказывался в
        // обычной одиночной игре, думая, что играет в мультиплеер. Хуже отказа
        // только отказ, притворившийся успехом.
        if (failure == game::EngineAddresses::Failure::WrongBuild) {
            spdlog::error("движок не опознан и не будет: {}", error);
            spdlog::error("каталог сигнатур выверен под одну сборку игры; на другой версии "
                          "клиент внутри игры работать не может");
            return nullptr;
        }

        // Одна и та же жалоба каждые полсекунды забьёт журнал, а меняется она
        // ровно в тот момент, когда что-то происходит, — его и записываем.
        if (error != lastError) {
            spdlog::debug("движок пока не опознан: {}", error);
            lastError = error;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            spdlog::error("не удалось опознать движок игры: {}", error);
            return nullptr;
        }

        std::this_thread::sleep_for(kEngineRetryDelay);
    }
}

/// Сколько ждать заполнения таблицы нативов.
///
/// Регистрация идёт по ходу инициализации игры и заканчивается заметно позже
/// того, как код станет доступен для поиска сигнатур.
constexpr auto kNativeTableTimeout = std::chrono::seconds{180};

constexpr auto kNativeTableRetryDelay = std::chrono::milliseconds{250};

/// Пауза между двумя замерами счётчика кадров при пробе вызова нативов.
constexpr auto kNativeProbeGap = std::chrono::milliseconds{500};

/// Сколько всего добиваться того, чтобы счётчик кадров сдвинулся.
///
/// Щедро: на медленной машине игра доходит до первого кадра долго, а цена
/// поспешного отказа — целая игровая часть, отменённая навсегда.
constexpr auto kNativeProbeTimeout = std::chrono::seconds{180};

/// Предел, означающий «все» при выгрузке хешей.
constexpr std::size_t kAllHashes = static_cast<std::size_t>(-1);

/// Выгружает все зарегистрированные хеши рядом с журналом.
///
/// Нужна для сопоставления с открытой базой нативов: имена в игре не хранятся,
/// поэтому единственный способ узнать, какому нативу принадлежит хеш, — сверить
/// список сборки с каноническим. Файл перезаписывается каждый запуск.
void dumpNativeHashes(const game::NativeTable& table) {
    const std::vector<std::uint64_t> hashes = table.registeredHashes(kAllHashes);
    if (hashes.empty()) {
        return;
    }

    const std::filesystem::path path = logFilePath().parent_path() / "natives.txt";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        spdlog::warn("не удалось записать список хешей в {}", path.string());
        return;
    }

    for (const std::uint64_t hash : hashes) {
        file << std::format("{:016X}\n", hash);
    }

    spdlog::info("список хешей выгружен: {} ({} штук)", path.string(), hashes.size());
}

/// Дожидается таблицы нативов и проверяет, что вызовы работают.
///
/// Проверяются сразу две вещи: что функция поиска вызвана по правильному
/// соглашению и что хеши записаны верно. Обе ошибки иначе всплыли бы много
/// позже и выглядели бы как беспричинный вылет.
bool probeNatives(const game::EngineAddresses& addresses) {
    const game::NativeTable table{addresses};
    if (!table.valid()) {
        spdlog::error("таблица нативов недоступна");
        return false;
    }

    // Регистрация идёт по ходу инициализации игры, поэтому ждём появления в
    // таблице хоть чего-нибудь. Ждать конкретный натив здесь нельзя: работает
    // ли поиск по конкретному хешу — это ровно то, что выясняется ниже.
    const auto startedAt = std::chrono::steady_clock::now();
    const auto deadline = startedAt + kNativeTableTimeout;

    while (table.registeredCount() == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            spdlog::error("таблица нативов так и не заполнилась");
            return false;
        }
        std::this_thread::sleep_for(kNativeTableRetryDelay);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    spdlog::info("в таблице зарегистрировано нативов: {} (ожидание {} мс)",
                 table.registeredCount(), elapsed.count());

    // Список хешей рядом с журналом — тоже только для отладки: игроку он не
    // нужен, а вопросов вызывает много.
#ifndef NDEBUG
    dumpNativeHashes(table);
#endif

    // Проба самого вызова.
    //
    // Счётчик кадров выбран мерилом потому, что растёт всегда, пока игра
    // рисует, — в отличие от игрового времени, которое вне сессии стоит на нуле
    // и ничего не доказывает. Выросшее между двумя вызовами значение означает,
    // что и хеш верен, и контекст вызова собран правильно.
    const game::NativeHandler frames = table.handlerFor(game::natives::kGetFrameCount);
    if (frames == nullptr) {
        spdlog::error("натив счётчика кадров не найден — хеши не годятся для этой сборки");
        return false;
    }

    // Проба повторяется, а не делается однажды, и это исправление по журналу с
    // чужой машины.
    //
    // Раньше счётчик замерялся дважды подряд, и не выросший означал приговор:
    // игровая часть не создавалась вовсе. Но не выросший счётчик значит и
    // «нативы сломаны», и «игра ещё не рисует» — а второе на загрузке
    // совершенно нормально. У кого игра доходила до кадров за эти полсекунды,
    // всё работало; у кого нет — клиент навсегда отказывался от игровой части,
    // и человек оказывался в обычной одиночной игре с миссиями, при живой сети
    // и полном молчании о причине.
    //
    // Отличить одно от другого можно только временем: сломанные нативы не
    // починятся никогда, а незагрузившаяся игра рано или поздно нарисует кадр.
    const auto startedProbe = std::chrono::steady_clock::now();
    const auto probeDeadline = startedProbe + kNativeProbeTimeout;

    const auto before = game::invokeNative<std::int32_t>(frames);

    for (;;) {
        std::this_thread::sleep_for(kNativeProbeGap);

        const auto after = game::invokeNative<std::int32_t>(frames);

        if (after > before) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedProbe);

            spdlog::info("вызов нативов работает: счётчик кадров {} → {} (ожидание {} мс)", before,
                         after, waited.count());
            return true;
        }

        if (std::chrono::steady_clock::now() >= probeDeadline) {
            spdlog::error("счётчик кадров стоит на {} уже {} с — игра не рисует, а значит и "
                          "работать в ней нечему",
                          after,
                          std::chrono::duration_cast<std::chrono::seconds>(kNativeProbeTimeout)
                              .count());
            return false;
        }
    }
}

/// Составляет список чужих игроков для показа в игре.
///
/// Положение берётся не последним пришедшим, а посчитанным на текущий момент:
/// снимки приходят реже кадров и с неровными промежутками, и по последнему
/// игроки двигались бы рывками.
std::vector<RemoteView> describeRemotePlayers(const Connection& connection) {
    const auto now = std::chrono::steady_clock::now();

    std::vector<RemoteView> players;
    players.reserve(connection.remotePlayers().size());

    for (const auto& [id, player] : connection.remotePlayers()) {
        if (!player.hasState) {
            continue;
        }

        // Всё, кроме положения, берётся из последнего снимка как есть:
        // достраивать по времени имеет смысл только то, что меняется плавно, а
        // «целится» и «стреляет» плавно не меняются.
        RemoteView view;
        view.id = id;
        view.nickname = player.nickname;
        view.state = player.latest;
        view.state.position = player.interpolatedPosition(now);

        players.push_back(std::move(view));
    }

    return players;
}

/// Составляет список чужих машин на текущий момент.
std::vector<RemoteVehicleView> describeRemoteVehicles(const Connection& connection) {
    const auto now = std::chrono::steady_clock::now();

    std::vector<RemoteVehicleView> vehicles;
    vehicles.reserve(connection.remoteVehicles().size());

    for (const auto& [owner, vehicle] : connection.remoteVehicles()) {
        if (!vehicle.hasState) {
            continue;
        }

        RemoteVehicleView view;
        view.state = vehicle.latest;
        view.state.position = vehicle.interpolatedPosition(now);

        vehicles.push_back(view);
    }

    return vehicles;
}

/// Кто сейчас в сессии, включая нас, — для списка на экране.
std::vector<UiFeed::Participant> describeRoster(const Connection& connection,
                                                const std::string& own) {
    std::vector<UiFeed::Participant> roster;
    roster.reserve(connection.remotePlayers().size() + 1);

    if (connection.localPlayerId() != shared::kInvalidPlayerId) {
        roster.push_back(UiFeed::Participant{.id = connection.localPlayerId(), .nickname = own});
    }

    for (const auto& [id, player] : connection.remotePlayers()) {
        roster.push_back(UiFeed::Participant{.id = id, .nickname = player.nickname});
    }

    // По номеру, а не по порядку в карте: список на экране не должен
    // перетасовываться сам по себе от кадра к кадру.
    std::ranges::sort(roster, {}, &UiFeed::Participant::id);

    return roster;
}

/// Собирает игровую часть клиента.
///
/// Отдельной функцией, а не внутри run: сюда стягиваются все причины, по
/// которым игровой части может не быть, и ни одна из них не должна отменять
/// работу сетевой.
std::unique_ptr<GameSession> startGameSession(const game::EngineAddresses& addresses,
                                              const Connection::Settings& settings,
                                              const SessionStatus& status,
                                              const RemoteRoster& roster, LocalState& localState,
                                              SessionMail& mail, UiFeed& feed,
                                              std::unique_ptr<game::ScriptStartup>& startup) {
    std::string error;

    // Скрипты игры запускаются как обычно. Запрещать их целиком нельзя: отсюда
    // же растут меню паузы и HUD, и без них игра падает при первом обращении —
    // проверено вылетом на нажатии Esc. Сюжет гасится поимённо, уже в тике.
    startup = game::ScriptStartup::install(addresses, game::GameScripts::Run, error);
    if (startup == nullptr) {
        spdlog::error("перехват запуска скриптов не поставлен: {}", error);
    }

    // Переключатель разведки читается здесь же, где и остальные настройки, и
    // тем же способом — переменной окружения. Пересборка ради него не нужна
    // намеренно: подделка сетевого состояния вправе уронить игру, и тогда
    // выключить её надо будет, не имея возможности запуститься.
    //
    // Значений три, а не два, потому что признаков два и порознь они ведут себя
    // по-разному: смешав их, нельзя узнать, который из них уронил игру.
    const std::string netgame = environmentValue(L"OXYMP_NETGAME");
    const std::string sessionSwitch = environmentValue(L"OXYMP_SESSION");

    GameSession::Settings sessionSettings{
        .serverAddress = std::format("{}:{}", settings.address, settings.port),
        .nickname = settings.nickname,
        .forceNetworkGame = netgame == "full"  ? game::NetworkGame::Fake::Both
                            : netgame == "1"   ? game::NetworkGame::Fake::Established
                                               : game::NetworkGame::Fake::None,
        // Сессия поднимается по умолчанию, и это перемена по существу: ради неё
        // всё и затевалось. Выключатель остаётся — вмешательство глубокое, и
        // если игра из-за него перестанет запускаться, выключить его надо будет,
        // не имея возможности запуститься.
        .hostSession = sessionSwitch != "0" && sessionSwitch != "off",
        .sessionMode = sessionSwitch == "raw" ? game::NetSession::Mode::Raw
                                              : game::NetSession::Mode::Solo,
    };

    auto session = GameSession::create(addresses, std::move(sessionSettings), status, roster,
                                       localState, mail, feed, error);
    if (session == nullptr) {
        spdlog::error("игровая сессия не создана: {}", error);
    }

    return session;
}

void run() {
    setUpLogging();

    const Connection::Settings settings = readSettings();

    // Всё, что показывает игровой интерфейс. Он живёт внутри этого же процесса
    // и рисуется прямо в кадр игры, поэтому состояние доходит до него напрямую,
    // а не через разделяемую память, как до экрана загрузки.
    UiFeed feed;
    feed.describeSession(std::format("{}:{}", settings.address, settings.port),
                         settings.nickname);

    // Ловушка падений ставится раньше всего остального: всё, что делает клиент
    // дальше, вправе уронить игру, и от этого мгновения в журнале должна
    // остаться строка, а не обрыв.
    const std::unique_ptr<game::CrashLog> crashLog = game::CrashLog::install();

    // Журнал раздваивается: в файл, как и прежде, и в игровую консоль на F8.
    // Ставится это до первой содержательной строки — консоль должна показывать
    // ход запуска с самого начала, а не с середины.
    spdlog::default_logger()->sinks().push_back(std::make_shared<ConsoleSink>(feed));

    spdlog::info("клиент запущен внутри игры: сервер {}:{}, имя \"{}\"", settings.address,
                 settings.port, settings.nickname);

    // Опознание движка идёт до сети и не мешает ей: даже если игра обновилась и
    // каталог устарел, соединение с сервером остаётся рабочим — просто в самой
    // игре ничего не появится.
    const std::unique_ptr<game::EngineAddresses> engine = resolveEngine();

    // Механизм перехвата поднимается сразу за опознанием движка, а не вместе с
    // игровой сессией. Причина во времени: сессия ждёт заполнения таблицы
    // нативов, а это минута, тогда как заставку нужно убрать в первые секунды —
    // позже убирать будет нечего, она уже прошла.
    bool hooksReady = false;

    if (engine != nullptr) {
        feed.setStage(shared::LoadStage::Engine);

        game::skipLegalScreens(*engine);

        std::string error;

        // Страница выбора режима убирается здесь же и по той же причине: до неё
        // остаётся около минуты, а скриптовый тик, из которого работает всё
        // остальное, начинается уже после неё.
        if (!game::skipLandingPage(*engine, error)) {
            spdlog::error("страница выбора режима останется: {}", error);
        }

        if (game::HookEngine::initialise(error)) {
            hooksReady = true;
        } else {
            spdlog::error("{}", error);
        }
    }

    // Объявлены раньше сессии и потому переживают её: сессия пользуется ими из
    // потока игры до самого своего разрушения.
    SessionStatus status;

    // Кто ещё в сессии. Список готовится сетевым потоком целиком и подменяется
    // разом, чтобы поток игры не застал его наполовину обновлённым.
    RemoteRoster roster;

    // Обратное направление: сюда поток игры кладёт своё положение, а сетевой
    // забирает и отправляет серверу.
    LocalState localState;

    // Почта между этим потоком и игровым: реплики чата и попадания. Всё
    // остальное между ними — состояние, у которого важно только последнее
    // значение; здесь наоборот, каждое событие обязано дойти ровно один раз.
    SessionMail mail;

    std::unique_ptr<game::ScriptStartup> scriptStartup;
    std::unique_ptr<GameSession> session;

    if (engine != nullptr && hooksReady && probeNatives(*engine)) {
        // Скриптовый движок готов. Дальше стадии публикует сессия — она видит
        // происходящее в игре покадрово, а мы отсюда уже нет.
        feed.setStage(shared::LoadStage::Scripts);

        session = startGameSession(*engine, settings, status, roster, localState, mail, feed,
                                   scriptStartup);
    }

    // Интерфейс внутри кадра игры. Поднимается после сессии и до сети: он
    // показывает и то и другое, а рисовать начинает с первого же кадра, который
    // игра покажет после его появления.
    //
    // Механизм перехвата ему нужен готовым — отсюда и проверка: без него
    // подменить показ кадра нечем.
    std::unique_ptr<game::UiLayer> ui;

    if (hooksReady) {
        std::string uiError;

        ui = game::UiLayer::create(feed, uiError);
        if (ui == nullptr) {
            spdlog::error("интерфейс в кадре игры не поднят: {}", uiError);
        }
    }

    // Соединение поднимается здесь, а не в начале, и это перемена по просьбе.
    //
    // Раньше клиент подключался сразу после внедрения — за минуту с лишним до
    // того, как игрок вообще попадал в мир. Толку от этого не было никакого:
    // отправлять было нечего, показывать некому, — а вреда два. Сервер целую
    // минуту числил игрока в сессии, хотя тот ещё смотрел заставку Rockstar; и,
    // главное, первая же неудачная попытка писала «сервер не отвечает» на экран
    // загрузки, где игроку и без того было тревожно.
    Connection connection{settings};

    // Discord держится сетевым потоком, а не игровым: кадр игры не имеет права
    // ждать чужой процесс, а Discord может быть закрыт или не установлен вовсе.
    DiscordPresence discord{kDiscordClientId};

    // Заголовок и значок окна ведёт этот же поток, а не игровой, и это не
    // вкусовщина. Окно принадлежит потоку игры, а обращение к чужому окну ждёт
    // его хозяина; сделай это из кадра игры — и ожидание станет взаимным.
    // Проверено: игра замирала сразу после ухода экрана загрузки.
    game::Window window;

    // Когда соединение началось. По нему решается, пора ли жаловаться игроку:
    // первые секунды сервер имеет право молчать, и объявлять его недоступным,
    // не дав ему ответить, — значит пугать на ровном месте.
    const auto connectionStartedAt = std::chrono::steady_clock::now();

    while (!g_stopRequested.load()) {
        connection.update(std::chrono::milliseconds{50});

        const std::size_t players = connection.remotePlayers().size();
        const std::optional<std::chrono::milliseconds> latency = connection.latency();

        status.update(connection.state(), players, latency, connection.localPlayerId());
        roster.replace(describeRemotePlayers(connection), describeRemoteVehicles(connection));

        if (const auto own = localState.get()) {
            connection.setLocalState(*own);
            connection.setLocalVehicle(localState.vehicle());
        }

        // Игровой поток просил отправить — отправляем.
        for (std::string& text : mail.takeOutgoingChat()) {
            connection.say(std::move(text));
        }
        for (const shared::DamageReport& report : mail.takeOutgoingDamage()) {
            connection.reportDamage(report.victim, report.amount, report.weapon);
        }
        for (const shared::AdminAction& action : mail.takeOutgoingAdmin()) {
            connection.order(action);
        }

        // Пришедшее раскладывается по двум разным адресатам: попадания нужны
        // игре, а строки чата — только экрану, и гонять их через игровой поток
        // незачем.
        mail.deliverDamage(connection.takeDamage());
        mail.deliverAdmin(connection.takeOrders());

        for (const shared::ChatLine& line : connection.takeChatLines()) {
            feed.pushChat(line.kind, line.kind == shared::ChatKind::Say
                                         ? std::format("{} [{}]: {}", line.nickname,
                                                       line.playerId, line.text)
                                         : line.text);
        }

        const bool troubled =
            connection.state() != ConnectionState::Connected &&
            std::chrono::steady_clock::now() - connectionStartedAt >= kSilenceBeforeAlarm;

        const int latencyMilliseconds =
            latency.has_value() ? static_cast<int>(latency->count()) : -1;

        feed.setConnection(UiFeed::Connection{
            .state = static_cast<unsigned int>(connection.state()),
            .players = players,
            .latencyMilliseconds = latencyMilliseconds,
            .playerId = connection.localPlayerId(),
            .troubled = troubled,
        });
        feed.setRoster(describeRoster(connection, settings.nickname));

        // Игроков на сервере на одного больше, чем чужих: себя в списке чужих
        // нет, а в Discord показывается общее число.
        discord.update(Presence{
            .server = std::format("{}:{}", settings.address, settings.port),
            .playerId = connection.localPlayerId(),
            .players = players + 1,
            .connected = connection.state() == ConnectionState::Connected,
        });

        // Страница интерфейса получает новое состояние отсюда же: этот цикл
        // и так крутится двадцать раз в секунду, а чаще ей нечего показывать.
        if (ui != nullptr) {
            ui->pump();
        }

        window.apply(kWindowTitle);
    }

    connection.disconnect();

    // Порядок обязателен: интерфейс держит перехват показа кадра, и снимать его
    // нужно раньше, чем будет снят сам механизм перехвата.
    ui.reset();

    session.reset();

    if (scriptStartup != nullptr) {
        spdlog::info("игра доходила до запуска скриптов раз: {}", scriptStartup->invocations());
        scriptStartup.reset();
    }

    game::HookEngine::shutdown();

    spdlog::info("клиент остановлен");
    spdlog::default_logger()->flush();
}

DWORD WINAPI workerEntry(LPVOID /*parameter*/) {
    run();
    return 0;
}

} // namespace

void ClientApp::requestStart() {
    g_stopRequested.store(false);

    // Поток создаётся под блокировкой загрузчика и до её снятия выполняться не
    // начнёт — это то, что нужно: точка входа модуля обязана вернуть управление
    // немедленно, а работа пойдёт уже без блокировки.
    g_worker = ::CreateThread(nullptr, 0, &workerEntry, nullptr, 0, nullptr);
}

void ClientApp::requestStop() {
    g_stopRequested.store(true);

    // Поток намеренно не ожидается: выгрузка модуля при завершении процесса
    // приходит уже после того, как Windows остановила все остальные потоки, и
    // ожидание здесь означало бы вечную блокировку.
    if (g_worker != nullptr) {
        ::CloseHandle(g_worker);
        g_worker = nullptr;
    }
}

} // namespace oxymp::client
