#include "session.hpp"

#include "game_conflicts.hpp"
#include "game_locator.hpp"
#include "game_mirror.hpp"
#include "game_platform.hpp"
#include "game_settings.hpp"
#include "game_store.hpp"
#include "launcher_patch.hpp"
#include "rockstar_launcher.hpp"
#include "text.hpp"

#include <oxymp/gamesig/catalog.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <windows.h>

#include <shellapi.h>

namespace oxymp::launcher {
namespace {

/// Сколько ждать появления процесса игры после запуска через лаунчер Rockstar.
constexpr auto kProcessTimeout = std::chrono::seconds{180};

/// Сколько добиваться внедрения. Сразу после запуска процесс занят собой и
/// удалённый поток в нём создать не удаётся.
constexpr auto kInjectTimeout = std::chrono::seconds{30};

/// Сколько ждать, пока человек войдёт в клиент своей площадки. Steam поднимается
/// быстро, но вход бывает и с подтверждением с телефона, и вводить его человек
/// будет не торопясь.
constexpr auto kStoreTimeout = std::chrono::seconds{120};

/// Сколько ждать готовности Rockstar Games Launcher, если поднимать его пришлось
/// нам. Холодный старт лаунчера с обновлением бывает долгим.
constexpr auto kLauncherTimeout = std::chrono::seconds{120};

/// Сколько ждать, пока подмена звена BattlEye встанет в лаунчере Rockstar.
///
/// Работы там на доли секунды: прочитать таблицу импорта и записать в неё один
/// указатель. Запас нужен на другое — модуль сперва должен загрузиться в чужой
/// процесс, а лаунчер в этот момент бывает занят собой.
constexpr auto kPatchTimeout = std::chrono::seconds{20};

bool setEnvironment(const wchar_t* name, const std::string& value) {
    const int size =
        ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(),
                          size);

    return ::SetEnvironmentVariableW(name, wide.c_str()) != 0;
}

/// Каталог пользовательских данных. Пусто, если Windows его не назвала.
std::filesystem::path localApplicationData() {
    std::wstring value(MAX_PATH, L'\0');

    const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(),
                                                    static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size()) {
        return {};
    }

    value.resize(written);
    return std::filesystem::path{value};
}

/// Просит Windows поднять нас же с правами — только ради сборки зеркала.
///
/// Тот же oxymp.exe, но с ключом --prepare-mirror: он собирает копию игры и сразу
/// заканчивается, не запуская ничего. Игру после этого поднимает обычный,
/// бесправный запуск — иначе права достались бы и ей, а с ними не работает
/// Chromium.
bool buildMirrorElevated(const Session::Settings& settings, std::string& error) {
    wchar_t self[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        error = "could not read our own path";
        return false;
    }

    std::wstring arguments = L"--prepare-mirror";

    // Каталог игры передаётся дальше, если его указали ключом: у запуска с
    // правами своё окружение, и найденное нами он сам не унаследует.
    if (!settings.gameDirectory.empty()) {
        arguments += L" --game \"" + settings.gameDirectory.wstring() + L"\"";
    }

    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_NOCLOSEPROCESS;
    request.lpVerb = L"runas";
    request.lpFile = self;
    request.lpParameters = arguments.c_str();
    request.nShow = SW_HIDE;

    if (::ShellExecuteExW(&request) == 0 || request.hProcess == nullptr) {
        // Отказ от прав — не поломка, а решение человека, и говорить о нём надо
        // так же.
        error = ::GetLastError() == ERROR_CANCELLED
                    ? "The pinned copy cannot be built without administrator rights.\n"
                      "Confirm the Windows prompt, or run without --standalone."
                    : "could not ask for administrator rights";
        return false;
    }

    ::WaitForSingleObject(request.hProcess, INFINITE);

    DWORD code = 1;
    ::GetExitCodeProcess(request.hProcess, &code);
    ::CloseHandle(request.hProcess);

    if (code != 0) {
        error = "the pinned copy could not be built - see the launcher log";
        return false;
    }

    return true;
}

/// Кладёт настройки сессии в файл рядом с журналом клиента.
///
/// Единственный способ передать адрес сервера и имя в уже запущенный процесс
/// (режим --attach): окружения он от нас не получал.
void writeSessionFile(const std::string& server, const std::string& nickname) {
    const std::filesystem::path localAppData = localApplicationData();
    if (localAppData.empty()) {
        return;
    }

    const std::filesystem::path path = localAppData / "oxyMP" / "session.cfg";

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) {
        file << "server=" << server << '\n' << "nickname=" << nickname << '\n';
    }
}

