// oxymp — лаунчер.
//
// Находит установленную игру, запускает её и внедряет клиентский модуль.
// В каталог игры ничего не записывается: все файлы проекта лежат рядом с этим
// исполняемым файлом, а настройки уезжают в игру переменными окружения.

#include "game_locator.hpp"
#include "game_process.hpp"

#include <oxymp/shared/protocol/protocol_version.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <windows.h>

namespace {

/// Имя модуля, который внедряется в игру.
constexpr const char* kClientModuleName = "oxymp-client.dll";

/// Сколько ждать появления окна игры.
constexpr auto kWindowTimeout = std::chrono::seconds{180};

/// Сколько ждать появления процесса игры после запуска через лаунчер Rockstar.
constexpr auto kProcessTimeout = std::chrono::seconds{180};

/// Сколько добиваться внедрения при прямом запуске.
constexpr auto kInjectTimeout = std::chrono::seconds{30};

void printUsage() {
    std::cerr << "Использование:\n"
                 "  oxymp [--server <адрес:порт>] [--nickname <имя>]\n"
                 "        [--game <каталог игры>] [--client <путь к модулю>]\n"
                 "        [--direct | --attach]\n\n"
                 "  (по умолчанию) запустить игру через Rockstar Games Launcher и внедрить\n"
                 "             модуль. Игра работает, но вместе с ней поднимается BattlEye.\n"
                 "  --direct   запустить GTA5.exe напрямую, минуя лаунчер Rockstar.\n"
                 "             Играть так нельзя: игра закрывается с ERR_NO_LAUNCHER.\n"
                 "  --attach   не запускать игру, а внедриться в уже запущенный GTA5.exe.\n"
                 "             Как именно игра запущена и остаётся живой — решает пользователь;\n"
                 "             oxyMP только вносит в неё мультиплеер.\n";
}

/// Режим работы лаунчера.
enum class Mode {
    Launch, ///< Запустить игру самим (через лаунчер Rockstar или напрямую).
    Attach, ///< Внедриться в уже запущенный процесс игры.
};

/// Каталог, в котором лежит этот исполняемый файл.
///
/// Модуль ищется рядом с ним, а не в текущем каталоге: лаунчер запускают
/// откуда угодно, в том числе ярлыком.
std::filesystem::path executableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return std::filesystem::current_path();
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }

        buffer.resize(buffer.size() * 2);
    }

    return std::filesystem::path{buffer}.parent_path();
}

/// Кладёт настройки сессии в файл рядом с журналом клиента.
///
/// Единственный способ передать адрес сервера и имя в уже запущенный процесс
/// (режим --attach): окружения он от нас не получал. Пишется в наш каталог, а
/// не в папку игры.
void writeSessionFile(const std::string& server, const std::string& nickname) {
    const std::string localAppData = [] {
        DWORD needed = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (needed == 0) {
            return std::string{};
        }
        std::wstring wide(needed, L'\0');
        needed = ::GetEnvironmentVariableW(L"LOCALAPPDATA", wide.data(), needed);
        wide.resize(needed);
        return std::filesystem::path{wide}.string();
    }();

    if (localAppData.empty()) {
        return;
    }

    const std::filesystem::path path = std::filesystem::path{localAppData} / "oxyMP" / "session.cfg";

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) {
        file << "server=" << server << '\n' << "nickname=" << nickname << '\n';
    }
}

