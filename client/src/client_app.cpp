#include "client_app.hpp"

#include <oxymp/client/connection.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

namespace oxymp::client {
namespace {

std::atomic<bool> g_stopRequested{false};
std::thread g_worker;

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
void applyServerAddress(std::string_view text, Connection::Settings& settings) {
    if (text.empty()) {
        return;
    }

    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        settings.address = text;
        return;
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
}

std::filesystem::path logFilePath() {
    const std::string localAppData = environmentValue(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        return "oxymp-client.log";
    }

    return std::filesystem::path{localAppData} / "oxyMP" / "logs" / "client.log";
}

/// Направляет журнал в файл.
///
/// Консоли у процесса игры нет, поэтому файл — единственный способ узнать, что
/// происходило внутри.
void setUpLogging() {
    try {
        const std::filesystem::path path = logFilePath();

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        // true: журнал каждого запуска начинается с чистого листа, иначе
        // разбираться в нём после нескольких попыток невозможно.
        auto logger = spdlog::basic_logger_mt("oxymp", path.string(), true);
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        logger->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(spdlog::level::debug);
    } catch (const spdlog::spdlog_ex&) {
        // Без журнала работать можно, падать из-за него — нельзя.
    }
}

Connection::Settings readSettings() {
    Connection::Settings settings;

    applyServerAddress(environmentValue(L"OXYMP_SERVER"), settings);

    if (std::string nickname = environmentValue(L"OXYMP_NICKNAME"); !nickname.empty()) {
        settings.nickname = std::move(nickname);
    }

    return settings;
}

void run() {
    setUpLogging();

    const Connection::Settings settings = readSettings();
    spdlog::info("клиент запущен внутри игры: сервер {}:{}, имя \"{}\"", settings.address,
                 settings.port, settings.nickname);

    Connection connection{settings};

    while (!g_stopRequested.load()) {
        connection.update(std::chrono::milliseconds{50});
    }

    connection.disconnect();
    spdlog::info("клиент остановлен");
    spdlog::default_logger()->flush();
}

} // namespace

void ClientApp::requestStart() {
    g_stopRequested.store(false);
    g_worker = std::thread{&run};
}

void ClientApp::requestStop() {
    g_stopRequested.store(true);

    // Поток намеренно отсоединяется, а не ожидается: см. пояснение в заголовке.
    if (g_worker.joinable()) {
        g_worker.detach();
    }
}

} // namespace oxymp::client
