#include "game_process.hpp"

#include "process_inject.hpp"
#include "process_lookup.hpp"

#include <format>

#include <windows.h>

namespace oxymp::launcher {
namespace {

constexpr const wchar_t* kGameExecutableName = L"GTA5.exe";

/// Ключ, которым игра отличает запуск из Rockstar Games Launcher от запуска в
/// обход него. Без ключа игра закрывается с ERR_NO_LAUNCHER, не дойдя до
/// загрузки, — и внедрять модуль оказывается некуда.
constexpr const wchar_t* kFromLauncherArgument = L"-fromRGL";

std::string lastErrorText() {
    return std::format("код ошибки Windows {}", ::GetLastError());
}

} // namespace

GameProcess::~GameProcess() {
    if (thread_ != nullptr) {
        ::CloseHandle(thread_);
    }
    if (process_ != nullptr) {
        ::CloseHandle(process_);
    }
}

std::unique_ptr<GameProcess> GameProcess::launchDirectly(const GameLocation& location,
                                                         std::string& error) {
    std::wstring commandLine =
        L"\"" + location.executable.wstring() + L"\" " + kFromLauncherArgument;

    const std::wstring workingDirectory = location.directory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    // Окружение не передаётся: nullptr означает «унаследовать наше», а нужные
    // переменные мы уже выставили себе.
    if (::CreateProcessW(location.executable.wstring().c_str(), commandLine.data(), nullptr,
                         nullptr, FALSE, 0, nullptr, workingDirectory.c_str(), &startup,
                         &information) == FALSE) {
        error = std::format("не удалось запустить {}: {}", location.executable.filename().string(),
                            lastErrorText());
        return nullptr;
    }

    // Процесс создаём мы сами, поэтому описатели получаем сразу и разыскивать
    // игру по имени не требуется.
    std::unique_ptr<GameProcess> game{new GameProcess};
    game->process_ = information.hProcess;
    game->thread_ = information.hThread;
    game->processId_ = information.dwProcessId;

    return game;
}

std::unique_ptr<GameProcess> GameProcess::attachTo(std::uint32_t processId, std::string& error) {
    const HANDLE handle = ::OpenProcess(kInjectAccess, FALSE, processId);
    if (handle == nullptr) {
        error = std::format("процесс игры найден, но доступ к нему закрыт: {}", lastErrorText());
        return nullptr;
    }

    std::unique_ptr<GameProcess> game{new GameProcess};
    game->process_ = handle;
    game->processId_ = processId;

    return game;
}

std::unique_ptr<GameProcess> GameProcess::attach(std::chrono::seconds waitTimeout,
                                                 std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + waitTimeout;

    for (;;) {
        if (const std::uint32_t found = findProcessByName(kGameExecutableName); found != 0) {
            return attachTo(found, error);
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            error = "процесс GTA5.exe не найден — игра не запущена";
            return nullptr;
        }

        ::Sleep(500);
    }
}

bool GameProcess::inject(const std::filesystem::path& module, std::string& error) {
    return injectModule(process_, module, error);
}

bool GameProcess::injectWithRetries(const std::filesystem::path& module,
                                    std::chrono::seconds timeout, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        if (inject(module, error)) {
            return true;
        }

        if (!isRunning()) {
            error = "процесс игры завершился раньше, чем удалось внедрить модуль";
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        ::Sleep(100);
    }
}

void GameProcess::waitForExit() {
    ::WaitForSingleObject(process_, INFINITE);
}

bool GameProcess::isRunning() const {
    return ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

} // namespace oxymp::launcher
