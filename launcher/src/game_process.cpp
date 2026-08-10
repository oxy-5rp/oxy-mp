#include "game_process.hpp"

#include <cwchar>
#include <format>
#include <vector>

#include <windows.h>

#include <tlhelp32.h>

namespace oxymp::launcher {
namespace {

/// Класс главного окна игры.
constexpr const wchar_t* kGameWindowClass = L"grcWindow";

constexpr const wchar_t* kGameExecutableName = L"GTA5.exe";

/// Пускатель Rockstar. Именно он проводит игру через проверку лаунчера:
/// запущенная в обход него игра закрывается с ошибкой ERR_NO_LAUNCHER.
constexpr const wchar_t* kRockstarStarterName = L"PlayGTAV.exe";

/// Сколько ждать завершения внедрения. LoadLibraryW внутри игры выполняется
/// быстро, но процесс в этот момент занят загрузкой, поэтому запас щедрый.
constexpr DWORD kInjectTimeoutMs = 30'000;

std::string lastErrorText() {
    return std::format("код ошибки Windows {}", ::GetLastError());
}

struct WindowSearch {
    DWORD processId = 0;
    bool found = false;
};

BOOL CALLBACK onWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);

    DWORD owner = 0;
    ::GetWindowThreadProcessId(window, &owner);
    if (owner != search.processId) {
        return TRUE;
    }

    wchar_t className[64] = {};
    ::GetClassNameW(window, className, static_cast<int>(std::size(className)));

    if (std::wcscmp(className, kGameWindowClass) == 0) {
        search.found = true;
        return FALSE;
    }

    return TRUE;
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

namespace {

/// Права, которых достаточно для внедрения и наблюдения за процессом.
constexpr DWORD kInjectAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                                SYNCHRONIZE;

/// Запускает указанный исполняемый файл и возвращает описатели процесса.
bool startProcess(const std::filesystem::path& executable, const std::filesystem::path& directory,
                  PROCESS_INFORMATION& information, std::string& error) {
    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    const std::wstring workingDirectory = directory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    // Окружение не передаётся: nullptr означает «унаследовать наше», а нужные
    // переменные мы уже выставили себе.
    if (::CreateProcessW(executable.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                         0, nullptr, workingDirectory.c_str(), &startup, &information) == FALSE) {
        error = std::format("не удалось запустить {}: {}", executable.filename().string(),
                            lastErrorText());
        return false;
    }

    return true;
}

/// Ищет процесс по имени исполняемого файла.
DWORD findProcessId(const wchar_t* name) {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    DWORD found = 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (::Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (::_wcsicmp(entry.szExeFile, name) == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry) != FALSE);
    }

    ::CloseHandle(snapshot);
    return found;
}

} // namespace

std::unique_ptr<GameProcess> GameProcess::launch(const GameLocation& location, LaunchMode mode,
                                                 std::chrono::seconds appearTimeout,
                                                 std::string& error) {
    std::unique_ptr<GameProcess> game{new GameProcess};

    if (mode == LaunchMode::Direct) {
        PROCESS_INFORMATION information{};
        if (!startProcess(location.executable, location.directory, information, error)) {
            return nullptr;
        }

        game->process_ = information.hProcess;
        game->thread_ = information.hThread;
        game->processId_ = information.dwProcessId;

        return game;
    }

    // Штатный путь: процесс игры создаём не мы, а лаунчер Rockstar, поэтому
    // после запуска остаётся только дождаться его появления и взять описатель.
    PROCESS_INFORMATION starter{};
    if (!startProcess(location.directory / kRockstarStarterName, location.directory, starter,
                      error)) {
        return nullptr;
    }
    ::CloseHandle(starter.hThread);
    ::CloseHandle(starter.hProcess);

    const auto deadline = std::chrono::steady_clock::now() + appearTimeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (const DWORD found = findProcessId(kGameExecutableName); found != 0) {
            const HANDLE handle = ::OpenProcess(kInjectAccess, FALSE, found);
            if (handle == nullptr) {
                error = std::format("процесс игры найден, но доступ к нему закрыт: {}",
                                    lastErrorText());
                return nullptr;
            }

            game->process_ = handle;
            game->processId_ = found;
            return game;
        }

        ::Sleep(500);
    }

    error = "процесс игры так и не появился — проверьте, что в Rockstar Games Launcher "
            "выполнен вход";
    return nullptr;
}

bool GameProcess::waitUntilWindowAppears(std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (!isRunning()) {
            return false;
        }

        WindowSearch search;
        search.processId = processId_;
        ::EnumWindows(&onWindow, reinterpret_cast<LPARAM>(&search));

        if (search.found) {
            return true;
        }

        ::Sleep(250);
    }

    return false;
}

bool GameProcess::inject(const std::filesystem::path& module, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(module, ec)) {
        error = std::format("модуль не найден: {}", module.string());
        return false;
    }

    const std::wstring path = std::filesystem::absolute(module, ec).wstring();
    const SIZE_T pathBytes = (path.size() + 1) * sizeof(wchar_t);

    void* remotePath = ::VirtualAllocEx(process_, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
    if (remotePath == nullptr) {
        error = std::format("не удалось выделить память в процессе игры: {}", lastErrorText());
        return false;
    }

    bool ok = false;

    // Освобождать выделенное нужно на любом пути выхода, поэтому дальше идёт
    // единый блок с одной точкой очистки.
    do {
        SIZE_T written = 0;
        if (::WriteProcessMemory(process_, remotePath, path.c_str(), pathBytes, &written) == 0 ||
            written != pathBytes) {
            error = std::format("не удалось передать путь к модулю: {}", lastErrorText());
            break;
        }

        // Адрес LoadLibraryW одинаков во всех процессах сеанса: kernel32
        // загружается по одной и той же базе.
        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<void*>(::GetProcAddress(kernel32, "LoadLibraryW")));
        if (loadLibrary == nullptr) {
            error = "не удалось найти LoadLibraryW";
            break;
        }

        const HANDLE remoteThread =
            ::CreateRemoteThread(process_, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
        if (remoteThread == nullptr) {
            error = std::format("не удалось создать поток в процессе игры: {}", lastErrorText());
            break;
        }

        const DWORD waited = ::WaitForSingleObject(remoteThread, kInjectTimeoutMs);

        DWORD result = 0;
        ::GetExitCodeThread(remoteThread, &result);
        ::CloseHandle(remoteThread);

        if (waited != WAIT_OBJECT_0) {
            error = "внедрение не завершилось за отведённое время";
            break;
        }

        // Возвращается младшая половина описателя загруженного модуля.
        // Ноль означает, что LoadLibraryW отказал.
        if (result == 0) {
            error = "игра отказалась загружать модуль";
            break;
        }

        ok = true;
    } while (false);

    ::VirtualFreeEx(process_, remotePath, 0, MEM_RELEASE);

    return ok;
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
