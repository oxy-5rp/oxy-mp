#include "session.hpp"

#include "game_locator.hpp"
#include "game_mirror.hpp"
#include "game_settings.hpp"
#include "game_store.hpp"
#include "launcher_patch.hpp"
#include "rockstar_launcher.hpp"

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
        error = "не удалось узнать собственный путь";
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
                    ? "Без прав администратора свою копию игры не собрать.\n"
                      "Либо подтвердите запрос Windows, либо запускайте без --standalone."
                    : "не удалось запросить права администратора";
        return false;
    }

    ::WaitForSingleObject(request.hProcess, INFINITE);

    DWORD code = 1;
    ::GetExitCodeProcess(request.hProcess, &code);
    ::CloseHandle(request.hProcess);

    if (code != 0) {
        error = "сборка своей копии игры не удалась — подробности в журнале лаунчера";
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
        report(Progress::Failed, "Не удалось передать настройки игре");
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
        report(Progress::Working, "Ищем запущенную игру");

        auto game = GameProcess::attach(kProcessTimeout, error);
        if (!game) {
            report(Progress::Failed, error);
            return nullptr;
        }

        report(Progress::Working, "Игра найдена, внедряем модуль");

        if (!game->injectWithRetries(settings.clientModule, kInjectTimeout, error)) {
            report(Progress::Failed, "Не удалось внедрить модуль: " + error);
            return nullptr;
        }

        report(Progress::Ready, "Модуль внедрён, игра ваша");
        return game;
    }

    // Вторую копию игра не поднимет: сессия Rockstar остаётся у первой, и новая
    // показывает «отсоединено». Молча запускать её — значит подсунуть
    // пользователю окно, которое выглядит сломанным без объяснений.
    if (std::string ignored; GameProcess::attach(std::chrono::seconds{0}, ignored)) {
        report(Progress::Failed,
               "GTA5.exe уже запущена.\nЗакройте её и попробуйте снова.");
        return nullptr;
    }

    report(Progress::Working, "Ищем установленную игру");

    const auto installed = settings.gameDirectory.empty()
                               ? locateGame(error)
                               : gameInDirectory(settings.gameDirectory, error);
    if (!installed) {
        report(Progress::Failed, error);
        return nullptr;
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
               std::format("У вас GTA V версии {}, а oxyMP собран под {}.\n\n"
                           "Это не поправимо настройками: клиент узнаёт игру по её коду, "
                           "а у другой сборки код другой.\n"
                           "Нужна ровно та версия — либо новая сборка oxyMP под вашу.",
                           installed->version, gamesig::kTargetGameVersion));
        return nullptr;
    }

    // Клиент площадки поднимается прежде всего остального. Копия из Steam
    // спрашивает права у steam_api64.dll, а та — у запущенного Steam: без него
    // игра закрывается, не дойдя до загрузки, и человек видит только мигнувшее
    // окно, а причину — нигде.
    if (installed->store != GameStore::Rockstar) {
        report(Progress::Working, std::format("Готовим {}", storeName(installed->store)));
    }

    if (!ensureStoreReady(installed->store, kStoreTimeout, error)) {
        report(Progress::Failed, error);
        return nullptr;
    }

    std::optional<GameLocation> location = installed;

    if (settings.standalone) {
        report(Progress::Working, "Готовим свою копию игры");

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
            report(Progress::Working, "Нужны права администратора — подтвердите запрос Windows");

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
            report(Progress::Working, "Игра обновилась — запускаем закреплённую копию");
        }
    }

    // Права на игру выдаёт лаунчер Rockstar, и спрашивают их в первые же
    // мгновения после старта. Поэтому лаунчер поднимается до игры: иначе она
    // закроется с ERR_NO_LAUNCHER, не дойдя до загрузки.
    report(Progress::Working, "Готовим Rockstar Games Launcher");

    if (!ensureRockstarLauncherReady(kLauncherTimeout, error)) {
        report(Progress::Failed, error);
        return nullptr;
    }

    std::unique_ptr<GameProcess> game;

    // Закреплённую копию поднять может только прямой запуск: лаунчер Rockstar
    // запускает свою игру, и сказать ему про другой файл нечем. Ключ --standalone
    // поэтому и включает прямой запуск — здесь это лишь соблюдается.
    if (settings.launchMode == LaunchMode::Direct) {
        report(Progress::Working, settings.standalone ? "Запускаем свою копию игры"
                                                      : "Запускаем игру напрямую");

        game = GameProcess::launchDirectly(*location, error);
        if (!game) {
            report(Progress::Failed, error);
            return nullptr;
        }
    } else {
        // Подмена ставится до просьбы запустить игру, а не после: лаунчер
        // создаёт процесс сразу, и опоздать здесь значит выпустить BattlEye.
        report(Progress::Working, "Убираем BattlEye из запуска");

        auto patch = LauncherPatch::install(settings.launcherPatch,
                                            LauncherPatch::Order{
                                                .logDirectory = settings.logDirectory,
                                                .straightIntoFreemode = settings.straightIntoFreemode,
                                                .gameLanguage = settings.gameLanguage,
                                            },
                                            kPatchTimeout, error);
        if (!patch) {
            report(Progress::Failed, error);
            return nullptr;
        }

        report(Progress::Working, "Просим Rockstar Games Launcher запустить игру");

        if (!startGameThroughLauncher(*location, error)) {
            report(Progress::Failed, error);
            return nullptr;
        }

        const std::uint32_t started = patch->waitForGame(kProcessTimeout, error);
        if (started == 0) {
            report(Progress::Failed, error);
            return nullptr;
        }

        game = GameProcess::attachTo(started, error);
        if (!game) {
            report(Progress::Failed, error);
            return nullptr;
        }
    }

    report(Progress::Working, "Игра запущена, внедряем модуль");

    // Окна игры дожидаться нечего: при прямом запуске оно появляется много позже
    // готовности загрузчика модулей, а иногда не появляется вовсе. Единственный
    // надёжный признак готовности — удавшееся внедрение.
    if (!game->injectWithRetries(settings.clientModule, kInjectTimeout, error)) {
        report(Progress::Failed, "Не удалось внедрить модуль: " + error);
        return nullptr;
    }

    // Слой поднимается сразу после внедрения, а не по готовности игры: смысл
    // его в том, чтобы заслонить собой заставку и страницу выбора режима, а они
    // начнутся через считанные секунды.
    report(Progress::Ready, "Модуль внедрён, ждём загрузки игры");
    return game;
}

} // namespace oxymp::launcher
