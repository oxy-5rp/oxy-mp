#include "game_choice.hpp"

#include "game_store.hpp"

#include <oxymp/config/settings.hpp>

#include <spdlog/spdlog.h>

#include <string>

namespace oxymp::launcher {
namespace {

constexpr const char* kPathSetting = "gtapath";
constexpr const char* kPlatformSetting = "gtaPlatform";

} // namespace

std::optional<GameLocation> chosenGame(const std::filesystem::path& settingsFile) {
    const config::Settings settings = config::Settings::load(settingsFile);

    const std::string recorded = settings.text(kPathSetting);
    if (recorded.empty()) {
        return std::nullopt;
    }

    std::string error;

    std::optional<GameLocation> location =
        gameInDirectory(std::filesystem::path{recorded}, error);
    if (!location) {
        // Игру могли перенести или удалить. Это не повод отказывать: человека
        // спросят заново, как при первом знакомстве.
        spdlog::warn("the recorded game path no longer holds the game: {}", error);
        return std::nullopt;
    }

    const std::string platform = settings.text(kPlatformSetting);
    if (platform.empty()) {
        return location;
    }

    const GameStore chosen = storeFromKey(platform);

    // Записанное человеком сильнее найденного нами, и это не безразличие к
    // признакам в каталоге игры. Признаки отвечают на вопрос «чья это копия»
    // верно почти всегда, но выбирал площадку человек, и молча решить за него
    // иначе — значит сделать выбор в окне установки украшением.
    //
    // Расхождение поэтому не исправляется, а называется: в журнале видно оба
    // ответа, и разбираться, если что-то пойдёт не так, будет с чем.
    if (chosen != location->store) {
        spdlog::warn("the settings say {} but the game directory looks like {}: going with the "
                     "settings",
                     storeName(chosen), storeName(location->store));
    }

    location->store = chosen;
    return location;
}

bool rememberGame(const std::filesystem::path& settingsFile, const GameLocation& location) {
    // Читаем перед записью: в файле лежат все настройки клиента, и записать одни
    // лишь наши два поля значило бы стереть остальные сорок.
    config::Settings settings = config::Settings::load(settingsFile);

    settings.set(kPathSetting, location.directory.string());
    settings.set(kPlatformSetting, storeKey(location.store));

    if (!settings.save(settingsFile)) {
        spdlog::error("the chosen game path was not written to {}", settingsFile.string());
        return false;
    }

    spdlog::info("Game path saved to oxymp.toml: {} ({})", location.directory.string(),
                 storeName(location.store));
    return true;
}

} // namespace oxymp::launcher
