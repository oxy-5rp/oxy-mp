// oxymp-server — сервер мультиплеера.
//
// Настройки задаются аргументами командной строки. Файла конфигурации пока нет
// намеренно: четыре параметра его не оправдывают, а лишняя зависимость — это
// лишний способ всё усложнить.

#include "server.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/// Флаг остановки. Обработчик сигнала не имеет права делать почти ничего,
/// поэтому он только поднимает флаг, а завершением занимается основной цикл.
std::atomic<bool> g_stopRequested{false};

extern "C" void onInterrupt(int) {
    g_stopRequested.store(true);
}

void printUsage() {
    std::cerr << "Использование:\n"
                 "  oxymp-server [--port <номер>] [--max-players <число>] [--name <имя>]\n";
}

/// Разбирает беззнаковое число целиком: \"30abc\" считается ошибкой, а не 30.
template<typename T>
bool parseNumber(std::string_view text, T& value) {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);

    return result.ec == std::errc{} && result.ptr == end;
}

bool parseArguments(int argc, char** argv, oxymp::server::Config& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const bool hasValue = i + 1 < argc;

        if (argument == "--port" && hasValue) {
            if (!parseNumber(argv[++i], config.port)) {
                return false;
            }
        } else if (argument == "--max-players" && hasValue) {
            if (!parseNumber(argv[++i], config.maxPlayers)) {
                return false;
            }
        } else if (argument == "--name" && hasValue) {
            config.name = argv[++i];
        } else {
            return false;
        }
    }

    return true;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    oxymp::server::Config config;
    if (!parseArguments(argc, argv, config)) {
        printUsage();
        return 2;
    }

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::debug);

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);

    std::string error;
    const auto server = oxymp::server::Server::start(config, error);
    if (!server) {
        spdlog::error("не удалось запустить сервер: {}", error);
        return 1;
    }

    spdlog::info("для остановки нажмите Ctrl+C");

    server->run(g_stopRequested);

    return 0;
}
