// oxymp-server — сервер мультиплеера.
//
// Настройки читаются из server.cfg рядом с исполняемым файлом, а ключи
// командной строки перекрывают прочитанное. Порядок именно такой: файл описывает
// сервер, а строка запуска — сегодняшний опыт над ним, и опыт должен побеждать.
//
// Долгое время файла не было намеренно: четырёх параметров он не оправдывал.
// Теперь их два десятка — точка появления, дальность видимости, погода, список
// раздаваемого, — и строка запуска перестала помещаться в голове.

#include "config_file.hpp"
#include "server.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <cstdio>
#include <csignal>
#include <filesystem>
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
    std::cerr << "Usage:\n"
                 "  oxymp-server [--config <file>] [--port <number>] "
                 "[--max-players <count>] [--name <name>] [--debug]\n";
}

/// Разбирает беззнаковое число целиком: "30abc" считается ошибкой, а не 30.
template<typename T>
bool parseNumber(std::string_view text, T& value) {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);

    return result.ec == std::errc{} && result.ptr == end;
}

/// Где искать конфигурацию, если её не назвали явно.
constexpr const char* kDefaultConfig = "server.cfg";

/// Как называет свою конфигурацию alt:V.
///
/// Берётся только если своей нет вовсе: сервер, у которого лежат обе, слушается
/// написанного для него.
constexpr const char* kAltConfig = "server.toml";

/// Достаёт путь к конфигурации из командной строки.
///
/// Отдельным проходом до разбора остального, и это не небрежность: файл
/// читается первым, а ключи строки ложатся поверх него. Узнать имя файла из
/// того же прохода, который уже начал заполнять настройки, было бы поздно.
[[nodiscard]] std::string configPath(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view{argv[i]} == "--config") {
            return argv[i + 1];
        }
    }

    if (!std::filesystem::exists(kDefaultConfig) && std::filesystem::exists(kAltConfig)) {
        return kAltConfig;
    }

    return kDefaultConfig;
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
        } else if (argument == "--debug") {
            config.verbose = true;
        } else if (argument == "--config" && hasValue) {
            // Уже прочитан отдельным проходом — здесь его нужно только пропустить
            // вместе со значением, чтобы он не сошёл за неизвестный ключ.
            ++i;
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

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    // По умолчанию — только то, что нужно хозяину сервера: кто вошёл, что
    // поднялось, что сломалось. Опись каталога раздаваемых файлов, адреса и
    // разбор пакетов нужны тому, кто чинит сервер, и никому больше — они уходят
    // на отладочный уровень и включаются ключом.
    //
    // Ставится до разбора настроек: жалобы на сам файл настроек должны быть
    // видны, а уровень к тому мгновению ещё не прочитан.
    spdlog::set_level(spdlog::level::info);

    oxymp::server::Config config;

    // Сперва файл, потом командная строка: файл описывает сервер, а строка
    // запуска — сегодняшний опыт над ним, и опыт должен побеждать.
    const std::string path = configPath(argc, argv);

    std::string configError;
    if (!oxymp::server::config_file::load(path, config, configError)) {
        spdlog::error("{}: {}", path, configError);
        return 2;
    }

    if (!parseArguments(argc, argv, config)) {
        printUsage();
        return 2;
    }

    if (config.verbose) {
        spdlog::set_level(spdlog::level::debug);
    }

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);

    std::string error;
    const auto server = oxymp::server::Server::start(config, error);
    if (!server) {
        spdlog::error("failed to start the server: {}", error);
        return 1;
    }

    spdlog::info("Press Ctrl+C to stop");

    server->run(g_stopRequested);

    return 0;
}
