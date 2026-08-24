#include "process_inject.hpp"

#include <format>

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Сколько ждать завершения внедрения. LoadLibraryW выполняется быстро, но
/// процесс в этот момент бывает занят собой, поэтому запас щедрый.
constexpr DWORD kInjectTimeoutMs = 30'000;

std::string lastErrorText() {
    return std::format("Windows error {}", ::GetLastError());
}

} // namespace

bool injectModule(void* process, const std::filesystem::path& module, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(module, ec)) {
        error = std::format("the module was not found: {}", module.string());
        return false;
    }

    const std::wstring path = std::filesystem::absolute(module, ec).wstring();
    const SIZE_T pathBytes = (path.size() + 1) * sizeof(wchar_t);

    void* remotePath =
        ::VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr) {
        error = std::format("could not allocate memory in the other process: {}", lastErrorText());
        return false;
    }

    bool ok = false;

    // Освобождать выделенное нужно на любом пути выхода, поэтому дальше идёт
    // единый блок с одной точкой очистки.
    do {
        SIZE_T written = 0;
        if (::WriteProcessMemory(process, remotePath, path.c_str(), pathBytes, &written) == 0 ||
            written != pathBytes) {
            error = std::format("could not pass the module path over: {}", lastErrorText());
            break;
        }

        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            reinterpret_cast<void*>(::GetProcAddress(kernel32, "LoadLibraryW")));
        if (loadLibrary == nullptr) {
            error = "LoadLibraryW was not found";
            break;
        }

        const HANDLE remoteThread =
            ::CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
        if (remoteThread == nullptr) {
            error = std::format("could not create a thread in the other process: {}", lastErrorText());
            break;
        }

        const DWORD waited = ::WaitForSingleObject(remoteThread, kInjectTimeoutMs);

        DWORD result = 0;
        ::GetExitCodeThread(remoteThread, &result);
        ::CloseHandle(remoteThread);

        if (waited != WAIT_OBJECT_0) {
            error = "the injection did not finish in time";
            break;
        }

        // Возвращается младшая половина описателя загруженного модуля.
        // Ноль означает, что LoadLibraryW отказал.
        if (result == 0) {
            error = "the process refused to load the module";
            break;
        }

        ok = true;
    } while (false);

    ::VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    return ok;
}

} // namespace oxymp::launcher
