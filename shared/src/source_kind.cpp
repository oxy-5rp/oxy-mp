#include <oxymp/shared/resource/source_kind.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace oxymp::shared {
namespace {

/// Окончание имени, приведённое к строчным. Пусто — окончания нет.
[[nodiscard]] std::string lowerSuffix(std::string_view fileName) {
    const std::size_t slash = fileName.find_last_of("/\\");
    const std::string_view tail =
        slash == std::string_view::npos ? fileName : fileName.substr(slash + 1);

    // Точка ищется в имени, а не во всём пути: каталог с точкой в названии
    // (`assets/v1.2/файл`) иначе отдал бы окончанием кусок пути.
    const std::size_t dot = tail.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }

    std::string suffix{tail.substr(dot)};

    std::ranges::transform(suffix, suffix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return suffix;
}

} // namespace

bool isSourceOrMarkup(std::string_view fileName) {
    // Здесь только то, чего игра не читает ни одной своей дорогой. Проверить
    // это просто: до игры файл ресурса доходит ровно двумя путями — объявлением
    // стримингу (и там клиент отвергает эти окончания сам) и загрузчиком
    // данных (а туда попадает лишь названное в `[meta]` и `*.meta`).
    //
    // Поэтому добавлять сюда можно смело, а вот убирать — нет: убранное
    // окончание тут же ложится игроку на диск открытым текстом.
    static constexpr std::array kSource = std::to_array<std::string_view>(
        {".js", ".cjs", ".mjs", ".jsx", ".ts", ".tsx", ".mts", ".cts", ".json", ".json5",
         ".html", ".htm", ".css", ".scss", ".sass", ".less", ".map", ".vue", ".svelte",
         ".md", ".txt", ".yml", ".yaml", ".toml", ".ini", ".cfg", ".conf", ".env",
         ".lua", ".sql", ".wasm", ".xml", ".csv", ".graphql", ".hbs", ".ejs", ".pug"});

    return std::ranges::find(kSource, lowerSuffix(fileName)) != kSource.end();
}

bool isGameDescription(std::string_view fileName) {
    return lowerSuffix(fileName) == ".meta";
}

} // namespace oxymp::shared