bool setEnvironment(const char* name, const std::string& value) {
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(),
                          size);

    const std::wstring wideName{name, name + std::char_traits<char>::length(name)};

    return ::SetEnvironmentVariableW(wideName.c_str(), wide.c_str()) != 0;
}

} // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    std::string server = "127.0.0.1:" + std::to_string(oxymp::shared::kDefaultServerPort);
    std::string nickname = "player";
    std::filesystem::path gameDirectory;
    std::filesystem::path clientModule;
    auto launchMode = oxymp::launcher::LaunchMode::ViaRockstarLauncher;
    Mode mode = Mode::Launch;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const bool hasValue = i + 1 < argc;

        if (argument == "--server" && hasValue) {
            server = argv[++i];
        } else if (argument == "--nickname" && hasValue) {
            nickname = argv[++i];
        } else if (argument == "--game" && hasValue) {
            gameDirectory = argv[++i];
        } else if (argument == "--client" && hasValue) {
            clientModule = argv[++i];
        } else if (argument == "--direct") {
            launchMode = oxymp::launcher::LaunchMode::Direct;
        } else if (argument == "--attach") {
            mode = Mode::Attach;
        } else {
            printUsage();
            return 2;
        }
    }

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    if (clientModule.empty()) {
        clientModule = executableDirectory() / kClientModuleName;
    }

    spdlog::info("сервер: {}, имя: {}", server, nickname);

    // Настройки дублируются двумя путями. Переменные окружения работают, когда
    // игру запускаем мы: дочерний процесс их наследует. Файл рядом с журналом
    // нужен для --attach: процесс уже запущен и окружения от нас не получал.
    if (!setEnvironment("OXYMP_SERVER", server) || !setEnvironment("OXYMP_NICKNAME", nickname)) {
        spdlog::error("не удалось передать настройки игре");
        return 1;
    }
    writeSessionFile(server, nickname);

    std::string error;
    std::unique_ptr<oxymp::launcher::GameProcess> game;

    if (mode == Mode::Attach) {
        spdlog::info("ищем запущенную игру");

        game = oxymp::launcher::GameProcess::attach(kProcessTimeout, error);
        if (!game) {
            spdlog::error("{}", error);
            return 1;
        }

        spdlog::info("игра найдена, идентификатор процесса {}, внедряем модуль", game->id());

        if (!game->injectWithRetries(clientModule, kInjectTimeout, error)) {
            spdlog::error("не удалось внедрить модуль: {}", error);
            return 1;
        }
    } else {
        const auto location = gameDirectory.empty()
                                  ? oxymp::launcher::locateGame(error)
                                  : oxymp::launcher::gameInDirectory(gameDirectory, error);
        if (!location) {
            spdlog::error("{}", error);
            return 1;
        }

        spdlog::info("игра: {}{}", location->directory.string(),
                     location->version.empty() ? "" : " версии " + location->version);
        spdlog::info("запуск {}", launchMode == oxymp::launcher::LaunchMode::Direct
                                     ? "напрямую (игра закроется с ERR_NO_LAUNCHER)"
                                     : "через Rockstar Games Launcher");

        game = oxymp::launcher::GameProcess::launch(*location, launchMode, kProcessTimeout, error);
        if (!game) {
            spdlog::error("{}", error);
            return 1;
        }

        spdlog::info("игра запущена, идентификатор процесса {}", game->id());

        if (launchMode == oxymp::launcher::LaunchMode::Direct) {
            // Окна при прямом запуске может не появиться вовсе, поэтому
            // единственный надёжный признак готовности — удавшееся внедрение.
            spdlog::info("внедряем модуль");

            if (!game->injectWithRetries(clientModule, kInjectTimeout, error)) {
                spdlog::error("не удалось внедрить модуль: {}", error);
                return 1;
            }
        } else {
            spdlog::info("ждём появления окна игры");

            if (!game->waitUntilWindowAppears(kWindowTimeout)) {
                spdlog::error("окно игры так и не появилось — внедрять модуль небезопасно");
                return 1;
            }

            if (!game->inject(clientModule, error)) {
                spdlog::error("не удалось внедрить модуль: {}", error);
                return 1;
            }
        }
    }

    spdlog::info("модуль внедрён: {}", clientModule.filename().string());
    spdlog::info("журнал клиента: %LOCALAPPDATA%\\oxyMP\\logs\\client.log");

    game->waitForExit();
    spdlog::info("игра завершилась");

    return 0;
}
