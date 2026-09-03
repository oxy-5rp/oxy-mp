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

/// Довод, которым игре называют язык. Тот же, что передаёт лаунчер Rockstar.
constexpr const wchar_t* kLanguageArgument = L"-rglLanguage";

/// Ключ, которым игра запускается сразу в свободный режим сети, минуя сюжет.
constexpr const wchar_t* kStraightIntoFreemode = L"-StraightIntoFreemode";

/// Как часто искать процесс игры, пока он не появился.
///
/// Этим интервалом ограничено окно, в котором игра уже бежит, а модуль ещё не
/// внедрён: всё это время игрок видит её собственный вступительный ролик —
/// логотипы и полицейскую заставку, — которую нечем закрыть, пока не встал наш
/// перехват показа кадра. Прежде здесь было полсекунды: игра, запущенная сразу
/// после опроса, оставалась неувиденной до следующего, и ролик успевал
/// показаться. Двадцать пять миллисекунд — перебор таблицы процессов раз в
/// полтора кадра, нагрузка на эти считаные секунды до появления игры
/// пренебрежимая.
constexpr DWORD kGameScanIntervalMs = 25;

/// Как часто пытаться внедриться, пока процесс к этому не готов.
///
/// Внедрение через LoadLibrary в удалённом потоке удаётся не сразу: у только что
/// созданного процесса загрузчик ещё поднимает ntdll и kernel32, и до того
/// попытка отказывает. Повтор идёт до первого успеха; чаще пробовать — раньше
/// поймать то мгновение, когда внедрение впервые проходит.
constexpr DWORD kInjectRetryIntervalMs = 25;

std::string lastErrorText() {
    return std::format("Windows error {}", ::GetLastError());
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
                                                         const Arguments& arguments,
                                                         std::string& error) {
    std::wstring commandLine =
        L"\"" + location.executable.wstring() + L"\" " + kFromLauncherArgument;

    // Язык называется здесь, потому что позже его назвать негде: внутри сессии
    // меню паузы у игры сетевое, и выбранную строку оно возвращает обратно —
    // так же, как в обычной GTA Online.
    if (!arguments.language.empty()) {
        commandLine += L' ';
        commandLine += kLanguageArgument;
        commandLine += L'=';
        commandLine += arguments.language;
    }

    if (arguments.straightIntoFreemode) {
        commandLine += L' ';
        commandLine += kStraightIntoFreemode;
    }

    const std::wstring workingDirectory = location.directory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    // Окружение не передаётся: nullptr означает «унаследовать наше», а нужные
    // переменные мы уже выставили себе.
    if (::CreateProcessW(location.executable.wstring().c_str(), commandLine.data(), nullptr,
                         nullptr, FALSE, 0, nullptr, workingDirectory.c_str(), &startup,
                         &information) == FALSE) {
        error = std::format("could not start {}: {}", location.executable.filename().string(),
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
        error = std::format("the game process was found, but access to it is denied: {}", lastErrorText());
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
            error = "no GTA5.exe process - the game is not running";
            return nullptr;
        }

        ::Sleep(kGameScanIntervalMs);
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
            error = "the game process exited before the client could be injected";
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        ::Sleep(kInjectRetryIntervalMs);
    }
}

void GameProcess::waitForExit() {
    ::WaitForSingleObject(process_, INFINITE);
}

bool GameProcess::isRunning() const {
    return ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

} // namespace oxymp::launcher
