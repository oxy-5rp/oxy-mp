#include "rockstar_launcher.hpp"

#include "registry.hpp"

#include <filesystem>
#include <format>

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Лаунчер ставится 32-разрядным установщиком, поэтому ключ лежит в WOW6432Node.
constexpr const wchar_t* kRegistryPath = L"SOFTWARE\\WOW6432Node\\Rockstar Games\\Launcher";

constexpr const wchar_t* kLauncherExecutableName = L"Launcher.exe";

/// Пускатель Rockstar в каталоге игры. Сам ничего не решает: разыскивает
/// Rockstar Games Launcher и передаёт запуск ему.
constexpr const wchar_t* kStarterName = L"PlayGTAV.exe";

/// Канал, который лаунчер держит открытым, пока готов обслуживать игры.
///
/// Имя выяснено наблюдением за работающим лаунчером. Внутри GTA5.exe лежит ещё
/// строка `\\.\pipe\MP3LauncherPipe`, но такого канала в системе не бывает —
/// это наследие прежнего движка, и ориентироваться на неё нельзя.
constexpr const wchar_t* kLauncherPipe = L"\\\\.\\pipe\\MTLLauncher_Pipe";

/// Как часто проверять появление канала.
constexpr DWORD kPollIntervalMs = 250;

/// Существует ли именованный канал.
///
/// WaitNamedPipeW разделяет два разных случая: канала нет вовсе
/// (ERROR_FILE_NOT_FOUND) и канал есть, но все его экземпляры заняты. Второе
/// нас устраивает — лаунчер работает, просто сейчас с кем-то разговаривает.
bool pipeExists(const wchar_t* name) {
    if (::WaitNamedPipeW(name, 0) != FALSE) {
        return true;
    }

    return ::GetLastError() != ERROR_FILE_NOT_FOUND;
}

} // namespace

bool ensureRockstarLauncherReady(std::chrono::seconds timeout, std::string& error) {
    if (pipeExists(kLauncherPipe)) {
        return true;
    }

    const auto installFolder = readLocalMachineString(kRegistryPath, L"InstallFolder");
    if (!installFolder) {
        error = "Rockstar Games Launcher не найден в реестре — без него игра не запустится";
        return false;
    }

    const std::filesystem::path executable =
        std::filesystem::path{*installFolder} / kLauncherExecutableName;

    std::error_code ec;
    if (!std::filesystem::exists(executable, ec)) {
        error = std::format("Rockstar Games Launcher не найден: {}", executable.string());
        return false;
    }

    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    if (::CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                         0, nullptr, executable.parent_path().wstring().c_str(), &startup,
                         &information) == FALSE) {
        error = std::format("не удалось запустить Rockstar Games Launcher: код ошибки Windows {}",
                            ::GetLastError());
        return false;
    }

    // Процесс лаунчера нам не принадлежит: он переживает игру и живёт своей
    // жизнью, поэтому описатели закрываются сразу.
    ::CloseHandle(information.hThread);
    ::CloseHandle(information.hProcess);

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (pipeExists(kLauncherPipe)) {
            return true;
        }

        ::Sleep(kPollIntervalMs);
    }

    error = "Rockstar Games Launcher запущен, но так и не начал отвечать";
    return false;
}

bool startGameThroughLauncher(const GameLocation& location, std::string& error) {
    const std::filesystem::path starter = location.directory / kStarterName;

    std::error_code ec;
    if (!std::filesystem::exists(starter, ec)) {
        error = std::format("пускатель игры не найден: {}", starter.string());
        return false;
    }

    std::wstring commandLine = L"\"" + starter.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    if (::CreateProcessW(starter.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                         nullptr, location.directory.wstring().c_str(), &startup,
                         &information) == FALSE) {
        error = std::format("не удалось запустить {}: код ошибки Windows {}",
                            starter.filename().string(), ::GetLastError());
        return false;
    }

    // Пускатель заканчивает работу сразу, передав просьбу лаунчеру: держать его
    // описатели незачем, игру он не создаёт.
    ::CloseHandle(information.hThread);
    ::CloseHandle(information.hProcess);

    return true;
}

} // namespace oxymp::launcher
