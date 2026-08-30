#include "client_app.hpp"

#include "console_sink.hpp"
#include "discord_presence.hpp"
#include "machine_id.hpp"
#include "resource_cache.hpp"
#include "session_mail.hpp"
#include "ui_feed.hpp"

#include "game/bink_sound.hpp"
#include "game/crash_log.hpp"
#include "game/custom_text.hpp"
#include "game/discord_block.hpp"
#include "game/engine_addresses.hpp"
#include "game/environment.hpp"
#include "game/data_files.hpp"
#include "game/file_device.hpp"
#include "game/manifests.hpp"
#include "game/file_system.hpp"
#include "game/streaming_files.hpp"
#include "game/focus_pause.hpp"
#include "game/hook.hpp"
#include "game/intro.hpp"
#include "game/keyboard_layout.hpp"
#include "game/landing_page.hpp"
#include "game/loaded_image.hpp"
#include "game/native_call.hpp"
#include "game/native_hashes.hpp"
#include "game/native_table.hpp"
#include "game/raw_input.hpp"
#include "game/script_startup.hpp"
#include "game/ui_layer.hpp"
#include "game/window.hpp"
#include "game/world_hold.hpp"
#include "game_session.hpp"
#include "frame_watch.hpp"
#include "game/asset_kind.hpp"
#include "game/packfiles.hpp"
#include "session_status.hpp"

#include <oxymp/cefui/browser.hpp>
#include <oxymp/config/settings.hpp>
#include <oxymp/client/connection.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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
#include <stop_token>
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

/// Просьба выйти из игры, переданная из потока CEF в сетевой.
///
/// Закрыть процесс можно и прямо из потока CEF — Windows закроет его откуда
/// угодно, — но тогда сервер узнаёт об уходе игрока не от него, а по молчанию, и
/// для остальных тот ещё полминуты стоит истуканом. Поэтому просьба сперва
/// доходит до сетевого потока: он прощается с сервером и закрывает игру сам.
class ExitRequest {
public:
    void ask() { asked_.store(true, std::memory_order_relaxed); }

    [[nodiscard]] bool asked() const { return asked_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> asked_{false};
};

/// Сколько ждать, пока сетевой поток простится с сервером.
///
/// Ждать приходится: просьба уходит в другой поток, и до неё он доберётся не в
/// то же мгновение. А срок нужен потому, что дойти она может и не успеть — до
/// сетевого цикла игра ещё добирается через две долгих поры ожидания (движок,
/// таблица нативов), и выйти из меню игрок вправе прямо посреди них.
///
/// Полсекунды — вдвое больше, чем нужно: сетевой цикл оборачивается за
/// пятьдесят миллисекунд, а прощание — один пакет, отправляемый тут же.
constexpr auto kGoodbyeGrace = std::chrono::milliseconds{500};

/// Как часто обслуживать соединение, пока идёт разбор ресурсов.
///
/// Разбор занимает сетевой поток целиком, а он же единственный кормит ENet.
/// Пятьдесят миллисекунд — обычный оборот его цикла: чаще незачем, реже —
/// значит опять молчать.
constexpr auto kSyncBeat = std::chrono::milliseconds{50};

/// Сколько знаков отпечатка брать в приставку архива.
///
/// Приставка — имя устройства в файловой системе игры, и коротким оно должно
/// быть не из красоты. У игры собственные имена устройств короткие — `common`,
/// `platform`, `dlcpacks`, — и на длинном она молча ничего не монтирует:
/// открытый архив вешается, но по приставке потом не читается ничего, и обход
/// её тоже не находит.
///
/// Четырёх знаков отпечатка довольно: архивов на сервере единицы, а совпадение
/// приставок просто отбрасывает второй архив, а не путает их между собой.
constexpr std::size_t kArchivePrefixLength = 4;

/// Закрывает игру.
///
/// Не ExitProcess: он останавливает остальные потоки где придётся и зовёт
/// выгрузку модулей под блокировкой загрузчика — а модуль тут мы сами, и в кадре
/// живёт Chromium со своими потоками. FiveM выходит из игры этим же движением, и
/// по той же причине.
void closeGame() {
    spdlog::default_logger()->flush();

    ::TerminateProcess(::GetCurrentProcess(), 0);
}

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

        // Подробности включает настройка, а не сборка.
        //
        // Журнал попадает к людям: его присылают, когда что-то не работает. В
        // нём не должно быть ни адресов внутренностей игры, ни разбора её кода,
        // ни строчки на каждый кадр — всё это нужно тому, кто чинит, и никому
        // больше. Игроку в нём нужны ровно две вещи: дошло ли дело до игры и что
        // сломалось, если не дошло.
        //
        // **Но решать это сборкой нельзя, и это уже стоило разбирательства.**
        // Стояло здесь `#ifdef NDEBUG`, и выпускной клиент — тот единственный,
        // что есть у людей, — не показывал ни одной подробной строки, что бы ни
        // стояло в `oxymp.toml`. Подмена звена признак слушалась, клиент нет: со
        // стороны это выглядело так, будто настройка есть, а толку от неё ноль.
        // Разбирать чужую поломку по такому журналу не по чему.
        //
        // Настройка читается здесь же, своим `config`: он для того и отделён от
        // игры и от сети, чтобы его можно было подключить и к лаунчеру, и к
        // внедряемому модулю. Спрашивать её у лаунчера через общий блок незачем —
        // файл лежит рядом с нами.
        // Файл настроек лежит рядом с клиентом — там же, где лаунчер его пишет.
        // Не нашёлся — получатся умолчания, и уровень останется обычным.
        const std::filesystem::path settings = game::clientDirectory() / "oxymp.toml";

        const bool verbose = config::Settings::load(settings).flag("debug");

        spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    } catch (const spdlog::spdlog_ex&) {
        // Без журнала работать можно, падать из-за него — нельзя.
    }
}

