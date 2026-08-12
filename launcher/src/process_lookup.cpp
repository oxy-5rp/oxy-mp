#include "process_lookup.hpp"

#include <cwchar>

#include <windows.h>

#include <tlhelp32.h>

namespace oxymp::launcher {

std::uint32_t findProcessByName(const wchar_t* name) {
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

} // namespace oxymp::launcher
