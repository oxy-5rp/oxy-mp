#pragma once

#include "game_store.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace oxymp::launcher {

/// Где установлена игра.
struct GameLocation {
    std::filesystem::path directory;
    std::filesystem::path executable;

    /// Версия, прочитанная из самого GTA5.exe.
    ///
    /// Из файла, а не из реестра, и это важно: реестр пишет установщик, а игру
    /// обновляют и в обход него. Версия файла — то, что на самом деле запустится.
    /// Пусто, если прочитать не удалось.
    std::string version;

    /// Чья это копия. Узнаётся по самому каталогу игры, а не по тому, где мы
    /// её нашли: искать можно где угодно, а площадка у копии одна.
    GameStore store = GameStore::Rockstar;
};

/// Находит установленную игру.
///
/// Ищет во всех местах, где она бывает: у Rockstar в реестре, в библиотеках
/// Steam, среди установленного Epic. Первая найденная и возвращается — двух
/// установок одной игры не бывает, а если бывает, человек укажет нужную ключом
/// --game.
[[nodiscard]] std::optional<GameLocation> locateGame(std::string& error);

/// Версия исполняемого файла игры. Пусто, если прочитать не удалось.
[[nodiscard]] std::string readGameVersion(const std::filesystem::path& executable);

/// Проверяет каталог, указанный вручную.
[[nodiscard]] std::optional<GameLocation> gameInDirectory(const std::filesystem::path& directory,
                                                          std::string& error);

} // namespace oxymp::launcher
