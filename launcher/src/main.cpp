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
#include <iostream>
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
                 "        [--direct]\n\n"
                 "  --direct   запустить GTA5.exe напрямую, минуя лаунчер Rockstar.\n"
                 "             Играть так нельзя: игра закрывается с ERR_NO_LAUNCHER.\n"
                 "             Режим оставлен для опытов над процессом игры.\n";
}

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
    auto mode = oxymp::launcher::LaunchMode::ViaRockstarLauncher;

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
            mode = oxymp::launcher::LaunchMode::Direct;
        } else {
            printUsage();
            return 2;
        }
    }

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    if (clientModule.empty()) {
        clientModule = executableDirectory() / kClientModuleName;
    }

    std::string error;
    const auto location = gameDirectory.empty()
                              ? oxymp::launcher::locateGame(error)
                              : oxymp::launcher::gameInDirectory(gameDirectory, error);
    if (!location) {
        spdlog::error("{}", error);
        return 1;
    }

    spdlog::info("игра: {}{}", location->directory.string(),
                 location->version.empty() ? "" : " версии " + location->version);
    spdlog::info("сервер: {}, имя: {}", server, nickname);

    if (!setEnvironment("OXYMP_SERVER", server) || !setEnvironment("OXYMP_NICKNAME", nickname)) {
        spdlog::error("не удалось передать настройки игре");
        return 1;
    }

    spdlog::info("запуск {}", mode == oxymp::launcher::LaunchMode::Direct
                                 ? "напрямую (игра закроется с ERR_NO_LAUNCHER)"
                                 : "через Rockstar Games Launcher");

    const auto game =
        oxymp::launcher::GameProcess::launch(*location, mode, kProcessTimeout, error);
    if (!game) {
        spdlog::error("{}", error);
        return 1;
    }

    spdlog::info("игра запущена, идентификатор процесса {}", game->id());

    if (mode == oxymp::launcher::LaunchMode::Direct) {
        // Окна при прямом запуске может не появиться вовсе, поэтому единственный
        // надёжный признак готовности процесса — удавшееся внедрение.
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

    spdlog::info("модуль внедрён: {}", clientModule.filename().string());
    spdlog::info("журнал клиента: %LOCALAPPDATA%\\oxyMP\\logs\\client.log");

    game->waitForExit();
    spdlog::info("игра завершилась");

    return 0;
}
