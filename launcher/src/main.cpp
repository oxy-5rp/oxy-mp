// oxymp — лаунчер.
//
// Показывает окно подключения, запускает игру и внедряет клиентский модуль.
// В каталог игры ничего не записывается: все файлы проекта лежат рядом с этим
// исполняемым файлом, а настройки уезжают в игру переменными окружения.

#include "game_choice.hpp"
#include "game_locator.hpp"
#include "game_mirror.hpp"
#include "launcher_window.hpp"
#include "paths.hpp"
#include "session.hpp"
#include "setup_window.hpp"

#include <oxymp/config/settings.hpp>
#include <oxymp/shared/protocol/protocol_version.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

namespace {

void printUsage() {
    std::cerr << "Usage:\n"
                 "  oxymp [--server <address:port>] [--nickname <name>]\n"
                 "        [--game <game folder>] [--client <module path>]\n"
                 "        [--setup] [--direct | --attach | --standalone] [--freemode]\n"
                 "        [--netgame | --netgame-full] [--no-session | --session-raw]\n"
                 "        [--no-ui]\n\n"
                 "  (default)  show the launcher window and start the game the same way\n"
                 "             alt:V does: the platform the game was bought on is asked to\n"
                 "             start it (Rockstar Games Launcher, Steam or Epic Games Store),\n"
                 "             BattlEye is kept out with -nobattleye, and GTA5.exe is then\n"
                 "             found by name. The server is not named here - it is picked in\n"
                 "             the in-game menu, F1.\n"
                 "  --setup    ask for the GTA V folder and the platform again, even if they\n"
                 "             are already written in oxymp.toml.\n"
                 "  --server   go straight to the named server instead of opening the menu.\n"
                 "             For scripts and tests, where nobody is there to press a key.\n"
                 "  --no-ui    do not show the window, just start - for scripts.\n"
                 "  --direct   start GTA5.exe ourselves instead of asking the platform. The\n"
                 "             platform then does not know the game is running. Kept for the\n"
                 "             pinned copy and for when the normal path misbehaves.\n"
                 "  --attach   do not start the game, inject into a running GTA5.exe.\n"
                 "  --standalone  run a pinned copy of the game instead of the installed one.\n"
                 "             GTA5.exe is pinned inside the oxymp-backup folder, the rest is\n"
                 "             hard-linked and takes no space. The point is that a Rockstar\n"
                 "             update changes the installed game, while the pinned one stays\n"
                 "             the build the signatures were written for.\n"
                 "  --freemode start the game straight into online freemode, skipping the\n"
                 "             story. The road oxyMP takes once it can hand the game a\n"
                 "             session. Until then the game sits on the GTA Online loading\n"
                 "             screen forever.\n"
                 "  --netgame  fake the 'network game is up' flag for the game. Recon: pause,\n"
                 "             map, population and the order of death all branch off it, but\n"
                 "             the session objects behind it are missing, and a crash is the\n"
                 "             expected outcome. What changed is in the client log.\n"
                 "  --netgame-full  the same plus the intermediate 'connecting' state. That\n"
                 "             flag sits closer to the session state machine and is stricter.\n"
                 "  --no-session  do not ask the game for a network session. Without one the\n"
                 "             game is single player: the fake flag alone gives the branches\n"
                 "             but not the session objects behind them, and it crashes.\n"
                 "  --session-raw  bring the session up without holding network_bail. The\n"
                 "             game then leaves it within a second - for measuring, not\n"
                 "             for playing.\n";
}

/// Что лаунчер берёт из настроек рядом с собой.
///
/// Читается один раз: файл общий с меню внутри игры, и второе его чтение было бы
/// вторым местом, где решают, что в нём написано.
struct LauncherSettings {
    /// На каком языке просить игру запуститься: `ru-RU`, `en-US`.
    ///
    /// Пусто, если настройка не заполнена, — тогда язык остаётся тем, что
    /// назначил лаунчер Rockstar.
    std::wstring gameLanguage;

    /// Писать ли в журнал подробности. Настройка `debug`, та же, что у alt:V.
    bool verbose = false;

