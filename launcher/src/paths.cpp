#include "paths.hpp"

#include <string>

#include <windows.h>

namespace oxymp::launcher {

Paths Paths::beside() {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return Paths{std::filesystem::current_path()};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }

        buffer.resize(buffer.size() * 2);
    }

    return Paths{std::filesystem::path{buffer}.parent_path()};
}

void Paths::ensure() const {
    std::error_code ec;

    for (const std::filesystem::path& directory :
         {browserDirectory(), browserCache(), resourceCache(), backup(), logs()}) {
        std::filesystem::create_directories(directory, ec);
    }
}

} // namespace oxymp::launcher
