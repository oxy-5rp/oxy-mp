// oxymp — лаунчер.
//
// Показывает окно подключения, запускает игру и внедряет клиентский модуль.
// В каталог игры ничего не записывается: все файлы проекта лежат рядом с этим
// исполняемым файлом, а настройки уезжают в игру переменными окружения.

#include "connect_window.hpp"
#include "game_locator.hpp"
#include "game_mirror.hpp"
#include "paths.hpp"
#include "session.hpp"

#include <oxymp/shared/protocol/protocol_version.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include <windows.h>

namespace {

void printUsage() {
    std::cerr << "Использование:\n"
                 "  oxymp [--server <адрес:порт>] [--nickname <имя>]\n"
                 "        [--game <каталог игры>] [--client <путь к модулю>]\n"
                 "        [--direct | --attach | --standalone] [--freemode]\n"
                 "        [--netgame | --netgame-full] [--session] [--no-ui]\n\n"
                 "  (по умолчанию) показать окно подключения. Игра запускается через\n"
                 "             Rockstar Games Launcher, но вместо GTA5_BE.exe тот поднимает\n"
                 "             сразу GTA5.exe: игра числится запущенной, BattlEye не встаёт.\n"
                 "  --no-ui    не показывать окно, запустить сразу — для скриптов.\n"
                 "  --direct   запустить GTA5.exe напрямую, без лаунчера Rockstar. BattlEye\n"
                 "             тоже не встаёт, но лаунчер об игре не знает и не показывает\n"
                 "             её запущенной. Запасной путь на случай поломки основного.\n"
                 "  --attach   не запускать игру, а внедриться в уже запущенный GTA5.exe.\n"
                 "  --standalone  запускать свою копию игры вместо установленной. GTA5.exe\n"
                 "             закрепляется в папке oxymp-backup внутри игры, остальное\n"
                 "             связывается жёсткими ссылками и места не занимает. Смысл в\n"
                 "             том, что обновление Rockstar меняет установленную игру, а\n"
                 "             закреплённая остаётся той, под которую написаны сигнатуры.\n"
                 "             Включает прямой запуск: закреплённую копию поднять нечем\n"
                 "             больше.\n"
                 "  --freemode вести игру сразу в сетевой свободный режим, минуя сюжет.\n"
                 "             Путь, которым oxyMP пойдёт, когда научится подставлять игре\n"
                 "             сессию. Пока не научился, игра по нему остаётся на вечной\n"
                 "             загрузке GTA Online.\n"
                 "  --netgame  подделать игре признак установившейся сетевой игры. Разведка:\n"
                 "             от него ветвятся пауза, карта, население и порядок смерти,\n"
                 "             но объектов сессии за ним нет, и вылет — ожидаемый исход.\n"
                 "             Что изменилось, видно в журнале клиента.\n"
                 "  --netgame-full  то же плюс промежуточное «подключаемся». Этот признак\n"
                 "             ближе к машине состояний сессии, и спрос с него строже.\n"
                 "  --session  попросить игру поднять настоящую сетевую сессию её же\n"
                 "             функцией. В отличие от --netgame признак выставит сама игра,\n"
                 "             вместе с объектами сессии — тем, чего подделке не хватало.\n"
                 "             Вызов пока вслепую: что игра сделает без живого слоя\n"
                 "             Rockstar Online, заранее не известно.\n";
}

} // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    const oxymp::launcher::Paths paths = oxymp::launcher::Paths::beside();
    paths.ensure();

    oxymp::launcher::Session::Settings settings;
    settings.server = "127.0.0.1:" + std::to_string(oxymp::shared::kDefaultServerPort);
    settings.nickname = "player";
    settings.clientModule = paths.clientModule();
    settings.launcherPatch = paths.launcherPatch();
    settings.logDirectory = paths.logs();
    settings.backupDirectory = paths.backup();

    bool showWindow = true;
    bool prepareMirrorOnly = false;

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

    if (prepareMirrorOnly) {
        // Этот запуск идёт с правами администратора и не делает ничего, кроме
        // сборки копии. Ни игры, ни сети, ни окна: чем меньше сделано с правами,
        // тем меньше их достанется тому, кому они не нужны.
        std::string error;

        const auto installed = settings.gameDirectory.empty()
                                   ? oxymp::launcher::locateGame(error)
                                   : oxymp::launcher::gameInDirectory(settings.gameDirectory,
                                                                      error);
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

        spdlog::info("копия игры готова: {}", mirror->location.directory.string());
        return 0;
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
    }

    if (showWindow) {
        // Журнал уходит в файл: окна консоли у оконного приложения нет, а
        // разбираться в том, что пошло не так у игрока, по чему-то надо.
        try {
            auto logger = spdlog::basic_logger_mt("launcher",
                                                  (paths.logs() / "launcher.log").string(), true);
            logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
            logger->flush_on(spdlog::level::debug);
            spdlog::set_default_logger(std::move(logger));
        } catch (const spdlog::spdlog_ex&) {
            // Без журнала лаунчер работает, падать из-за него — нельзя.
        }

        return oxymp::launcher::ConnectWindow::run(paths, std::move(settings));
    }

    spdlog::info("сервер: {}, имя: {}", settings.server, settings.nickname);

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

    spdlog::info("журнал клиента: %LOCALAPPDATA%\\oxyMP\\logs\\client.log");

    game->waitForExit();
    spdlog::info("игра завершилась");

    return 0;
}