    /// Закрывать ли Rockstar Games Launcher после удавшегося внедрения.
    bool closeRockstarLauncher = true;
};

LauncherSettings readLauncherSettings(const oxymp::launcher::Paths& paths) {
    const oxymp::config::Settings settings =
        oxymp::config::Settings::load(paths.root / "oxymp.toml");

    LauncherSettings chosen;
    chosen.verbose = settings.flag("debug");
    chosen.closeRockstarLauncher = settings.flag("closeRockstarLauncher");

    const std::string language = settings.text("gameLanguage");

    // Название языка — латиница и дефис (`ru-RU`), так что расширение до
    // широких знаков посимвольно здесь честно и не портит ничего.
    chosen.gameLanguage = std::wstring{language.begin(), language.end()};

    return chosen;
}

} // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    const oxymp::launcher::Paths paths = oxymp::launcher::Paths::beside();
    paths.ensure();

    oxymp::launcher::Session::Settings settings;

    // Адреса по умолчанию больше нет, и это не упущение. Клиент без адреса
    // никуда не идёт сам: он поднимает меню и ждёт, пока сервер выберет игрок, —
    // как alt:V. Адрес в ключе `--server` остаётся для запуска из скрипта и для
    // проверок, где меню нажимать некому.
    settings.nickname = "player";
    settings.clientModule = paths.clientModule();
    settings.launcherPatch = paths.launcherPatch();
    settings.logDirectory = paths.logs();
    settings.backupDirectory = paths.backup();

    // Язык игры берётся из тех же настроек, что правит меню внутри игры, — и
    // читает их лаунчер, а не клиент. Иначе и нельзя: игре язык называют при
    // запуске, а к тому мгновению клиента ещё нет.
    const LauncherSettings chosen = readLauncherSettings(paths);

    settings.gameLanguage = chosen.gameLanguage;
    settings.verbose = chosen.verbose;
    settings.closeRockstarLauncher = chosen.closeRockstarLauncher;

    // Подробности в журнале включаются настройкой, а не сборкой, и это не
    // мелочь: строки уровня `debug` в коде были всегда, а увидеть их не мог
    // никто — уровень не выставлялся вовсе. Разбираться в чужой поломке было
    // не по чему.
    spdlog::set_level(chosen.verbose ? spdlog::level::debug : spdlog::level::info);

    bool showWindow = true;
    bool prepareMirrorOnly = false;
    bool askAgain = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const bool hasValue = i + 1 < argc;

        if (argument == "--server" && hasValue) {
            settings.server = argv[++i];
        } else if (argument == "--nickname" && hasValue) {
            settings.nickname = argv[++i];
        } else if (argument == "--game" && hasValue) {
            settings.gameDirectory = argv[++i];
        } else if (argument == "--client" && hasValue) {
            settings.clientModule = argv[++i];
        } else if (argument == "--freemode") {
            settings.straightIntoFreemode = true;
        } else if (argument == "--no-session") {
            settings.hostSession = false;
        } else if (argument == "--session-raw") {
            settings.hostSession = true;
            settings.sessionRaw = true;
        } else if (argument == "--netgame") {
            settings.networkGameFake = "1";
        } else if (argument == "--netgame-full") {
            settings.networkGameFake = "full";
        } else if (argument == "--direct") {
            settings.launchMode = oxymp::launcher::LaunchMode::Direct;
        } else if (argument == "--setup") {
            askAgain = true;
        } else if (argument == "--standalone") {
            // Прямой запуск включается заодно, а не требуется отдельным ключом:
            // закреплённую копию поднять больше нечем, и заставлять человека
            // помнить об этом значило бы разложить одно решение на два ключа.
            settings.standalone = true;
            settings.launchMode = oxymp::launcher::LaunchMode::Direct;
        } else if (argument == "--attach") {
            settings.attach = true;
        } else if (argument == "--prepare-mirror") {
            // Внутренний ключ, в подсказке его нет. Им лаунчер зовёт сам себя с
            // правами администратора: собрать копию игры и сразу закончиться, не
            // запуская ничего. Права нужны только на это.
            prepareMirrorOnly = true;
            showWindow = false;
        } else if (argument == "--no-ui") {
            showWindow = false;
        } else {
            printUsage();
            return 2;
        }
    }

    if (!showWindow) {
        // Лаунчер объявлен оконным, и своей консоли у него нет вовсе — чёрное
        // окно больше не мигает при запуске. Но запущенный из командной строки
        // он обязан в неё же и говорить, иначе вывод пропадает бесследно.
        //
        // Подключаемся к консоли того, кто нас позвал. Позвали не из консоли —
        // подключаться не к чему, и это не ошибка.
        if (::AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
            FILE* stream = nullptr;
            ::freopen_s(&stream, "CONOUT$", "w", stdout);
            ::freopen_s(&stream, "CONOUT$", "w", stderr);
        }
    } else {
        // Журнал уходит в файл: окна консоли у оконного приложения нет, а
        // разбираться в том, что пошло не так у игрока, по чему-то надо.
        //
        // Заводится до окна установки, а не после: оно тоже пишет в журнал, и
        // потерять его записи значило бы остаться без объяснений ровно там, где
        // они нужнее всего — при первом знакомстве.
        try {
            auto logger = spdlog::basic_logger_mt("launcher",
                                                  (paths.logs() / "launcher.log").string(), true);
            logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
            logger->flush_on(spdlog::level::debug);
            logger->set_level(chosen.verbose ? spdlog::level::debug : spdlog::level::info);
            spdlog::set_default_logger(std::move(logger));
        } catch (const spdlog::spdlog_ex&) {
            // Без журнала лаунчер работает, падать из-за него — нельзя.
        }
    }

    // Где лежит игра, спрашивается один раз и записывается в oxymp.toml. Поиск
    // при каждом запуске был ошибкой: одна и та же работа повторялась, исход её
    // зависел от чужих файлов, и человек не мог ни поправить его, ни узнать
    // заранее. Порядок здесь такой:
    //
    //   --game       сказано прямо, значит спорить не с чем;
    //   oxymp.toml   уже выбрано однажды;
    //   окно         не выбрано ещё ни разу — спрашиваем и записываем.
    if (settings.gameDirectory.empty()) {
        std::optional<oxymp::launcher::GameLocation> game;

        if (!askAgain) {
            game = oxymp::launcher::chosenGame(paths.root / "oxymp.toml");
        }

        if (!game) {
            if (showWindow) {
                game = oxymp::launcher::SetupWindow::ask(paths);
            } else {
                // Без окна спросить некого, и единственное, что остаётся, — найти
                // игру самим. Найденное сразу же записывается: следующий запуск
                // искать уже не будет.
                std::string error;

                game = oxymp::launcher::locateGame(error);
                if (!game) {
                    spdlog::error("{}", error);
                    spdlog::error("Pass --game <folder>, or run oxymp.exe with no arguments "
                                  "to pick it");
                    return 1;
                }

                oxymp::launcher::rememberGame(paths.root / "oxymp.toml", *game);
            }
        }

        if (!game) {
            // Окно установки закрыли, ничего не выбрав. Запускать нечего, и это
            // не поломка: человек передумал.
            return 1;
        }

        settings.gameDirectory = game->directory;
        settings.gameStore = game->store;
    }

    if (prepareMirrorOnly) {
        // Этот запуск идёт с правами администратора и не делает ничего, кроме
        // сборки копии. Ни игры, ни сети, ни окна: чем меньше сделано с правами,
        // тем меньше их достанется тому, кому они не нужны.
        std::string error;

        const auto installed = oxymp::launcher::gameInDirectory(settings.gameDirectory, error);
        if (!installed) {
            spdlog::error("{}", error);
            return 1;
        }

        bool needsAdministrator = false;

        const auto mirror = oxymp::launcher::GameMirror::prepare(
            *installed, settings.backupDirectory / "game", error, needsAdministrator);

        if (!mirror) {
            spdlog::error("{}", error);
            return 1;
        }

        spdlog::info("Game copy ready: {}", mirror->location.directory.string());
        return 0;
    }

    if (showWindow) {
        return oxymp::launcher::LauncherWindow::run(paths, std::move(settings));
    }

    if (settings.server.empty()) {
        spdlog::info("No server given: the client will open the menu and wait for a choice");
    } else {
        spdlog::info("Server: {}, name: {}", settings.server, settings.nickname);
    }

    auto game = oxymp::launcher::Session::run(
        settings, [](oxymp::launcher::Progress progress, std::string_view text) {
            if (progress == oxymp::launcher::Progress::Failed) {
                spdlog::error("{}", text);
            } else {
                spdlog::info("{}", text);
            }
        });

    if (game == nullptr) {
        return 1;
    }

    spdlog::info("Client log: logs\\client_<date>.log next to this file");

    game->waitForExit();
    spdlog::info("The game has exited");

    return 0;
}
