#pragma once

#include "game_store.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

/// Находит все установленные копии игры.
///
/// Все, а не первую попавшуюся, и это не мелочь: у человека их бывает две —
/// обычная и Legacy, купленные на разных площадках, — и решать за него, какая
/// нужна, лаунчеру не по чину. Список уходит в окно выбора каталога, и выбирает
/// человек. Так же поступает alt:V: его окно `gamepath` показывает найденное
/// списком.
///
/// Ищется во всех местах, где игра бывает: в собственном списке Rockstar Games
/// Launcher, в реестре (тремя разными значениями — своим, Steam и Epic), в
/// библиотеках Steam и среди установленного Epic. Одинаковые пути схлопываются.
[[nodiscard]] std::vector<GameLocation> findGames();

/// Находит установленную игру — первую из findGames.
///
/// Остаётся ради запуска без окна: там спросить некого, и выбирать приходится
/// за человека.
[[nodiscard]] std::optional<GameLocation> locateGame(std::string& error);

/// Версия исполняемого файла игры. Пусто, если прочитать не удалось.
[[nodiscard]] std::string readGameVersion(const std::filesystem::path& executable);

/// Проверяет каталог, указанный вручную.
[[nodiscard]] std::optional<GameLocation> gameInDirectory(const std::filesystem::path& directory,
                                                          std::string& error);

} // namespace oxymp::launcher
