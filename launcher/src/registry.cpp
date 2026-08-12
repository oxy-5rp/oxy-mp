#include "registry.hpp"

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Читает строковое значение из указанной ветки реестра.
///
/// Ветка — довод, а не два почти одинаковых тела: у Rockstar настройки лежат
/// в машинной ветке, у Steam в пользовательской, и различаются они только этим.
std::optional<std::wstring> readString(HKEY root, const wchar_t* path, const wchar_t* name) {
    DWORD bytes = 0;
    if (::RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }

    // RegGetValueW считает вместе с завершающим нулём, в std::wstring он лишний.
    if (const std::size_t end = value.find(L'\0'); end != std::wstring::npos) {
        value.resize(end);
    }

    return value;
}

} // namespace

std::optional<std::wstring> readLocalMachineString(const wchar_t* path, const wchar_t* name) {
    return readString(HKEY_LOCAL_MACHINE, path, name);
}

std::optional<std::wstring> readCurrentUserString(const wchar_t* path, const wchar_t* name) {
    return readString(HKEY_CURRENT_USER, path, name);
}

} // namespace oxymp::launcher
