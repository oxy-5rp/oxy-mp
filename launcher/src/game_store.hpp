#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace oxymp::launcher {

/// Через какую площадку куплена игра.
enum class GameStore {
    Rockstar,
    Steam,
    Epic,
};

/// Узнаёт площадку по каталогу самой игры.
///
/// По файлам рядом с GTA5.exe, а не по тому, где мы игру нашли, и это не
/// придирка. Rockstar Games Launcher запускает игру любой площадки и записывает
/// её путь у себя наравне со своей: «нашли в ветке Rockstar» не значит «куплена
/// у Rockstar». Библиотека площадки лежит в каталоге игры и не врёт.
[[nodiscard]] GameStore storeOf(const std::filesystem::path& directory);

/// Название площадки — для журнала и для человека.
[[nodiscard]] std::string_view storeName(GameStore store) noexcept;

/// Название площадки в настройках: `rgl`, `steam`, `epic`.
///
/// Имена не наши: так их пишет alt:V в `gtaPlatform`, и файл настроек переносят
/// с одного мультиплеера на другой руками. Своё название означало бы, что
/// перенесённый файл читается наполовину.
[[nodiscard]] std::string_view storeKey(GameStore store) noexcept;

/// Площадка по названию из настроек.
///
/// Неизвестное имя означает Rockstar — то же, что и пустое: у копии Rockstar
/// своего признака нет вовсе, и «ничего особенного» здесь и есть она.
[[nodiscard]] GameStore storeFromKey(std::string_view key) noexcept;

/// Поднимает клиент площадки, без которого игра не запустится.
///
/// Копия из Steam спрашивает права у steam_api64.dll, а та — у запущенного
/// Steam: без него игра закрывается, не дойдя до загрузки, и человек видит лишь
/// мигнувшее окно. Копии Rockstar и Epic поднимаются одним и тем же PlayGTAV.exe
/// и своего клиента не требуют — для них здесь не делается ничего.
[[nodiscard]] bool ensureStoreReady(GameStore store, std::chrono::seconds timeout,
                                    std::string& error);

} // namespace oxymp::launcher
