#include "game_locator.hpp"

#include "registry.hpp"

#include <format>

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Игра ставится 32-разрядным установщиком, поэтому её ключ лежит в ветке
/// WOW6432Node, даже когда сама игра 64-разрядная.
constexpr const wchar_t* kRegistryPath = L"SOFTWARE\\WOW6432Node\\Rockstar Games\\Grand Theft Auto V";

constexpr const wchar_t* kExecutableName = L"GTA5.exe";

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                          size, nullptr, nullptr);

    return result;
}

} // namespace

std::optional<GameLocation> gameInDirectory(const std::filesystem::path& directory,
                                            std::string& error) {
    GameLocation location;
    location.directory = directory;
    location.executable = directory / kExecutableName;

    std::error_code ec;
    if (!std::filesystem::exists(location.executable, ec)) {
        error = std::format("в каталоге {} нет GTA5.exe", directory.string());
        return std::nullopt;
    }

    return location;
}

std::optional<GameLocation> locateGame(std::string& error) {
    const auto installFolder = readLocalMachineString(kRegistryPath, L"InstallFolder");
    if (!installFolder) {
        error = "игра не найдена в реестре — укажите каталог ключом --game";
        return std::nullopt;
    }

    auto location = gameInDirectory(*installFolder, error);
    if (!location) {
        return std::nullopt;
    }

    if (const auto version = readLocalMachineString(kRegistryPath, L"Version")) {
        location->version = narrow(*version);
    }

    return location;
}

} // namespace oxymp::launcher
