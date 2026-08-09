#include "game_process.hpp"

#include <format>
#include <vector>

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Класс главного окна игры.
constexpr const wchar_t* kGameWindowClass = L"grcWindow";

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

std::unique_ptr<GameProcess> GameProcess::launch(const GameLocation& location, std::string& error) {
    std::wstring commandLine = L"\"" + location.executable.wstring() + L"\"";
    const std::wstring workingDirectory = location.directory.wstring();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    // Окружение не передаётся: nullptr означает «унаследовать наше», а нужные
    // переменные мы уже выставили себе.
    const BOOL created = ::CreateProcessW(location.executable.wstring().c_str(), commandLine.data(),
                                          nullptr, nullptr, FALSE, 0, nullptr,
                                          workingDirectory.c_str(), &startup, &information);
    if (created == FALSE) {
        error = std::format("не удалось запустить игру: {}", lastErrorText());
        return nullptr;
    }

    std::unique_ptr<GameProcess> game{new GameProcess};
    game->process_ = information.hProcess;
    game->thread_ = information.hThread;
    game->processId_ = information.dwProcessId;

    return game;
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

void GameProcess::waitForExit() {
    ::WaitForSingleObject(process_, INFINITE);
}

bool GameProcess::isRunning() const {
    return ::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

} // namespace oxymp::launcher
