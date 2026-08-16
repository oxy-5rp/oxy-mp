#include "client_app.hpp"

#include "console_sink.hpp"
#include "discord_presence.hpp"
#include "resource_cache.hpp"
#include "session_mail.hpp"
#include "ui_feed.hpp"

#include "game/crash_log.hpp"
#include "game/engine_addresses.hpp"
#include "game/environment.hpp"
#include "game/file_system.hpp"
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
#include "game/world_hold.hpp"
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
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <shellapi.h>

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
///
/// Отвечает, был ли адрес назван вообще. Ответ важен: клиент без адреса никуда
/// не идёт сам, а ждёт слова от меню — так же, как alt:V.
bool applyServerAddress(std::string_view text, Connection::Settings& settings) {
    if (text.empty()) {
        return false;
    }

    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        settings.address = text;
        return true;
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

    return true;
}

/// Настройки клиента и то, откуда взялся адрес.
struct Startup {
    Connection::Settings connection;

    /// Назвал ли адрес тот, кто нас запустил.
    ///
    /// Различать обязательно, и это самая суть нового устройства запуска. Назвал
    /// — идём по нему, как прежде: так работают проверки и бот. Не назвал —
    /// висим в меню и ждём, пока сервер выберет игрок; мир при этом не грузится.
    bool addressGiven = false;
};

/// Просьба подключиться, переданная из потока CEF в сетевой.
///
/// Меню живёт в потоке CEF, соединение — в сетевом, и другого пути между ними
/// нет. Ждущих просьб не бывает двух: новая заменяет прежнюю — игрок, дважды
/// щёлкнувший по разным серверам, имел в виду последний.
class ConnectRequest {
public:
    struct Wanted {
        std::string address;
        std::string password;
    };

    void ask(std::string address, std::string password) {
        const std::lock_guard guard{mutex_};

        wanted_ = Wanted{std::move(address), std::move(password)};

        // Просьба подключиться отменяет неисполненную просьбу отключиться:
        // игрок, нажавший «отключиться» и тут же выбравший сервер, хочет второе.
        disconnect_ = false;
    }

    void askDisconnect() {
        const std::lock_guard guard{mutex_};

        wanted_.reset();
        disconnect_ = true;
    }

    [[nodiscard]] std::optional<Wanted> take() {
        const std::lock_guard guard{mutex_};

        std::optional<Wanted> taken;
        taken.swap(wanted_);
        return taken;
    }

    [[nodiscard]] bool takeDisconnect() {
        const std::lock_guard guard{mutex_};
        return std::exchange(disconnect_, false);
    }

private:
    std::mutex mutex_;
    std::optional<Wanted> wanted_;
    bool disconnect_ = false;
};

/// Куда писать журнал клиента.
///
/// Рядом с самим клиентом, в `logs`, — там же, где журналы лаунчера, патчера и
/// Chromium. Так же у alt:V, и причина не в подражании: человек, у которого
/// что-то не работает, присылает журналы, и просить его лезть за одним из них в
/// скрытую папку профиля — верный способ получить три из четырёх.
///
/// Имя с меткой времени, а не одно на всех. Прежде журнал начинался с чистого
/// листа при каждом запуске, и разобрать «а что было в прошлый раз» было уже
/// нельзя: чтобы прочесть его, игру приходилось запускать снова — и тем самым
/// стирать искомое.
///
/// Запасной путь — профиль пользователя: каталог клиента может оказаться
/// доступным только на чтение, а без журнала разбирать поломки нечем.
/// Журнал этого запуска, уже открытый.
///
/// Заполняется при настройке журнала и дальше только читается. Вычислить путь
/// второй раз нельзя: в имени стоит метка времени, и второе вычисление назовёт
/// файл, которого нет.
std::filesystem::path g_logFile;

std::filesystem::path logFilePath() {
    const auto now = std::chrono::system_clock::now();
    const std::string stamp = std::format("{:%Y-%m-%d_%H-%M-%S}",
                                          std::chrono::floor<std::chrono::seconds>(
                                              std::chrono::current_zone()->to_local(now)));

    const std::string name = std::format("client_{}.log", stamp);

    if (const std::filesystem::path directory = game::clientDirectory(); !directory.empty()) {
        return directory / "logs" / name;
    }

    const std::string localAppData = environmentValue(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        return name;
    }

    return std::filesystem::path{localAppData} / "oxyMP" / "logs" / name;
}