/// Настройки из файла сессии.
///
/// Файл лаунчер кладёт рядом с журналом перед каждым запуском. Отвечает, нашёлся
/// ли он: не нашёлся — значит игру подняли мимо лаунчера, и спрашивать остаётся
/// только окружение.
bool applySessionFile(Startup& startup) {
    const std::string localAppData = environmentValue(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        return false;
    }

    std::ifstream file(std::filesystem::path{localAppData} / "oxyMP" / "session.cfg");
    if (!file) {
        return false;
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

    return true;
}

Startup readSettings() {
    Startup startup;

    // Файл — источник правды, окружение — запасной путь, и порядок этот
    // выстрадан.
    //
    // Игру запускает не наш лаунчер, а Rockstar Games Launcher, и окружение она
    // наследует от него. Тот живёт куда дольше одного запуска: подняв его
    // однажды с адресом сервера, мы получаем этот же адрес во всех последующих
    // запусках — в том числе тогда, когда игрок не выбирал ничего. Ровно так
    // игра и уезжала «сама» на сервер прошлого раза, минуя меню и удержание
    // мира.
    //
    // Файл же лаунчер пишет заново перед каждым запуском, и пустая строка в нём
    // означает именно пустую строку. Нашёлся файл — окружение не спрашиваем
    // вовсе: свежее его в нём быть уже не может.
    if (applySessionFile(startup)) {
        return startup;
    }

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

/// Как называется сам oxyMP там, где на него смотрит игрок.
///
/// Прежде всего — в меню паузы самой игры: там стояло «GTA ONLINE», и это была
/// неправда. Игрок в нашей сессии, а не в чужой.
constexpr const char* kProductName = "Oxy Multiplayer";

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
                spdlog::debug("engine resolved in {} ms", elapsed.count());
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
            spdlog::error("the game engine did not resolve and will not: {}", error);
            spdlog::error("the signature catalogue is verified against one game build; on another "
                          "build the client inside the game cannot work");
            return nullptr;
        }

        // Одна и та же жалоба каждые полсекунды забьёт журнал, а меняется она
        // ровно в тот момент, когда что-то происходит, — его и записываем.
        if (error != lastError) {
            spdlog::debug("the engine has not resolved yet: {}", error);
            lastError = error;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            spdlog::error("failed to resolve the game engine: {}", error);
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
        spdlog::debug("could not write the hash dump to {}", path.string());
        return;
    }

    for (const std::uint64_t hash : hashes) {
        file << std::format("{:016X}\n", hash);
    }

    spdlog::debug("hash dump written: {} ({} entries)", path.string(), hashes.size());
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
        spdlog::warn("the game file system is unavailable: serving our own files will not work");
        return;
    }

    // Тот же файл, о котором рапортует alt:V своим «Replaced …settings.meta».
    constexpr const char* kProbe = "common:/data/control/settings.meta";

    const std::int64_t size = files.sizeOf(kProbe);
    if (size < 0) {
        spdlog::debug("the game file system answers, but {} was not found in it", kProbe);
        return;
    }

    spdlog::debug("the game file system is available: {} — {} bytes", kProbe, size);
}

/// Имя файла, если он лежит в каталоге моделей ресурса, — иначе пусто.
///
/// Имена ресурсов приходят составными: «ресурс/путь/к/файлу». Нас занимает
/// ровно один вид — «ресурс/stream/имя»: так лежат модели и текстуры и у alt:V,
/// и у FiveM, и договорённость эта старше нас обоих.
///
/// Вложенных каталогов внутри `stream` не бывает и быть не должно: игра ищет
/// модель по имени, а не по пути, и два одинаковых имени в разных подкаталогах
/// означали бы, что одна из моделей молча не найдётся.
[[nodiscard]] std::string streamFileName(std::string_view resourceEntry) {
    constexpr std::string_view kMarker = "/stream/";

    const std::size_t marker = resourceEntry.find(kMarker);
    if (marker == std::string_view::npos) {
        return {};
    }

    // Вглубь заходим: у карт содержимое разложено по подкаталогам —
    // `stream/maps`, `stream/props`, — и требовать плоского каталога значило бы
    // отказать половине чужих ресурсов. Игре подкаталог всё равно не виден: она
    // ищет по имени, а не по пути.
    const std::string_view tail = resourceEntry.substr(marker + kMarker.size());

    const std::size_t slash = tail.rfind('/');
    const std::string_view base = slash == std::string_view::npos ? tail : tail.substr(slash + 1);

    return std::string{base};
}

/// Стоит ли вообще предлагать этот файл потоковой подгрузке.
///
/// Список — не белый, а чёрный, и это выбрано нарочно. Белый список расширений
/// пришлось бы держать полным: пропусти в нём один вид — и ресурс, который на
/// нём держится, молча не заработает, а искать такое будут часами. Чёрный
/// перечисляет только то, про что мы знаем наверняка: игровым добром это не
/// бывает.
///
/// Нужен он ради журнала. Чужие карты приезжают с мусором, оставшимся от
/// FiveM: `fxmanifest.lua`, `_manifest.ymf`, забытый рядом `.zip` с
/// исходником. Игра такое отвергает — и на каждый файл мы писали человеку
/// испуганное предупреждение, с которым он не мог сделать ничего.
/// Имя файла, если это описание в корне ресурса, — иначе пусто.
///
/// Описания лежат рядом с каталогом моделей, а не внутри него: так их кладут и
/// у alt:V, и у FiveM. Вглубь не заходим — `.meta` в подкаталогах бывают у
/// чужого добра, которое ресурс просто носит с собой.
[[nodiscard]] std::string describedFileName(std::string_view resourceEntry) {
    const std::size_t slash = resourceEntry.rfind('/');

    const std::string_view tail =
        slash == std::string_view::npos ? resourceEntry : resourceEntry.substr(slash + 1);

    if (!tail.ends_with(".meta")) {
        return {};
    }

    return std::string{tail};
}

