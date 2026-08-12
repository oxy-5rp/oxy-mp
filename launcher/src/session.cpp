#include "session.hpp"

#include "game_locator.hpp"
#include "game_settings.hpp"
#include "launcher_patch.hpp"
#include "rockstar_launcher.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Сколько ждать появления процесса игры после запуска через лаунчер Rockstar.
constexpr auto kProcessTimeout = std::chrono::seconds{180};

/// Сколько добиваться внедрения. Сразу после запуска процесс занят собой и
/// удалённый поток в нём создать не удаётся.
constexpr auto kInjectTimeout = std::chrono::seconds{30};

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

/// Кладёт настройки сессии в файл рядом с журналом клиента.
///
/// Единственный способ передать адрес сервера и имя в уже запущенный процесс
/// (режим --attach): окружения он от нас не получал.
void writeSessionFile(const std::string& server, const std::string& nickname) {
    std::wstring localAppData(MAX_PATH, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(),
                                                    static_cast<DWORD>(localAppData.size()));
    if (written == 0 || written >= localAppData.size()) {
        return;
    }
    localAppData.resize(written);

    const std::filesystem::path path =
        std::filesystem::path{localAppData} / "oxyMP" / "session.cfg";

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
        spdlog::warn("оконный режим игры не изменён: {}", note);
    } else {
        spdlog::info("оконный режим игры: {}", note);
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

    const auto location = settings.gameDirectory.empty()
                              ? locateGame(error)
                              : gameInDirectory(settings.gameDirectory, error);
    if (!location) {
        report(Progress::Failed, error);
        return nullptr;
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

    if (settings.launchMode == LaunchMode::Direct) {
        report(Progress::Working, "Запускаем игру напрямую");

        game = GameProcess::launchDirectly(*location, error);
        if (!game) {
            report(Progress::Failed, error);
            return nullptr;
        }
    } else {
        // Подмена ставится до просьбы запустить игру, а не после: лаунчер
        // создаёт процесс сразу, и опоздать здесь значит выпустить BattlEye.
        report(Progress::Working, "Убираем BattlEye из запуска");

        auto patch = LauncherPatch::install(settings.launcherPatch, settings.logDirectory,
                                            settings.straightIntoFreemode, kPatchTimeout, error);
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