/// Направляет журнал в файл.
///
/// Консоли у процесса игры нет, поэтому файл — единственный способ узнать, что
/// происходило внутри.
void setUpLogging() {
    try {
        const std::filesystem::path path = logFilePath();
        g_logFile = path;

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
void applySessionFile(Startup& startup) {
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
            startup.addressGiven |= applyServerAddress(value, startup.connection);
        } else if (key == "nickname" && !value.empty()) {
            startup.connection.nickname = value;
        }
    }
}

Startup readSettings() {
    Startup startup;

    // Файл читается первым, окружение — вторым и имеет приоритет: когда игру
    // запускает лаунчер, окружение свежее файла, оставшегося от прошлого раза.
    applySessionFile(startup);

    startup.addressGiven |=
        applyServerAddress(environmentValue(L"OXYMP_SERVER"), startup.connection);

    if (std::string nickname = environmentValue(L"OXYMP_NICKNAME"); !nickname.empty()) {
        startup.connection.nickname = std::move(nickname);
    }

    return startup;
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
/// Каталог, в котором лежит сам модуль клиента.
///
/// Именно модуля, а не исполняемого файла: исполняемый файл здесь — GTA5.exe, и
/// её папка к oxyMP отношения не имеет. Рядом с модулем лежит всё наше: cef,
/// кеш, журналы.
std::filesystem::path clientDirectory() {
    HMODULE module = nullptr;

    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&clientDirectory), &module) == 0) {
        return {};
    }

    std::wstring path(MAX_PATH, L'\0');
    const DWORD written =
        ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0 || written >= path.size()) {
        return {};
    }
    path.resize(written);

    return std::filesystem::path{path}.parent_path();
}

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
/// Проверяет, что файловая система игры нам доступна, и говорит об этом.
///
/// Спрашивается настоящий файл, а не выдуманный: `settings.meta` лежит внутри
/// архива с зашифрованным оглавлением, и удавшееся чтение означает, что игра
/// расшифровала его за нас. Именно этим и снимается тупик со своими машинами —
/// подбирать схему шифрования не нужно, читает и пишет её сама игра.
///
/// Ничего не меняет: только читает и записывает в журнал.
void reportFileSystem(const game::EngineAddresses& addresses) {
    const game::FileSystem files{addresses};

    if (!files.ready()) {
        spdlog::warn("файловая система игры недоступна: подмена файлов работать не будет");
        return;
    }

    // Тот же файл, о котором рапортует alt:V своим «Replaced …settings.meta».
    constexpr const char* kProbe = "common:/data/control/settings.meta";

    const std::int64_t size = files.sizeOf(kProbe);
    if (size < 0) {
        spdlog::warn("файловая система игры отвечает, но {} в ней не нашлось", kProbe);
        return;
    }

    spdlog::info("файловая система игры доступна: {} — {} байт", kProbe, size);
}

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

/// Сколько игрок числится видимым после последнего снимка.
///
/// Заведомо длиннее и предела достраивания движения, и любой разумной заминки в
/// сети: пропасть игрок должен оттого, что отошёл за границу видимости, а не
/// оттого, что один пакет задержался.
constexpr auto kPlayerSilence = std::chrono::seconds{2};

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
        // Игрок, о котором не пришло ни одного снимка, не показывается: где он,
        // неизвестно, а поставить его в начало координат — значит собрать там
        // всех, кто ещё не успел о себе рассказать.
        if (!player.visible()) {
            continue;
        }

        // Замолчавший — тоже. Снимки о себе игрок шлёт непрерывно, пока он
        // рядом, и молчание означает ровно одно: он отошёл дальше, чем сервер
        // считает нужным о нём рассказывать.
        //
        // У машин молчание ничего не значило и означать не могло: стоящую машину
        // не рассылает никто, и убирать её по молчанию было той самой ошибкой,
        // из-за которой машины пропадали. Здесь наоборот — молчит только тот,
        // кого не стало видно.
        if (now - player.latestAt > kPlayerSilence) {
            continue;
        }

        RemoteView view;
        view.id = id;
        view.nickname = player.nickname;
        view.state = player.at(now);

        players.push_back(std::move(view));
    }

    return players;
}

