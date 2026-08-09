#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace oxymp::launcher {

/// Где установлена игра.
struct GameLocation {
    std::filesystem::path directory;
    std::filesystem::path executable;

    /// Версия из реестра. Может расходиться с фактической, если игру обновляли
    /// в обход установщика, поэтому используется только для сообщений.
    std::string version;
};

/// Находит установленную игру через реестр.
[[nodiscard]] std::optional<GameLocation> locateGame(std::string& error);

/// Проверяет каталог, указанный вручную.
[[nodiscard]] std::optional<GameLocation> gameInDirectory(const std::filesystem::path& directory,
                                                          std::string& error);

} // namespace oxymp::launcher
