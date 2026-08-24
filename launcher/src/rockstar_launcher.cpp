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
        error = "Rockstar Games Launcher is not in the registry - the game will not start without it";
        return false;
    }

    const std::filesystem::path executable =
        std::filesystem::path{*installFolder} / kLauncherExecutableName;

    std::error_code ec;
    if (!std::filesystem::exists(executable, ec)) {
        error = std::format("Rockstar Games Launcher not found: {}", executable.string());
        return false;
    }

    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    if (::CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                         0, nullptr, executable.parent_path().wstring().c_str(), &startup,
                         &information) == FALSE) {
        error = std::format("could not start Rockstar Games Launcher: Windows error {}",
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

    error = "Rockstar Games Launcher started but never began answering";
    return false;
}

} // namespace oxymp::launcher