/// Составляет список машин сессии на текущий момент.
std::vector<SessionVehicleView> describeSessionVehicles(const Connection& connection) {
    const auto now = std::chrono::steady_clock::now();

    std::vector<SessionVehicleView> vehicles;
    vehicles.reserve(connection.vehicles().size());

    for (const auto& [id, vehicle] : connection.vehicles()) {
        // Проверки на «был ли снимок» здесь больше нет, и это существенно.
        // Машина, о которой сервер объявил, существует с этого мгновения — даже
        // если снимков о ней ещё не было и не будет: стоящую машину никто не
        // рассылает. Пропусти мы её здесь — и брошенные машины не появились бы
        // в мире никогда.
        SessionVehicleView view;
        view.state = vehicle.at(now);
        view.owner = vehicle.owner;

        vehicles.push_back(view);
    }

    return vehicles;
}

/// Что меню услышало о соединении в последний раз.
///
/// Нужно потому, что говорить ему приходится о переменах, а не о состоянии: на
/// каждое слово страница перестраивает себя целиком, и повторять одно и то же
/// двадцать раз в секунду значило бы не дать ей нарисоваться вовсе.
enum class MenuStage {
    /// Ещё ничего не сказано либо сказано «подключаемся к серверу».
    Connecting,

    /// Сказано «идёт вход в игру»: сервер принял, мир грузится.
    Joining,

    Connected,
    Failed,
    Lost,
};