/// Закрывает лаунчер Rockstar, если о том просили настройки.
///
/// Одним местом на оба пути внедрения — обычный запуск и `--attach`: правило у
/// них одно, и разойтись ему нельзя.
void dismissRockstarLauncher(bool wanted) {
    if (!wanted) {
        return;
    }

    std::string note;

    // Успех и неуспех одинаково не беда: игра уже идёт, а лишний лаунчер
    // человек закроет и сам. В журнал они всё же уходят, и разными уровнями —
    // закрытие это то, чего игрок не просил у самой игры, и объяснить исчезнувшее
    // окно должно быть по чему.
    if (closeRockstarLauncher(note)) {
        spdlog::info("{}", note);
    } else {
        spdlog::debug("{}", note);
    }
}

} // namespace

std::unique_ptr<GameProcess> Session::run(const Settings& settings, const Reporter& report) {
    std::string error;

    // Настройки дублируются двумя путями. Переменные окружения работают, когда
    // игру запускаем мы: дочерний процесс их наследует. Файл нужен для --attach:
    // процесс уже запущен и окружения от нас не получал.
    if (!setEnvironment(L"OXYMP_SERVER", settings.server) ||
        !setEnvironment(L"OXYMP_NICKNAME", settings.nickname) ||
        !setEnvironment(L"OXYMP_NETGAME", settings.networkGameFake) ||
        !setEnvironment(L"OXYMP_SESSION",
                        settings.hostSession ? (settings.sessionRaw ? "raw" : "1") : "0")) {
        report(Progress::Failed, "Could not pass the settings to the game");
        return nullptr;
    }
    writeSessionFile(settings.server, settings.nickname);

    // Оконный режим правится до запуска игры: прочитав settings.xml однажды,
    // игра больше в него не заглядывает, и правка на ходу пропала бы впустую.
    //
    // Делается это и при --attach тоже, хоть и с опозданием на один запуск:
    // сейчас слой будет невиден, зато в следующий раз — виден.
    if (std::string note; !preferBorderlessWindow(settings.backupDirectory, note)) {
        // Не беда, из-за которой стоит не запускать игру: без правки останется
        // невидимым слой, а сама игра будет работать. Сказать об этом, впрочем,
        // нужно — иначе пропавший экран загрузки выглядит поломкой клиента.
        spdlog::warn("the game window mode was not changed: {}", note);
    } else {
        spdlog::info("game window mode: {}", note);
    }

    if (settings.attach) {
        report(Progress::Working, "Looking for a running game");

        auto game = GameProcess::attach(kProcessTimeout, error);
        if (!game) {
            report(Progress::Failed, error);
            return nullptr;
        }

        report(Progress::Working, "Game found, injecting the client");

        if (!game->injectWithRetries(settings.clientModule, kInjectTimeout, error)) {
            spdlog::error("{}: {}", text::kErrFailedToInject, error);

            report(Progress::Failed, text::kErrFailedToInject);
            return nullptr;
        }

        dismissRockstarLauncher(settings.closeRockstarLauncher);

        report(Progress::Ready, "Injected");
        return game;
    }

    report(Progress::Working, text::kCheckingPreconditions);

    // Вторую копию игра не поднимет: сессия Rockstar остаётся у первой, и новая
    // показывает «отсоединено». Молча запускать её — значит подсунуть
    // пользователю окно, которое выглядит сломанным без объяснений.
    if (std::string ignored; GameProcess::attach(std::chrono::seconds{0}, ignored)) {
        report(Progress::Failed, text::kGtavAlreadyRunning);
        return nullptr;
    }

    // Игру здесь не разыскивают. Путь к ней — настройка `gtapath`, спрошенная
    // один раз в окне установки; сюда он приходит готовым. Поиск при каждом
    // запуске был ошибкой: он повторял одну и ту же работу, зависел от чужих
    // файлов и не давал человеку ни поправить исход, ни узнать его заранее.
    if (settings.gameDirectory.empty()) {
        report(Progress::Failed,
               "GTA V folder is not set.\nRun oxymp.exe with no arguments to pick it.");
        return nullptr;
    }

    auto installed = gameInDirectory(settings.gameDirectory, error);
    if (!installed) {
        report(Progress::Failed, error);
        return nullptr;
    }

    // Площадка, выбранная человеком, сильнее признаков в каталоге: выбирал он
    // сам, и решать за него молча незачем.
    if (settings.gameStore.has_value()) {
        installed->store = *settings.gameStore;
    }

    spdlog::info("Game: {} ({})", installed->directory.string(), storeName(installed->store));

    // Что в каталоге игры мешает запуску — до всего остального, и это не
    // придирчивость. Чужой мод и пиратская копия проявляются не отказом, а
    // мигнувшим окном: игра закрывается, не дойдя до загрузки, и причины нет
    // нигде. Каждая строка в этом списке однажды стоила кому-то вечера.
    {
        const std::vector<GameConflicts::Found> conflicts =
            GameConflicts::inspect(installed->directory);

        for (const GameConflicts::Found& each : conflicts) {
            spdlog::log(each.weight == GameConflicts::Weight::Blocking ? spdlog::level::err
                                                                      : spdlog::level::warn,
                        "{} in the game folder ({}): {}", each.name, each.file.filename().string(),
                        each.advice);
        }

        if (GameConflicts::blocked(conflicts)) {
            std::string what;

            for (const GameConflicts::Found& each : conflicts) {
                if (each.weight != GameConflicts::Weight::Blocking) {
                    continue;
                }

                what += std::format("\n- {}: {}", each.name, each.advice);
            }

            report(Progress::Failed,
                   std::format("The game folder has something oxyMP cannot start with:{}", what));
            return nullptr;
        }
    }

    // Версия сверяется здесь, до всего остального, и это не придирка.
    //
    // Клиент опознаёт игру по байтам её кода, и на другой сборке эти байты
    // другие: он откажется работать уже внутри игры, а человек увидит запущенную
    // GTA без всякого oxyMP и без единого слова о причине — она останется в
    // журнале, куда он не полезет.
    //
    // Пустая версия — не повод отказывать: прочитать её могло не выйти по
    // причинам, к сборке отношения не имеющим, и тогда пусть решает клиент.
    if (!installed->version.empty() && installed->version != gamesig::kTargetGameVersion) {
        report(Progress::Failed,
               std::format("This is GTA V {}, but oxyMP is built for {}.\n\n"
                           "No setting can bridge that: the client recognises the game by "
                           "its code, and another build has other code.\n"
                           "Install that exact version, or build oxyMP for yours.",
                           installed->version, gamesig::kTargetGameVersion));
        return nullptr;
    }

    // Клиент площадки поднимается прежде всего остального. Копия из Steam
    // спрашивает права у steam_api64.dll, а та — у запущенного Steam: без него
    // игра закрывается, не дойдя до загрузки, и человек видит только мигнувшее
    // окно, а причину — нигде.
    if (installed->store != GameStore::Rockstar) {
        report(Progress::Working, std::format("Preparing {}", storeName(installed->store)));
    }

    if (!ensureStoreReady(installed->store, kStoreTimeout, error)) {
        report(Progress::Failed, error);
        return nullptr;
    }

    std::optional<GameLocation> location = installed;

    if (settings.standalone) {
        report(Progress::Working, text::kValidatingBackup);

        // Рядом с oxymp.exe, в его же папке backup: человек видит, где лежит его
        // копия игры, и удаляет её вместе с модом, а не разыскивает по системе.
        const std::filesystem::path preferred = settings.backupDirectory / "game";

        bool needsAdministrator = false;
        auto mirror = GameMirror::prepare(*installed, preferred, error, needsAdministrator);

        // Права спрашиваются отдельным коротким запуском, а не для всего лаунчера,
        // и это существенно, а не аккуратности ради.
        //
        // Лаунчер, запущенный с правами, запускает с ними и игру — права
        // наследуются. А в игре с правами администратора не поднимается Chromium:
        // интерфейс oxyMP просто не появляется. Проверено — именно так он и
        // пропал.
        //
        // Поэтому под правами делается только то, ради чего они нужны: ссылки на
        // файлы игры. Сама игра запускается обычным порядком.
        if (!mirror && needsAdministrator) {
            report(Progress::Working, "Administrator rights are needed - confirm the Windows prompt");

            if (!buildMirrorElevated(settings, error)) {
                report(Progress::Failed, error);
                return nullptr;
            }

            mirror = GameMirror::prepare(*installed, preferred, error, needsAdministrator);
        }

        if (!mirror) {
            report(Progress::Failed, error);
            return nullptr;
        }

        location = mirror->location;

        if (mirror->gameUpdated) {
            report(Progress::Working, "The game was updated - starting the pinned copy");
        }
    }

    // Права на игру выдаёт лаунчер Rockstar, и спрашивают их в первые же
    // мгновения после старта — на всех трёх площадках, а не только у себя.
    // Поэтому лаунчер поднимается до игры: иначе она закроется с
    // ERR_NO_LAUNCHER, не дойдя до загрузки.
    if (!ensureRockstarLauncherReady(kLauncherTimeout, error)) {
        report(Progress::Failed, error);
        return nullptr;
    }

    std::unique_ptr<GameProcess> game;

    if (settings.launchMode == LaunchMode::Direct) {
        // Прямой запуск нужен закреплённой копии: площадка запускает свою игру,
        // и сказать ей про другой файл нечем. Он же остаётся под рукой на
        // случай, когда путь через площадку почему-то не работает.
        report(Progress::Working, settings.standalone ? "Starting the pinned copy of the game"
                                                      : text::kStartingGtav);

        game = GameProcess::launchDirectly(*location,
                                           GameProcess::Arguments{
                                               .language = settings.gameLanguage,
                                               .straightIntoFreemode = settings.straightIntoFreemode,
                                           },
                                           error);
        if (!game) {
            report(Progress::Failed, std::string{text::kErrGameStart} + ": " + error);
            return nullptr;
        }
    } else {
        // Подмена ставится до просьбы запустить игру, а не после: площадка
        // создаёт процесс сразу, и опоздать здесь значит выпустить BattlEye.
        //
        // Она стоит и тогда, когда игру поднимает не лаунчер Rockstar. Довод
        // `-nobattleye` защиту и так не пускает, но у копии из Steam или Epic
        // командную строку составляет площадка, и второй рубеж здесь не роскошь.
        // Заодно им же доезжает до игры выбранный язык — там, где игру
        // запускает лаунчер Rockstar.
        report(Progress::Working, text::kInjectingLauncherPatches);

        auto patch = LauncherPatch::install(settings.launcherPatch,
                                            LauncherPatch::Order{
                                                .logDirectory = settings.logDirectory,
                                                .straightIntoFreemode = settings.straightIntoFreemode,
                                                .verbose = settings.verbose,
                                                .gameLanguage = settings.gameLanguage,
                                            },
                                            kPatchTimeout, error);
        if (!patch) {
            spdlog::error("{}: {}", text::kErrFailedToPatchLauncher, error);

            report(Progress::Failed, text::kErrFailedToPatchLauncher);
            return nullptr;
        }

        report(Progress::Working, text::kStartingGtav);

        if (!startGameThroughPlatform(*location, error)) {
            report(Progress::Failed, error);
            return nullptr;
        }

        // Процесс разыскивается по имени, а не берётся у подмены, и это главное
        // изменение против того, как было. Прежде номер приходил от перехвата
        // внутри Launcher.exe — и приходил только у копии Rockstar: игру из
        // Steam или Epic лаунчер не создаёт сам, перехват не срабатывает ни
        // разу, и лаунчер oxyMP молча ждал три минуты, пока рядом шла обычная
        // GTA V. По имени процесс находится у всех трёх; так же поступает alt:V.
        game = GameProcess::attach(kProcessTimeout, error);

        // Сработал перехват или нет — видно только здесь, и знать это нужно:
        // у копии из Steam или Epic он не срабатывает вовсе, и разбираться,
        // почему у игры оказался BattlEye, придётся по этой строке.
        spdlog::debug("the BattlEye link patch {}",
                      patch->startedGame() != 0 ? "started the game itself"
                                                : "never fired: the platform started the game");

        if (!game) {
            spdlog::error("the game process never appeared: {}", error);

            report(Progress::Failed,
                   std::format("{}\n\n{}\n- {}\n- {}", text::kErrGameStartTimeout,
                               text::kPossibleSolutions,
                               location->store == GameStore::Rockstar
                                   ? text::kSolRestartPlatformRgl
                                   : std::format(text::kSolRestartPlatform,
                                                 storeName(location->store)),
                               text::kSolStartGameOnce));
            return nullptr;
        }
    }

    report(Progress::Working, text::kLoadingClient);

    // Окна игры дожидаться нечего: при прямом запуске оно появляется много позже
    // готовности загрузчика модулей, а иногда не появляется вовсе. Единственный
    // надёжный признак готовности — удавшееся внедрение.
    if (!game->injectWithRetries(settings.clientModule, kInjectTimeout, error)) {
        spdlog::error("{}: {}", text::kErrFailedToInject, error);

        report(Progress::Failed, text::kErrFailedToInject);
        return nullptr;
    }

    // Лаунчер Rockstar закрывается здесь, а не раньше: до удавшегося внедрения
    // он ещё может понадобиться — игра спрашивает у него права по именованному
    // каналу, и спрашивает при запуске. Внедрились — значит спросила и получила.
    dismissRockstarLauncher(settings.closeRockstarLauncher);

    // Слой поднимается сразу после внедрения, а не по готовности игры: смысл
    // его в том, чтобы заслонить собой заставку и страницу выбора режима, а они
    // начнутся через считанные секунды.
    report(Progress::Ready, "Injected, waiting for the game to load");
    return game;
}

} // namespace oxymp::launcher
