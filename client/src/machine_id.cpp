#include "machine_id.hpp"

#include <windows.h>

#include <array>
#include <string>

namespace oxymp::client {
namespace {

/// Смешивание по SplitMix64: одно число из нескольких, без своих таблиц.
///
/// joaat здесь не годится: он тридцатидвухразрядный, а у alt:V отпечаток —
/// число в шестьдесят четыре разряда, и сузив его, мы получили бы совпадения
/// там, где их быть не должно.
[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;

    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t fold(std::uint64_t seed, std::string_view text) noexcept {
    for (const char letter : text) {
        seed = mix(seed ^ static_cast<std::uint64_t>(static_cast<unsigned char>(letter)));
    }

    return seed;
}

/// Постоянный номер этой установки Windows.
///
/// Лежит он в разделе, доступном на чтение всякому, и меняется только при
/// переустановке системы — то есть ровно тогда, когда меняется и машина с точки
/// зрения того, кто её узнаёт.
[[nodiscard]] std::string machineGuid() {
    HKEY key = nullptr;

    // KEY_WOW64_64KEY обязателен: клиент собран под 64 разряда, но раздел этот
    // Windows подменяет по разрядности, и без указания мы прочли бы чужой.
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\Microsoft\Cryptography", 0,
                        KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
    }

    std::array<wchar_t, 128> value{};
    auto size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    DWORD kind = 0;

    const LSTATUS read = ::RegQueryValueExW(key, L"MachineGuid", nullptr, &kind,
                                            reinterpret_cast<LPBYTE>(value.data()), &size);
    ::RegCloseKey(key);

    if (read != ERROR_SUCCESS || kind != REG_SZ) {
        return {};
    }

    std::string narrow;
    for (const wchar_t letter : value) {
        if (letter == L'\0') {
            break;
        }

        narrow.push_back(static_cast<char>(letter));
    }

    return narrow;
}

/// Серийный номер тома, на котором стоит система.
///
/// Второе слагаемое, а не единственное: номер тома меняется от всякого
/// форматирования, а `MachineGuid` — нет. Порознь каждое из них слабее пары.
[[nodiscard]] std::uint32_t systemVolumeSerial() noexcept {
    std::array<wchar_t, MAX_PATH> directory{};

    if (::GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size())) == 0) {
        return 0;
    }

    // Корень тома: «C:\» — три знака и завершающий ноль.
    directory[3] = L'\0';

    DWORD serial = 0;
    if (::GetVolumeInformationW(directory.data(), nullptr, 0, &serial, nullptr, nullptr, nullptr,
                                0) == FALSE) {
        return 0;
    }

    return serial;
}

} // namespace

std::uint64_t machineFingerprint() noexcept {
    const std::string guid = machineGuid();
    const std::uint32_t serial = systemVolumeSerial();

    if (guid.empty() && serial == 0) {
        return 0;
    }

    return fold(mix(serial), guid);
}

} // namespace oxymp::client