/// Рассказывает меню о ходе подключения.
///
/// Пока страница не услышит «подключились», она держит себя открытой поверх
/// всего; так она устроена, и так же ведёт себя alt:V.
///
/// «Подключились» говорится не по состоянию сети, а по уходу экрана загрузки.
/// Разница в минуты: сеть отвечает за доли секунды, а игрок в это время ещё едет
/// по загрузке, и меню, убранное по сетевому признаку, открыло бы ему пустой мир
/// без персонажа.
void tellMenu(Menu* menu, const Connection& connection, const UiFeed& feed, MenuStage& stage) {
    if (menu == nullptr) {
        return;
    }

    if (connection.state() == ConnectionState::Rejected) {
        if (stage != MenuStage::Failed) {
            stage = MenuStage::Failed;

            menu->failed(connection.rejectReason().has_value()
                             ? describe(*connection.rejectReason())
                             : std::string_view{"сервер отказал"});
        }
        return;
    }

    // Игрок был в сессии и остался без неё. Повторные попытки идут своим чередом,
    // и удавшаяся скажет «подключились» заново.
    if (stage == MenuStage::Connected &&
        connection.disconnectReason() == DisconnectReason::Lost) {
        stage = MenuStage::Lost;
        menu->disconnected("связь с сервером потеряна");
        return;
    }

    if (connection.state() != ConnectionState::Connected) {
        return;
    }

    if (!feed.ready()) {
        // Сервер принял, мир ещё грузится. Строка на странице меняется с
        // «подключаемся к серверу» на «входим в игру» — и висит там минуты,
        // ровно столько, сколько идёт загрузка.
        if (stage == MenuStage::Connecting) {
            stage = MenuStage::Joining;
            menu->joining();
        }
        return;
    }

    if (stage != MenuStage::Connected) {
        stage = MenuStage::Connected;
        menu->connected();
    }
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
    const std::string onlineMap = environmentValue(L"OXYMP_ONLINE_MAP");

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
        // Карта сетевого режима выключена по умолчанию: она под подозрением в
        // дырах, сквозь которые игрок проваливается. Включается тем же способом,
        // что и остальное, — переменной окружения, без пересборки.
        .onlineMap = onlineMap == "1" || onlineMap == "on",
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

    // Сразу за журналом и раньше всего прочего: сведения об окружении нужны
    // именно тогда, когда дальше что-то пошло не так, — а «дальше» начинается со
    // следующей строки.
    game::reportEnvironment();

    const Startup startup = readSettings();
    const Connection::Settings& settings = startup.connection;

    // Всё, что показывает игровой интерфейс. Он живёт внутри этого же процесса
    // и рисуется прямо в кадр игры, поэтому состояние доходит до него напрямую,
    // а не через разделяемую память, как до экрана загрузки.
    UiFeed feed;

    // Адрес называется только тогда, когда он есть. Без него экран загрузки
    // рассказывал бы игроку про 127.0.0.1 — умолчание, к которому мы никуда не
    // идём: сервер выбирается в меню, и до выбора говорить нечего.
    feed.describeSession(startup.addressGiven
                             ? std::format("{}:{}", settings.address, settings.port)
                             : std::string{},
                         settings.nickname);

    // Ловушка падений ставится раньше всего остального: всё, что делает клиент
    // дальше, вправе уронить игру, и от этого мгновения в журнале должна
    // остаться строка, а не обрыв.
    const std::unique_ptr<game::CrashLog> crashLog = game::CrashLog::install();

    // Журнал раздваивается: в файл, как и прежде, и в игровую консоль на F8.
    // Ставится это до первой содержательной строки — консоль должна показывать
    // ход запуска с самого начала, а не с середины.
    spdlog::default_logger()->sinks().push_back(std::make_shared<ConsoleSink>(feed));

    if (startup.addressGiven) {
        spdlog::info("клиент запущен внутри игры: сервер {}:{}, имя \"{}\"", settings.address,
                     settings.port, settings.nickname);
    } else {
        spdlog::info("клиент запущен внутри игры: сервер не назван, ждём выбора в меню");
    }

    // Опознание движка идёт до сети и не мешает ей: даже если игра обновилась и
    // каталог устарел, соединение с сервером остаётся рабочим — просто в самой
    // игре ничего не появится.
    const std::unique_ptr<game::EngineAddresses> engine = resolveEngine();

    // Механизм перехвата поднимается сразу за опознанием движка, а не вместе с
    // игровой сессией. Причина во времени: сессия ждёт заполнения таблицы
    // нативов, а это минута, тогда как заставку нужно убрать в первые секунды —
    // позже убирать будет нечего, она уже прошла.
    bool hooksReady = false;

    // Удержание мира, если оно поставлено. Живёт до конца работы клиента: пока
    // оно цело, поток игры вправе сидеть внутри него.
    std::unique_ptr<game::WorldHold> worldHold;

    if (engine != nullptr) {
        feed.setStage(shared::LoadStage::Engine);

        game::skipLegalScreens(*engine);

        std::string error;

        // Ролик Rockstar здесь больше не убирается, и это не забывчивость.
        // Правка «показывать ролик» в ноль останавливала раскрутку игры намертво:
        // состояние застревало на пятёрке — «ждём страницу выбора режима», — и
        // мир не грузился вовсе. Двенадцать запусков подряд, ни одного входа в
        // мир; сняли правку — вошли с первого. Подробности в docs/altv-parity.md.
        if (!game::muteLoadingMusic(*engine, error)) {
            spdlog::error("музыка загрузки останется: {}", error);
        }

        // Страница выбора режима убирается здесь же и по той же причине: до неё
        // остаётся около минуты, а скриптовый тик, из которого работает всё
        // остальное, начинается уже после неё.
        if (!game::skipLandingPage(*engine, error)) {
            spdlog::error("страница выбора режима останется: {}", error);
        }

        // Мир держится только тогда, когда сервер ещё не выбран, — то есть при
        // запуске из лаунчера, без адреса. Назвали адрес — держать нечего и
        // некого: идти в меню незачем, и придержанная игра означала бы, что бот
        // и проверки навсегда остались бы на чёрном экране.
        if (!startup.addressGiven) {
            worldHold = game::WorldHold::install(*engine, error);
            if (worldHold == nullptr) {
                spdlog::error("мир будет грузиться сразу: {}", error);
            }
        }

        if (game::HookEngine::initialise(error)) {
            hooksReady = true;
        } else {
            spdlog::error("{}", error);
        }
    }

    // Интерфейс внутри кадра игры поднимается сразу за перехватами и раньше
    // всего остального, и это не вкусовщина, а единственный способ показать
    // экран загрузки вовремя.
    //
    // Раньше он поднимался после игровой сессии, а та ждёт заполнения таблицы
    // нативов — минуту с лишним. Всё это время экран загрузки существовал
    // только в замысле: игрок смотрел заставку игры и её же страницу загрузки,
    // а наша появлялась к тому мгновению, когда закрывать ею было уже нечего.
    //
    // Механизм перехвата ему нужен готовым — отсюда и проверка: без него
    // подменить показ кадра нечем. Ничего другого, кроме перехватов, ему не
    // нужно: ни таблицы нативов, ни скриптового тика.
    std::unique_ptr<game::UiLayer> ui;

    // Чего меню просит у сети. Объявлено раньше слоя интерфейса и переживает
    // его: обработчики страницы держат ссылку на эту запись.
    ConnectRequest request;

    if (hooksReady) {
        std::string uiError;

        Menu::Actions actions;

        // Выход из игры. Сперва по-хорошему — закрытием окна: игра успевает
        // сохранить настройки и попрощаться с Social Club.
        //
        // По-хорошему выходит не всегда, и это выяснено на живой игре: пока мир
        // придержан, до разбора WM_CLOSE игра просто не доходит — нажатие
        // «выйти» не делало ничего. Поэтому следом идёт срок: не закрылась за
        // две секунды — закрываем сами. Терять при этом нечего, мир ещё не
        // загружен.
        actions.quit = [] {
            if (const HWND window = game::Window::findOwnWindow(); window != nullptr) {
                ::PostMessageW(window, WM_CLOSE, 0, 0);
            }

            spdlog::info("меню просит выйти из игры");

            std::thread{[] {
                std::this_thread::sleep_for(std::chrono::seconds{2});

                spdlog::warn("игра не закрылась сама — выходим");
                spdlog::default_logger()->flush();

                ::TerminateProcess(::GetCurrentProcess(), 0);
            }}.detach();
        };

        // Подключение и отключение исполняются не здесь, а в сетевом цикле, и
        // это не лишнее звено. Обработчик зовётся из потока CEF, и пока он не
        // вернётся, страница не отвечает на мышь; поднимать в нём соединение
        // значило бы подвесить меню на всё время попытки.
        actions.connect = [&request](const std::string& address, const std::string& password) {
            spdlog::info("меню просит подключиться к {}", address);
            request.ask(address, password);
        };

        actions.disconnect = [&request] { request.askDisconnect(); };

        // Журнал открывается тем, чем его открыл бы сам игрок, — блокнотом по
        // выбору Windows. Своего окна для чтения журнала клиент не заводит:
        // сотня строк текста в кадре игры хуже читается, чем в любом редакторе.
        actions.openLog = [] {
            if (g_logFile.empty()) {
                return;
            }

            const std::wstring path = g_logFile.wstring();

            ::ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };

        ui = game::UiLayer::create(feed, std::move(actions), uiError);
        if (ui == nullptr) {
            spdlog::error("интерфейс в кадре игры не поднят: {}", uiError);
        }
    }

    // Меню не поднялось — держать мир нельзя ни мгновения дольше. Выбирать
    // сервер игроку было бы нечем, а игра осталась бы стоять на чёрном экране
    // навсегда: закрыть её он смог бы только диспетчером задач.
    if (worldHold != nullptr && ui == nullptr) {
        spdlog::warn("выбирать сервер не в чем — отпускаем мир сразу");
        worldHold->release();
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
        // Файловая система игры к этому мгновению точно поднята: скриптовый
        // движок работает, а он сам читает через неё. Раньше спрашивать нельзя —
        // устройства навешиваются на пути не сразу.
        //
        // Пока только проверка доступа. Она нужна не сама по себе: подмена
        // файлов игры, ради которой всё и заводится, начинается с того, что мы
        // умеем спросить у игры файл её же средствами.
        reportFileSystem(*engine);

        // Скриптовый движок готов. Дальше стадии публикует сессия — она видит
        // происходящее в игре покадрово, а мы отсюда уже нет.
        feed.setStage(shared::LoadStage::Scripts);

        session = startGameSession(*engine, settings, status, roster, localState, mail, feed,
                                   scriptStartup);
    }

    // Соединения нет, пока его не попросят, и это главная перемена устройства
    // запуска: клиент приходит в игру без сервера, как alt:V.
    //
    // Просьб две, и они разные. Первая — от лаунчера, адресом в окружении: ей
    // клиент подчиняется сам, но не раньше, чем игрок появится в мире. Так было
    // и прежде, и причина не изменилась: подключаться посреди загрузки незачем —
    // отправлять нечего, а сервер тем временем числит игроком того, кто ещё
    // смотрит заставку.
    //
    // Вторая — от меню, щелчком по серверу. Эта исполняется немедленно: игрок
    // попросил сам и ждёт ответа. Отложить её до появления в мире значило бы
    // молчать те самые минуты, что идёт загрузка, — а если сервера нет, сказать
    // об этом надо сразу, а не в конце.
    std::unique_ptr<Connection> connection;

    // Адрес от лаунчера, пока он не исполнен. Пусто — значит его и не было.
    std::optional<std::string> launcherAddress;
    if (startup.addressGiven) {
        launcherAddress = std::format("{}:{}", settings.address, settings.port);
    }

    // Кеш ресурсов рядом с клиентом, а не в системных каталогах: игрок должен
    // видеть, чем занято место, и уметь очистить его, просто удалив папку.
    ResourceCache resources{clientDirectory() / "cache"};

    // Discord держится сетевым потоком, а не игровым: кадр игры не имеет права
    // ждать чужой процесс, а Discord может быть закрыт или не установлен вовсе.
    DiscordPresence discord{kDiscordClientId};

    // Заголовок и значок окна ведёт этот же поток, а не игровой, и это не
    // вкусовщина. Окно принадлежит потоку игры, а обращение к чужому окну ждёт
    // его хозяина; сделай это из кадра игры — и ожидание станет взаимным.
    // Проверено: игра замирала сразу после ухода экрана загрузки.
    game::Window window;

    // Когда соединение началось. Ставится не здесь, а в тот миг, когда к серверу
    // действительно пошли: по нему решается, пора ли жаловаться игроку, и отсчёт
    // от несделанной попытки объявил бы сервер молчащим ещё до того, как его
    // о чём-то спросили.
    std::chrono::steady_clock::time_point connectionStartedAt{};

    // Меню, если оно поднялось. Без него клиент работает по-прежнему — просто
    // рассказывать о ходе подключения будет некому.
    Menu* const menu = ui != nullptr ? ui->menu() : nullptr;

    // Куда идём сейчас. Не то же самое, что settings: адрес мог прийти от меню,
    // а имя — из настроек страницы.
    Connection::Settings active = settings;

    // Что меню уже знает о соединении.
    MenuStage menuStage = MenuStage::Connecting;

    while (!g_stopRequested.load()) {
        // Заголовок окна выправляется на каждом обороте, а не только при живом
        // соединении: окно игра пересоздаёт на переходах, и до появления игрока
        // в мире это случается не раз.
        window.apply(kWindowTitle);

        // Просьба отключиться разбирается первой: она отменяет всё остальное.
        if (request.takeDisconnect()) {
            if (connection != nullptr) {
                connection->disconnect();
                connection.reset();

                spdlog::info("меню просит отключиться — соединение закрыто");
            }

            // Мир при этом остаётся загруженным, и притворяться иначе нечем:
            // разобрать загруженную GTA обратно клиент не умеет. Игрок
            // возвращается в меню, но игра под ним та же.
            if (menu != nullptr) {
                menu->disconnected("вы отключились от сервера");
            }

            status.update(ConnectionState::Waiting, shared::kInvalidPlayerId);

            menuStage = MenuStage::Lost;
        }

        if (connection == nullptr) {
            std::optional<ConnectRequest::Wanted> start = request.take();

            // Слово лаунчера исполняется только по появлении игрока в мире —
            // причина в комментарии у объявления launcherAddress. Слово меню
            // исполняется сразу, потому и разбирается первым.
            if (!start && launcherAddress.has_value() && feed.playerInWorld()) {
                start = ConnectRequest::Wanted{*launcherAddress, {}};
                launcherAddress.reset();
            }

            if (!start) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
                continue;
            }

            active = settings;
            applyServerAddress(start->address, active);

            // Имя берётся из настроек меню, а не из довода лаунчера: игрок
            // правит его на странице, и правка обязана дойти до сервера с
            // первым же представлением. Пустое имя означает, что страницы нет
            // или он ничего не набрал, — тогда остаётся то, с чем нас запустили.
            if (menu != nullptr) {
                if (std::string name = menu->playerName(); !name.empty()) {
                    active.nickname = std::move(name);
                }
            }

            // Пароль идёт от страницы и никуда больше не сохраняется: ни в
            // настройки, ни в журнал. Строка живёт ровно до конца попытки.
            active.password = start->password;

            feed.describeSession(std::format("{}:{}", active.address, active.port),
                                 active.nickname);

            if (menu != nullptr) {
                menu->connecting(start->address);
            }

            spdlog::info("подключаемся к {}:{} именем \"{}\"", active.address, active.port,
                         active.nickname);

            // Сервер выбран — мир отпускается. Именно здесь, а не по успешному
            // подключению: загрузка идёт минуты, и начинать её после ответа
            // сервера значило бы сложить два ожидания в одно длинное. У alt:V
            // так же — меню показывает ход подключения, а мир тем временем уже
            // грузится.
            if (worldHold != nullptr) {
                worldHold->release();
            }

            connection = std::make_unique<Connection>(active);

            connectionStartedAt = std::chrono::steady_clock::now();
            menuStage = MenuStage::Connecting;
        }

        connection->update(std::chrono::milliseconds{50});

        const std::size_t players = connection->remotePlayers().size();

        status.update(connection->state(), connection->localPlayerId());

        if (const auto spawn = connection->spawnPosition()) {
            status.setSpawn(*spawn);
        }

        // Загрузка идёт здесь, в сетевом потоке, и это не выбор из удобства:
        // качать в потоке игры значило бы остановить кадр на время закачки.
        if (const auto offered = connection->takeResources()) {
            // Меню на это время показывает «ресурсы»: закачка идёт в этом же
            // потоке и занимает столько, сколько занимает, а молчащая страница
            // выглядит как зависшая.
            if (menu != nullptr) {
                menu->loadingResources();
            }

            for (const ResourceCache::Ready& item : resources.sync(active.address, active.port,
                                                                   *offered)) {
                feed.pushConsole(0, std::format("ресурс готов: {}", item.name));
            }

            // Стадия сбрасывается назад: следующий оборот скажет странице «входим
            // в игру» заново, и она сменит строку сама.
            if (menuStage == MenuStage::Joining) {
                menuStage = MenuStage::Connecting;
            }
        }
        roster.replace(describeRemotePlayers(*connection), describeSessionVehicles(*connection));

        if (const auto own = localState.get()) {
            connection->setLocalState(*own);
            connection->setOwnedVehicles(localState.vehicles());
            connection->setOwnedAppearances(localState.appearances());
        }

        // Игровой поток просил отправить — отправляем.
        for (std::string& text : mail.takeOutgoingChat()) {
            connection->say(std::move(text));
        }
        for (const shared::DamageReport& report : mail.takeOutgoingDamage()) {
            connection->reportDamage(report.victim, report.amount, report.weapon);
        }
        for (shared::ClientEvent& event : mail.takeOutgoingEvents()) {
            connection->emit(std::move(event.name), std::move(event.payload));
        }

        // Пришедшее раскладывается по двум разным адресатам: попадания нужны
        // игре, а строки чата — только экрану, и гонять их через игровой поток
        // незачем.
        mail.deliverDamage(connection->takeDamage());
        mail.deliverTeleports(connection->takeTeleports());
        mail.deliverServerEvents(connection->takeServerEvents());
        mail.deliverVehicleAppearances(connection->takeVehicleAppearances());
        mail.deliverWorld(connection->takeWorld());
        mail.deliverLoadout(connection->takeLoadout());
        mail.deliverHealth(connection->takeHealth());
        mail.deliverObjects(connection->takeObjects(), connection->takeRemovedObjects());

        for (const shared::ChatLine& line : connection->takeChatLines()) {
            feed.pushChat(line.kind, line.kind == shared::ChatKind::Say
                                         ? std::format("{} [{}]: {}", line.nickname,
                                                       line.playerId, line.text)
                                         : line.text);
        }

        const bool troubled =
            connection->state() != ConnectionState::Connected &&
            std::chrono::steady_clock::now() - connectionStartedAt >= kSilenceBeforeAlarm;

        const DisconnectReason lost = connection->disconnectReason();

        feed.setConnection(UiFeed::Connection{
            .state = static_cast<unsigned int>(connection->state()),
            .troubled = troubled,
            .disconnect = static_cast<unsigned int>(lost),

            // Объяснение прикладывается только к отказу: его сервер назвал
            // сам. У оборванной связи причины нет — есть только то, что её
            // больше нет, и придумывать здесь объяснение значило бы солгать.
            .disconnectDetail = connection->rejectReason().has_value()
                                    ? std::string{describe(*connection->rejectReason())}
                                    : std::string{},
        });

        tellMenu(menu, *connection, feed, menuStage);

        // Игроков на сервере на одного больше, чем чужих: себя в списке чужих
        // нет, а в Discord показывается общее число.
        discord.update(Presence{
            .server = std::format("{}:{}", active.address, active.port),
            .playerId = connection->localPlayerId(),
            .players = players + 1,
            .connected = connection->state() == ConnectionState::Connected,
        });
    }

    if (connection != nullptr) {
        connection->disconnect();
    }

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