/// Кладёт рядом с клиентом файл, которым устройство себя проверяет.
///
/// Сама проверка идёт позже и не здесь: спросить у игры можно только из потока
/// с обработчиком скрипта, а это уже игровая сессия. Здесь — только положить
/// байты и объявить, что этот путь отдаём мы.
void prepareFileDeviceProbe(game::FileDevice& device) {
    const std::filesystem::path directory = clientDirectory();
    if (directory.empty()) {
        return;
    }

    const std::filesystem::path source = directory / "cache" / "device-probe.bin";

    {
        std::error_code failed;
        std::filesystem::create_directories(source.parent_path(), failed);

        std::ofstream out{source, std::ios::binary | std::ios::trunc};
        if (!out) {
            spdlog::debug("nothing to probe the device with: {} is not writable", source.string());
            return;
        }

        const std::string_view contents = game::FileDevice::kProbeContents;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    device.serve(game::FileDevice::kProbePath, source);
    device.mount("oxymp:/");
}

bool probeNatives(const game::EngineAddresses& addresses) {
    const game::NativeTable table{addresses};
    if (!table.valid()) {
        spdlog::error("the native table is unavailable");
        return false;
    }

    // Регистрация идёт по ходу инициализации игры, поэтому ждём появления в
    // таблице хоть чего-нибудь. Ждать конкретный натив здесь нельзя: работает
    // ли поиск по конкретному хешу — это ровно то, что выясняется ниже.
    const auto startedAt = std::chrono::steady_clock::now();
    const auto deadline = startedAt + kNativeTableTimeout;

    while (table.registeredCount() == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            spdlog::error("the native table never filled up");
            return false;
        }
        std::this_thread::sleep_for(kNativeTableRetryDelay);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    spdlog::debug("natives registered in the table: {} (waited {} ms)",
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
        spdlog::error("the frame counter native was not found: the hashes do not fit this build");
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

            spdlog::debug("calling natives works: frame counter {} -> {} (waited {} ms)", before,
                         after, waited.count());
            return true;
        }

        if (std::chrono::steady_clock::now() >= probeDeadline) {
            spdlog::error("the frame counter has been stuck at {} for {} s: the game is not drawing, "
                          "so there is nothing to work in",
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
        view.computedAt = now;

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
        view.computedAt = now;

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

    /// Сказано «ресурсы»: идёт сверка и закачка того, что предложил сервер.
    Resources,

    /// Сказано «грузим игру»: сервер принял, мир грузится. Самая долгая часть.
    Starting,

    /// Сказано «входим в мир»: мир загружен, идёт появление персонажа.
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
/// Сколько стадий у входа в мир. Столько же значений у shared::LoadStage.
constexpr unsigned int kLoadStages = 6;

/// Что меню уже знает о ходе подключения.
///
/// Одной записью, а не тремя отдельными переменными: все три меняются вместе и
/// нужны одному и тому же разговору.
struct MenuProgress {
    /// О чём говорили последним.
    MenuStage stage = MenuStage::Connecting;

    /// На каком делении полосы «грузим игру» остановились.
    unsigned int loadStage = 0;

    /// Назвали ли странице имя сервера. Оно приходит однажды, с приветствием.
    bool named = false;

    /// Когда начали подключаться и сказали ли уже, что не достучались.
    ///
    /// Нужно затем, что молчание здесь читается как «всё идёт»: пока соединения
    /// нет, странице раньше не говорили ничего вовсе, и игрок смотрел на
    /// «подключаемся» столько, сколько хватало терпения.
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    bool unreachableTold = false;
};

/// Через сколько молчания сервер считается недостижимым.
///
/// Пятнадцать секунд. Больше того, за что успевает пройти обычное подключение, и
/// меньше того, после чего человек решает, что мод сломан.
constexpr auto kUnreachableAfter = std::chrono::seconds{15};

void tellMenu(Menu* menu, const Connection& connection, const UiFeed& feed,
              MenuProgress& progress) {
    if (menu == nullptr) {
        return;
    }

    if (connection.state() == ConnectionState::Rejected) {
        if (progress.stage != MenuStage::Failed) {
            progress.stage = MenuStage::Failed;

            menu->failed(connection.rejectReason().has_value()
                             ? describe(*connection.rejectReason())
                             : std::string_view{"the server refused the connection"});
        }
        return;
    }

    // Игрок был в сессии и остался без неё. Повторные попытки идут своим чередом,
    // и удавшаяся скажет «подключились» заново.
    if (progress.stage == MenuStage::Connected &&
        connection.disconnectReason() == DisconnectReason::Lost) {
        progress.stage = MenuStage::Lost;
        menu->disconnected("the connection to the server was lost");
        return;
    }

    if (connection.state() != ConnectionState::Connected) {
        // До сервера не достучаться — и об этом надо сказать, а не молчать.
        //
        // Причина почти всегда одна и та же, и назвать её здесь дешевле, чем
        // разбираться потом по переписке: игра ходит по UDP, а проверяльщики
        // портов и наша же раздача ресурсов — по TCP. Открытый TCP 7790 ничего
        // не говорит об UDP 7790, и человек, у которого «порт открыт», ищет
        // поломку у нас.
        if (!progress.unreachableTold && progress.stage != MenuStage::Failed &&
            std::chrono::steady_clock::now() - progress.since >= kUnreachableAfter) {
            progress.unreachableTold = true;

            menu->failed("Cannot reach the server.\n"
                         "\nThe game talks over UDP, not TCP. A port checker only "
                         "tests TCP, so \"port is open\" does not mean the game can get "
                         "through.\nForward UDP as well, on the same port.");
        }

        return;
    }

    // Достучались — счёт молчания начинается заново: следующий обрыв должен
    // мериться от него, а не от запуска клиента.
    progress.since = std::chrono::steady_clock::now();
    progress.unreachableTold = false;

    // Имя сервера — то, что он назвал сам. До приветствия в заголовке стоит
    // адрес: другого имени у нас в этот миг нет.
    if (!progress.named && !connection.serverName().empty()) {
        progress.named = true;
        menu->nameServer(connection.serverName());
    }

    if (!feed.ready()) {
        // Сервер принял, а игрок ещё не в мире. Это самая долгая часть — минуты,
        // — и одной строкой на всю её длину дело не обходится: пока идёт
        // загрузка, страница показывает ход по стадиям, а как только игрок
        // появился в мире, говорит «входим в игру».
        //
        // Прежде здесь говорилось одно «входим в игру» на всё время, и игрок
        // минутами смотрел на строку, по которой нельзя понять, идёт ли хоть
        // что-нибудь.
        if (!feed.playerInWorld()) {
            const auto reached = static_cast<unsigned int>(feed.stage());

            if (progress.stage != MenuStage::Starting || reached != progress.loadStage) {
                progress.stage = MenuStage::Starting;
                progress.loadStage = reached;

                menu->startingGame(reached, kLoadStages);
            }
            return;
        }

        if (progress.stage != MenuStage::Joining) {
            progress.stage = MenuStage::Joining;
            menu->joining();
        }
        return;
    }

    if (progress.stage != MenuStage::Connected) {
        progress.stage = MenuStage::Connected;
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
                                              FrameWatch& watch, game::FileDevice* files,
                                              game::StreamingFiles* streamed,
                                              game::DataFiles* described,
                                              game::Packfiles* archives,
                                              game::Manifests* manifests,
                                              std::unique_ptr<game::ScriptStartup>& startup) {
    std::string error;

    // Скрипты игры запускаются как обычно. Запрещать их целиком нельзя: отсюда
    // же растут меню паузы и HUD, и без них игра падает при первом обращении —
    // проверено вылетом на нажатии Esc. Сюжет гасится поимённо, уже в тике.
    startup = game::ScriptStartup::install(addresses, game::GameScripts::Run, error);
    if (startup == nullptr) {
        spdlog::error("the script startup hook was not installed: {}", error);
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
                                       localState, mail, feed, watch, files, streamed, described,
                                       archives, manifests, error);
    if (session == nullptr) {
        spdlog::error("the game session was not created: {}", error);
    }

    return session;
}

void run() {
    setUpLogging();

    // Сразу за журналом и раньше всего прочего: сведения об окружении нужны
    // именно тогда, когда дальше что-то пошло не так, — а «дальше» начинается со
    // следующей строки.
    game::reportEnvironment();

    // Имя и значок окна — сразу за журналом и раньше всего остального.
    //
    // Раньше это делал сетевой цикл, а он начинается после опознания движка,
    // постановки перехватов и подъёма Chromium — то есть спустя десятки секунд.
    // Всё это время в панели задач висела «Grand Theft Auto V» с логотипом
    // Rockstar, и только потом появлялось наше. Здесь же ждать нечего: свой
    // поток дремлет, пока окна нет, и называет его в то самое мгновение, когда
    // игра его создаёт.
    const std::unique_ptr<game::WindowName> windowName = game::WindowName::hold(kWindowTitle);

    const Startup startup = readSettings();
    const Connection::Settings& settings = startup.connection;

    // Всё, что показывает игровой интерфейс. Он живёт внутри этого же процесса
    // и рисуется прямо в кадр игры, поэтому состояние доходит до него напрямую,
    // а не через разделяемую память, как до экрана загрузки.
    UiFeed feed;

    // Ловушка падений ставится раньше всего остального: всё, что делает клиент
    // дальше, вправе уронить игру, и от этого мгновения в журнале должна
    // остаться строка, а не обрыв.
    const std::unique_ptr<game::CrashLog> crashLog = game::CrashLog::install();

    // Журнал раздваивается: в файл, как и прежде, и в игровую консоль на F8.
    // Ставится это до первой содержательной строки — консоль должна показывать
    // ход запуска с самого начала, а не с середины.
    spdlog::default_logger()->sinks().push_back(std::make_shared<ConsoleSink>(feed));

    if (startup.addressGiven) {
        spdlog::debug("client started inside the game: server {}:{}, name \"{}\"", settings.address,
                     settings.port, settings.nickname);
    } else {
        spdlog::debug("client started inside the game: no server named, waiting for the menu");
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
            spdlog::error("the loading music will stay: {}", error);
        }

        // Страница выбора режима убирается здесь же и по той же причине: до неё
        // остаётся около минуты, а скриптовый тик, из которого работает всё
        // остальное, начинается уже после неё.
        if (!game::skipLandingPage(*engine, error)) {
            spdlog::error("the mode selection page will stay: {}", error);
        }

        // Пауза при потере фокуса убирается здесь же: в сетевой игре её быть не
        // должно вовсе, а признак игра выставляет рано.
        if (!game::keepRunningUnfocused(*engine, error)) {
            spdlog::error("the game will pause when it loses focus: {}", error);
        }

        // Раскладка отпускается как можно раньше: игра навязывает себе en-US,
        // заводя клавиатуру, и сделать это она успевает в первые же мгновения.
        // Опоздав, мы правили бы место, через которое она уже прошла.
        if (!game::unlockKeyboardLayout(*engine, error)) {
            spdlog::error("the keyboard layout will stay forced, and chat will take no Cyrillic: {}",
                          error);
        }

        // Мир держится только тогда, когда сервер ещё не выбран, — то есть при
        // запуске из лаунчера, без адреса. Назвали адрес — держать нечего и
        // некого: идти в меню незачем, и придержанная игра означала бы, что бот
        // и проверки навсегда остались бы на чёрном экране.
        if (!startup.addressGiven) {
            worldHold = game::WorldHold::install(*engine, error);
            if (worldHold == nullptr) {
                spdlog::error("the world will load right away: {}", error);
            }
        }

        if (game::HookEngine::initialise(error)) {
            hooksReady = true;
        } else {
            spdlog::error("{}", error);
        }
    }

    // Горячие клавиши Windows возвращаются сразу за перехватами и до того, как
    // игра заведёт ввод: попросится она однажды, и опоздавший перехват не
    // изменил бы ничего.
    std::unique_ptr<game::RawInput> rawInput;

    // Удержание раскладки — там же: без него игра возвращает свою раз за разом,
    // и переключение живёт доли секунды.
    std::unique_ptr<game::LayoutGuard> layoutGuard;

    if (hooksReady) {
        std::string error;

        layoutGuard = game::LayoutGuard::install(error);
        if (layoutGuard == nullptr) {
            spdlog::error("the game will keep taking the keyboard layout back: {}", error);
        }

        rawInput = game::RawInput::install(error);
        if (rawInput == nullptr) {
            spdlog::error("system hotkeys will stay taken away, and the layout inside the game "
                          "will not switch: {}",
                          error);
        }
    }

    // Надписи игры подменяются сразу за перехватами: в меню паузы у oxyMP не
    // должно быть написано «GTA ONLINE» — игрок в нашей сессии, а не в чужой.
    std::unique_ptr<game::CustomText> gameText;

    if (engine != nullptr && hooksReady) {
        std::string error;

        gameText = game::CustomText::install(*engine, error);
        if (gameText == nullptr) {
            spdlog::error("the game texts will stay its own: {}", error);
        } else {
            gameText->set("FE_THDR_GTAO", kProductName);
            gameText->set("PM_PANE_LEAVE", "Отключиться");
        }
    }

    // Звук вступительного ролика. Сам ролик убрать нельзя — правка, которая его
    // убирала, останавливала загрузку мира, — а звук можно.
    std::unique_ptr<game::BinkSound> binkSound;

    if (hooksReady) {
        std::string error;

        binkSound = game::BinkSound::mute(error);
        if (binkSound == nullptr) {
            spdlog::debug("the intro movie will sound as before: {}", error);
        }
    }

    // Показ игры в Discord затыкается независимо от движка: он идёт от Social
    // Club, а опознание сигнатур ему не нужно вовсе — перехватывается обычная
    // функция Windows. Нужен только сам механизм перехвата.
    std::unique_ptr<game::DiscordBlock> discordBlock;

    if (hooksReady) {
        std::string error;

        discordBlock = game::DiscordBlock::install(error);
        if (discordBlock == nullptr) {
            spdlog::error("the game will keep announcing itself in Discord: {}", error);
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

    // Просьба выйти — оттуда же и туда же, и потому живёт рядом.
    ExitRequest exitRequest;

    // Почта между этим потоком и игровым: реплики чата и попадания. Всё
    // остальное между ними — состояние, у которого важно только последнее
    // значение; здесь наоборот, каждое событие обязано дойти ровно один раз.
    //
    // Раньше слоя интерфейса и по той же причине: в неё кладёт строки консоль
    // страницы, а её обработчики держат ссылку отсюда.
    SessionMail mail;

    if (hooksReady) {
        std::string uiError;

        Menu::Actions actions;

        // Выход из игры — своим движением, а не просьбой к ней.
        //
        // Прежде окну слался WM_CLOSE, а через две секунды процесс добивался,
        // «если игра не закрылась сама». Не закрывалась она никогда: WM_CLOSE —
        // то же самое, чем просит закрыться Alt+F4, и в ответ игра показывает
        // свой вопрос «выйти из игры?». Игрок видел его поверх нашего, уже
        // отвеченного, и две секунды смотрел на него, пока не срабатывало
        // добивание. В журнале это стояло под каждым выходом без исключения:
        // «меню просит выйти» и следом «игра не закрылась сама».
        //
        // Спрашивать второй раз не о чем: игрок ответил нашему окну. Терять
        // тоже нечего — до прощания с Social Club и сохранения настроек игра в
        // том пути всё равно не доходила, застряв на своём вопросе.
        //
        // Закрывает игру при этом не этот поток, а сетевой: ему прежде нужно
        // проститься с сервером. Здесь остаётся только запасной срок на случай,
        // если до сетевого цикла игра ещё не дошла.
        actions.quit = [&exitRequest] {
            spdlog::debug("the menu asks to quit");

            exitRequest.ask();

            std::thread{[] {
                std::this_thread::sleep_for(kGoodbyeGrace);

                spdlog::debug("could not say goodbye to the server, quitting anyway");
                closeGame();
            }}.detach();
        };

        // Подключение и отключение исполняются не здесь, а в сетевом цикле, и
        // это не лишнее звено. Обработчик зовётся из потока CEF, и пока он не
        // вернётся, страница не отвечает на мышь; поднимать в нём соединение
        // значило бы подвесить меню на всё время попытки.
        actions.connect = [&request](const std::string& address, const std::string& password) {
            spdlog::debug("the menu asks to connect to {}", address);
            request.ask(address, password);
        };

        actions.disconnect = [&request] {
            spdlog::debug("the menu asks to disconnect");
            request.askDisconnect();
        };

        // Строка из консоли страницы уходит в чат — тем же путём, каким игрок
        // отправил бы её сам. Ничего сверх этого консоль не даёт и не должна:
        // своих правил игры у клиента нет, команды разбирает сервер.
        actions.say = [&mail](std::string text) { mail.postChat(std::move(text)); };

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
            spdlog::error("the in-game interface layer did not start: {}", uiError);
        } else {
            // Слой поднялся — значит окна ресурсов теперь есть кому заводить.
            game::UiLayer* const layer = ui.get();

            mail.setViewBridge(SessionMail::ViewBridge{
                .create = [layer](std::string url) { return layer->createView(std::move(url)); },
                .destroy = [layer](std::uint32_t view) { layer->destroyView(view); },
                .emit = [layer](std::uint32_t view, std::string name, std::string arguments) {
                    layer->emitView(view, std::move(name), std::move(arguments));
                },
                .show = [layer](std::uint32_t view, bool visible) {
                    layer->showView(view, visible);
                },
                .focus = [layer](std::uint32_t view, bool focused) {
                    layer->focusView(view, focused);
                },
                .cursor = [layer](bool show) { layer->showCursor(show); }});

            // Событие от страницы кладётся в почту, а исполняет его игровой
            // поток: зовут этот обработчик из потока CEF, и трогать оттуда
            // скриптовую машину нельзя.
            layer->onViewEvent([&mail](std::uint32_t view, std::string_view name,
                                       std::string_view arguments) {
                mail.deliverViewEvent(view, std::string{name}, std::string{arguments});
            });

            // Клавиши — тем же порядком и по той же причине: перехват ввода
            // живёт в своём потоке, а обработчик ресурса зовёт нативы.
            layer->onGameKey([&mail](unsigned key, bool down) {
                mail.postKey(static_cast<std::uint32_t>(key), down);
            });
        }
    }

    // Меню не поднялось — держать мир нельзя ни мгновения дольше. Выбирать
    // сервер игроку было бы нечем, а игра осталась бы стоять на чёрном экране
    // навсегда: закрыть её он смог бы только диспетчером задач.
    if (worldHold != nullptr && ui == nullptr) {
        spdlog::warn("nothing to pick a server with: releasing the world at once");
        worldHold->release();
    }

    // Объявлены раньше сессии и потому переживают её: сессия пользуется ими из
    // потока игры до самого своего разрушения.
    SessionStatus status;

    // Сторож кадра и его поток.
    //
    // Поток отдельный, а не заодно с этим циклом, и это условие работы: в
    // настоящем зависании сетевой цикл упирается в те же блокировки, что и
    // поток игры, и молчит вместе с ним — а сказать надо именно тогда.
    FrameWatch frameWatch;

    // jthread, а не thread: он сам просит остановки и дожидается потока в своём
    // разрушении. Выходов из этого цикла несколько, и забытая остановка в одном
    // из них оставила бы сторожа ссылаться на уже разрушенное.
    const std::jthread watcher{
        [&frameWatch](std::stop_token stop) { watchFrames(frameWatch, std::move(stop)); }};

    // Кто ещё в сессии. Список готовится сетевым потоком целиком и подменяется
    // разом, чтобы поток игры не застал его наполовину обновлённым.
    RemoteRoster roster;

    // Обратное направление: сюда поток игры кладёт своё положение, а сетевой
    // забирает и отправляет серверу.
    LocalState localState;

    std::unique_ptr<game::ScriptStartup> scriptStartup;

    // Архивы игры, присланные сервером: открывает их она сама, мы только просим.
    std::unique_ptr<game::Packfiles> packfiles;

    // Своё устройство файловой системы игры: через него ей отдаются файлы,
    // присланные сервером.
    //
    // Объявлено раньше сессии намеренно: сессия держит на него ссылку и трогает
    // его каждым кадром, а разрушается всё здесь в обратном порядке объявления.
    std::unique_ptr<game::FileDevice> fileDevice;

    // Вторая половина того же: отданный файл нужно ещё и объявить стримингу,
    // иначе игра его не найдёт — она ищет не по путям, а по имени в своём
    // списке подгружаемого.
    std::unique_ptr<game::StreamingFiles> streamedFiles;

    // И третья: описания, из которых игра узнаёт, что такая машина вообще
    // бывает. Без них у неё есть модель, но нет машины.
    std::unique_ptr<game::DataFiles> dataFiles;

    // И четвёртая, без которой карта доезжает, но в мир не встаёт: опись
    // содержимого. Из неё игра узнаёт, что расстановку вообще надо ставить.
    std::unique_ptr<game::Manifests> manifests;

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

        // Своё устройство — вторая половина того же дела: спросить у игры файл
        // мы уже умеем, теперь умеем и отдать ей свой.
        std::string deviceError;
        fileDevice = game::FileDevice::create(*engine, deviceError);

        if (fileDevice == nullptr) {
            spdlog::warn("our file device was not created: {}", deviceError);
        } else {
            prepareFileDeviceProbe(*fileDevice);
        }

        std::string streamingError;
        streamedFiles = game::StreamingFiles::create(*engine, streamingError);

        if (streamedFiles == nullptr) {
            spdlog::warn("nothing to declare our models to the game with: {}", streamingError);
        }

        std::string dataError;
        dataFiles = game::DataFiles::create(*engine, dataError);

        if (dataFiles == nullptr) {
            spdlog::warn("nothing to hand our data files to the game with: {}", dataError);
        }

        std::string manifestError;
        manifests = game::Manifests::create(*engine, manifestError);

        if (manifests == nullptr) {
            spdlog::warn("nothing to read map manifests with: {}", manifestError);
        }

        std::string archiveError;
        packfiles = game::Packfiles::create(*engine, archiveError);

        if (packfiles == nullptr) {
            spdlog::warn("nothing to open game archives with: {}", archiveError);
        }

        // Скриптовый движок готов. Дальше стадии публикует сессия — она видит
        // происходящее в игре покадрово, а мы отсюда уже нет.
        feed.setStage(shared::LoadStage::Scripts);

        session = startGameSession(*engine, settings, status, roster, localState, mail, feed,
                                   frameWatch, fileDevice.get(), streamedFiles.get(),
                                   dataFiles.get(), packfiles.get(), manifests.get(),
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

    // Читать файлы ресурсов будут не здесь: скриптовая машина — из кадра игры,
    // страницы интерфейса — из потока Chromium. Способ им отдаётся один раз и
    // держит свёртки за разделяемый указатель, а не за этот кеш: тот живёт по
    // подключение, а окно ресурса переживает переход на другой сервер.
    const auto readFromBundle = [bundles = resources.bundles()](std::string_view resource,
                                                                std::string_view file,
                                                                std::vector<std::uint8_t>& out) {
        return bundles->read(resource, file, out);
    };

    mail.setResourceReader(readFromBundle);

    // Страницы режима читают свои файлы той же дорогой. Схема `http://resource`
    // заведена раньше — вместе с CEF, при запуске клиента, — и способ чтения ей
    // ставится теперь, когда свёртки есть чем открывать.
    cefui::setResourceReader(readFromBundle);

    // Discord держится сетевым потоком, а не игровым: кадр игры не имеет права
    // ждать чужой процесс, а Discord может быть закрыт или не установлен вовсе.
    DiscordPresence discord{kDiscordClientId};

    // Меню, если оно поднялось. Без него клиент работает по-прежнему — просто
    // рассказывать о ходе подключения будет некому.
    Menu* const menu = ui != nullptr ? ui->menu() : nullptr;

    // Куда идём сейчас. Не то же самое, что settings: адрес мог прийти от меню,
    // а имя — из настроек страницы.
    Connection::Settings active = settings;

    // Отпечаток машины считается один раз: он не меняется ни между попытками
    // подключения, ни между серверами, а лезть за ним в реестр на каждой
    // попытке значило бы делать одну и ту же работу заново.
    //
    // Лежит он отдельно от `active`, а не в нём: перед всяким подключением
    // `active` берётся заново из настроек, и положенное в него до того
    // затирается. Так уже случилось — сервер получал нулевой отпечаток при
    // живом и посчитанном.
    const std::uint64_t fingerprint = machineFingerprint();

    spdlog::debug("machine fingerprint {:#016x}", fingerprint);

    // Что меню уже знает о соединении.
    MenuProgress menuProgress;

    while (!g_stopRequested.load()) {
        // Просьба выйти разбирается прежде всех прочих: отвечать на них уже
        // некому. Прощание — то самое, ради чего просьба сюда и шла: сервер
        // узнаёт об уходе от игрока, а не по молчанию, и остальные видят его
        // ушедшим сразу.
        if (exitRequest.asked()) {
            if (connection != nullptr) {
                connection->disconnect();

                spdlog::debug("the server was told we are leaving");
            }

            closeGame();
        }

        // Просьба отключиться разбирается первой: она отменяет всё остальное.
        if (request.takeDisconnect()) {
            if (connection != nullptr) {
                connection->disconnect();
                connection.reset();

                spdlog::info("Disconnected");
            } else {
                // Просили отключиться, а соединения уже нет. Само по себе это
                // не беда — так выходит, когда щёлкнули дважды, — но по этой
                // строке видно, что просьба дошла, и разбираться дальше нужно
                // не в ней.
                spdlog::debug("отключиться просили, а соединения уже не было");
            }

            // Мир при этом остаётся загруженным, и притворяться иначе нечем:
            // разобрать загруженную GTA обратно клиент не умеет. Игрок
            // возвращается в меню, но игра под ним та же.
            if (menu != nullptr) {
                menu->disconnected("you left the server");
            }

            status.update(ConnectionState::Waiting, shared::kInvalidPlayerId);

            menuProgress = MenuProgress{.stage = MenuStage::Lost};
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
                // Показ в Discord идёт и здесь, пока игрок выбирает сервер.
                // Прежде он начинался с первой попытки подключения — то есть
                // человек, стоящий в меню, для Discord не играл ни во что, а
                // запустивший игру без сервера не играл вовсе никогда.
                discord.update(Presence{.inMenu = true});

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

            if (menu != nullptr) {
                menu->connecting(start->address);
            }

            spdlog::debug("connecting to {}:{} as \"{}\"", active.address, active.port,
                         active.nickname);

            // Сервер выбран — мир отпускается. Именно здесь, а не по успешному
            // подключению: загрузка идёт минуты, и начинать её после ответа
            // сервера значило бы сложить два ожидания в одно длинное. У alt:V
            // так же — меню показывает ход подключения, а мир тем временем уже
            // грузится.
            if (worldHold != nullptr) {
                worldHold->release();
            }

            // Кем мы назовёмся — ставится здесь, перед самым представлением, и
            // обе половины вместе. Имя Social Club спрашивает игра, и до её
            // входа в мир она молчит; спрошенное позже, оно опоздало бы —
            // режим читает `player.socialClubName` в обработчике входа.
            active.hwidHash = fingerprint;
            active.socialId = mail.socialId();
            active.socialName = mail.socialName();

            connection = std::make_unique<Connection>(active);

            menuProgress = MenuProgress{};
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
            // Ход разбора ресурсов страница показывает по-настоящему: отдельно
            // сверку того, что уже лежит в кеше, отдельно закачку недостающего,
            // и обе — с делением «столько-то из стольких». Закачка идёт в этом
            // же потоке и занимает столько, сколько занимает; молчащая страница
            // всё это время выглядела бы зависшей.
            // Соединение обслуживается прямо посреди разбора, и это не
            // украшение, а единственное, чем держится связь.
            //
            // Качаем мы в сетевом потоке — том самом и единственном, который
            // кормит ENet. Пока он занят закачкой, сервер не слышит от нас
            // ничего: ни ping, ни состояния. На карте в четыре с половиной
            // тысячи файлов разбор шёл полминуты, и сервер выкидывал нас по
            // молчанию раньше, чем закачка кончалась, — а клиент замечал это
            // ещё через полминуты, уже войдя в мир.
            auto beatAt = std::chrono::steady_clock::now();

            const auto report = [&](const ResourceCache::Report& progress) {
                if (const auto now = std::chrono::steady_clock::now(); now - beatAt >= kSyncBeat) {
                    beatAt = now;

                    // Без ожидания: время здесь тратится на закачку, а не на
                    // сон. Нужно лишь разобрать пришедшее и отправить своё.
                    connection->update(std::chrono::milliseconds{0});
                }

                if (menu == nullptr) {
                    return;
                }

                // Закачка показывается байтами, сверка — файлами, и это не
                // прихоть страницы: сверка тысячи файлов проходит мгновенно и
                // мерится их числом, а закачка весит гигабайты, и число файлов
                // о её длительности не говорит ничего.
                if (progress.downloading) {
                    menu->downloadingResources(progress.bytesDone, progress.bytesTotal,
                                               progress.bytesPerSecond);
                } else {
                    menu->validatingResources(progress.filesDone, progress.filesTotal);
                }
            };

            const std::vector<ResourceCache::Ready> ready =
                resources.sync(active.address, active.port, *offered, report);

            for (const ResourceCache::Ready& item : ready) {
                feed.pushConsole(0, std::format("ресурс готов: {}", item.name));
            }

            // Архивы игры открывает она сама.
            //
            // Приходят они целым файлом — так раздают свои машины и карты и
            // RAGE MP, и просто люди в интернете, — и до сих пор такой файл
            // доезжал до клиента и ложился в кеш мёртвым грузом: россыпь рядом
            // с ресурсом мы отдавать умели, а архив нет.
            //
            // Оглавление у архива зашифровано, но подбирать схему не нужно:
            // расшифровывает его сама игра, ей довольно попросить.
            if (packfiles != nullptr && fileDevice != nullptr) {
                for (const ResourceCache::Ready& item : ready) {
                    // Составное имя — часть скриптового ресурса, а не игровой
                    // файл. Архивы лежат отдельно и называются просто.
                    if (item.name.find('/') != std::string::npos ||
                        !game::isArchive(item.name)) {
                        continue;
                    }

                    // Приставка выводится из имени файла в кеше, а в нём лежит
                    // отпечаток содержимого: у одного и того же архива она
                    // всегда одна и та же, у разных — разная. Своего счётчика
                    // здесь нельзя: он сбился бы при переподключении, и тот же
                    // архив повесился бы вторым устройством.
                    const std::string stem = item.path.stem().string();
                    const std::string_view mark{stem};

                    // Путь для игры — её собственный, а не путь Windows, и это
                    // не придирка: `D:\...` она открыть отказывается, потому что
                    // разрешает пути по приставке, а у буквы диска приставки нет.
                    // Так же делает и CitizenFX: свой архив он сперва кладёт под
                    // свою приставку и открывает уже её.
                    //
                    // Отдаёт архив наше же устройство. Оно для этого годится
                    // целиком: архив читается по смещениям, а чтение по
                    // смещениям у него и есть то, чем игра подгружает модели.
                    // Имя файла берётся настоящее, а не отпечаток, и это не
                    // мелочь: у части архивов ключ расшифровки оглавления игра
                    // выбирает по имени и размеру файла. Переименованный архив
                    // она расшифрует не тем ключом и получит из оглавления
                    // мусор.
                    const std::string gamePath =
                        std::format("oxymp:/archives/{}", item.name);

                    fileDevice->serve(gamePath, item.path);
                    fileDevice->mount("oxymp:/");

                    // Вешается архив туда, где игра держит свои же DLC, — под
                    // `dlcpacks:/имя/`. Своя приставка не годится: игра её
                    // принимает, но по ней потом не отдаёт ничего — ни чтением,
                    // ни обходом. Здесь же устройство встаёт в то место, где
                    // содержимое такого архива и полагается искать.
                    const std::string stemName =
                        std::filesystem::path{item.name}.stem().string();

                    packfiles->mount(item.path, gamePath,
                                     std::format("dlcpacks:/{}/", stemName));
                }
            }

            // Имена расстановки карты. Собираются здесь, а просятся из кадра
            // игры: просьба идёт нативом.
            std::vector<std::string> placements;

            // Модели и текстуры ресурса отдаются игре.
            //
            // Узнаются они по каталогу `stream` внутри ресурса — ровно так же,
            // как у alt:V и FiveM. Класть их куда-то ещё режим волен, но тогда
            // это будут просто файлы: `stream` — то место, о котором обе стороны
            // договорились без лишних объявлений.
            if (fileDevice != nullptr) {
                // Что из раздаваемого — описание, решает сервер: он называет вид
                // в `[meta]`, а по имени файла вид карты не угадать. Догадка по
                // имени осталась ради ресурсов без этого раздела, и дальше
                // `.meta` рядом с машиной она не идёт.
                const auto describes = [](const shared::ResourceEntry& entry) {
                    return !entry.dataFile.empty() || !describedFileName(entry.name).empty();
                };

                for (const shared::ResourceEntry& entry : *offered) {
                    const std::string streamed = streamFileName(entry.name);
                    if (streamed.empty()) {
                        continue;
                    }

                    // Список архетипов проходит здесь даже названным в `[meta]`:
                    // он нужен игре дважды — и файлом, и описанием, — а описание
                    // ему выпишется тут же, ниже.
                    if (describes(entry) && !game::isArchetypeList(streamed)) {
                        continue;
                    }

                    // Опись содержимого — не модель, и стримингу её объявлять
                    // незачем. Но и выбрасывать нельзя: без неё расстановка
                    // карты доезжает файлом и в мир не встаёт. Её разбирает
                    // сама игра, отдельной дорогой.
                    // Расстановка объявляется стримингу как файл — без этого
                    // игре нечего будет применять, — а вдобавок запоминается:
                    // применить её нужно отдельной просьбой, нативом, и уже из
                    // кадра игры.
                    if (streamed.ends_with(".ymap")) {
                        placements.push_back(
                            std::filesystem::path{streamed}.stem().string());
                    }

                    if (streamed.ends_with(".ymf")) {
                        if (manifests != nullptr) {
                            manifests->add(resources.resourceRoot() / entry.name,
                                           entry.name.substr(0, entry.name.find('/')));
                        }
                        continue;
                    }

                    if (!game::worthStreaming(streamed)) {
                        spdlog::debug("{} is not a game asset, not offering it to streaming",
                                      entry.name);
                        continue;
                    }

                    const std::string gamePath = std::format("oxymp:/stream/{}", streamed);

                    fileDevice->serve(gamePath, resources.resourceRoot() / entry.name);
                    fileDevice->mount("oxymp:/");

                    if (streamedFiles != nullptr) {
                        streamedFiles->add(gamePath, streamed);
                    }

                    if (dataFiles != nullptr && game::isArchetypeList(streamed)) {
                        dataFiles->add(gamePath, streamed,
                                       entry.dataFile.empty() ? game::kArchetypeType
                                                              : std::string_view{entry.dataFile});
                    }
                }

                // Описания — отдельным проходом и после моделей, чтобы порядок
                // просьб совпадал с порядком, в котором игра их получит.
                for (const shared::ResourceEntry& entry : *offered) {
                    if (!describes(entry)) {
                        continue;
                    }

                    // Архетипы уже объявлены — вместе со своим файлом.
                    if (game::isArchetypeList(entry.name) && !streamFileName(entry.name).empty()) {
                        continue;
                    }

                    // Путь у описания свой, с именем ресурса внутри: одинаково
                    // названные `vehicles.meta` двух ресурсов иначе сошлись бы
                    // в одно.
                    const std::string gamePath = std::format("oxymp:/meta/{}", entry.name);

                    fileDevice->serve(gamePath, resources.resourceRoot() / entry.name);
                    fileDevice->mount("oxymp:/");

                    if (dataFiles != nullptr) {
                        dataFiles->add(gamePath, describedFileName(entry.name), entry.dataFile);
                    }
                }
            }

            // Имена расстановки уезжают в игровой поток: просить о ней можно
            // только оттуда, а узнаём мы их здесь.
            mail.deliverMapPlacements(std::move(placements));

            // Клиентские половины ресурсов уезжают в игровой поток.
            //
            // Точку входа сервер помечает сам (ResourceEntry::page): по имени
            // её не угадать, а угадывать нельзя — сегодня она называется
            // index.cjs, завтра иначе.
            std::vector<SessionMail::ClientResource> startable;

            for (const shared::ResourceEntry& entry : *offered) {
                const std::size_t slash = entry.name.find('/');

                // Составное имя — «ресурс/путь/к/файлу». Без косой черты это
                // игровой файл, а не часть ресурса.
                if (!entry.page || slash == std::string::npos) {
                    continue;
                }

                startable.push_back(SessionMail::ClientResource{
                    .name = entry.name.substr(0, slash),
                    .root = resources.resourceRoot() / entry.name.substr(0, slash),
                    .entry = entry.name.substr(slash + 1)});
            }

            if (startable.empty()) {
                // Клиентских половин нет вовсе — ждать нечего, и сервер об этом
                // нужно известить сразу. Иначе он держал бы событие входа до
                // конца сессии, и режим без клиентской части не работал бы вовсе.
                mail.postEvent(std::string{shared::kClientReadyEvent}, {});
            } else {
                mail.deliverClientResources(std::move(startable));
            }

            // Разложенное сервер теперь запускает — об этом странице и говорим.
            if (menu != nullptr) {
                menu->loadingResources();
            }

            // Стадия сбрасывается назад: следующий оборот расскажет про загрузку
            // мира заново, и страница сменит строку сама.
            menuProgress.stage = MenuStage::Resources;
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
        for (const shared::WeaponFired& fired : mail.takeOutgoingShots()) {
            connection->reportShot(fired.weapon, fired.target);
        }

        for (const shared::DamageReport& report : mail.takeOutgoingDamage()) {
            connection->reportDamage(report.victim, report.amount, report.weapon);
        }
        for (const shared::VehicleDamageReport& hit : mail.takeOutgoingVehicleDamage()) {
            connection->reportVehicleDamage(hit.vehicle, hit.harm, hit.weapon);
        }
        for (const shared::PedDamageReport& hit : mail.takeOutgoingPedDamage()) {
            connection->reportPedDamage(hit.victim, hit.amount, hit.weapon);
        }
        for (shared::ClientEvent& event : mail.takeOutgoingEvents()) {
            connection->emit(std::move(event.name), std::move(event.payload));
        }

        // Своя внешность уходит только когда изменилась: игровой поток кладёт её
        // сюда лишь в этом случае.
        if (const auto appearance = mail.takeOutgoingAppearance()) {
            connection->sendAppearance(*appearance);
        }

        // Пришедшее раскладывается по двум разным адресатам: попадания нужны
        // игре, а строки чата — только экрану, и гонять их через игровой поток
        // незачем.
        mail.deliverDamage(connection->takeDamage());
        mail.deliverTeleports(connection->takeTeleports());
        mail.deliverControl(connection->control());
        mail.deliverVehicleControls(connection->takeVehicleControls());
        mail.deliverVehicleDoors(connection->takeVehicleDoors());
        mail.deliverVehicleTeleports(connection->takeVehicleTeleports());
        mail.deliverVehicleRepairs(connection->takeVehicleRepairs());
        mail.deliverVehicleDamage(connection->takeVehicleDamage());
        mail.deliverBlips(connection->takeBlips());
        mail.deliverRemovedBlips(connection->takeRemovedBlips());
        mail.deliverAnimations(connection->takeAnimations());
        mail.deliverSpeeches(connection->takeSpeeches());
        mail.deliverBodyOrders(connection->takeBodyOrders());
        mail.deliverExplosions(connection->takeExplosions());
        mail.deliverShots(connection->takeShots());
        mail.deliverWeaponLooks(connection->takeWeaponLooks());
        mail.deliverPeds(connection->takePeds(), connection->takeRemovedPeds());
        mail.deliverAttachments(connection->takeAttachments());
        mail.deliverMarkers(connection->takeMarkers());
        mail.deliverRemovedMarkers(connection->takeRemovedMarkers());
        mail.deliverCheckpoints(connection->takeCheckpoints());
        mail.deliverRemovedCheckpoints(connection->takeRemovedCheckpoints());
        mail.deliverSeats(connection->takeSeats());
        mail.deliverServerEvents(connection->takeServerEvents());
        mail.deliverVehicleAppearances(connection->takeVehicleAppearances());
        mail.deliverPlayerAppearances(connection->takePlayerAppearances());
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

        tellMenu(menu, *connection, feed, menuProgress);

        // Игроков на сервере на одного больше, чем чужих: себя в списке чужих
        // нет, а в Discord показывается общее число.
        discord.update(Presence{
            // Имя сервера, если он назвался, и адрес, пока нет: человеку в
            // Discord имя говорит больше, а адрес — то, по чему к нему зайдут.
            .server = connection->serverName().empty()
                          ? std::format("{}:{}", active.address, active.port)
                          : connection->serverName(),
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
        spdlog::debug("the game reached script startup {} times", scriptStartup->invocations());
        scriptStartup.reset();
    }

    game::HookEngine::shutdown();

    spdlog::info("Client stopped");
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
