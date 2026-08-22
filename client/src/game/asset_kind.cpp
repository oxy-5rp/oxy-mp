#include "asset_kind.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace oxymp::client::game {

std::string lowerSuffix(std::string_view fileName) {
    const std::size_t dot = fileName.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }

    std::string suffix{fileName.substr(dot)};
    std::ranges::transform(suffix, suffix.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return suffix;
}

bool worthStreaming(std::string_view fileName) {
    // `.ymf` здесь не по ошибке: расширение у игры своё, но читает такие файлы
    // не стриминг, а загрузчик содержимого DLC. Объявленный стримингу вручную,
    // он отвергается — что игра нам и отвечала.
    //
    // `.rpf` тоже не сюда: архив не объявляют стримингу, его открывают и вешают
    // на приставку, а объявляют уже то, что внутри.
    static constexpr std::array kNotAssets = std::to_array<std::string_view>(
        {".lua", ".zip", ".rar", ".7z", ".txt", ".md", ".json", ".js", ".ts", ".html", ".css",
         ".ini", ".cfg", ".log", ".bak", ".psd", ".blend", ".fbx", ".ymf", ".db", ".sql", ".rpf",
         ".xml", ".dat", ".asi", ".dll", ".exe"});

    const std::string suffix = lowerSuffix(fileName);
    if (suffix.empty()) {
        return true;
    }

    return std::ranges::find(kNotAssets, suffix) == kNotAssets.end();
}

bool isArchetypeList(std::string_view fileName) {
    return lowerSuffix(fileName) == ".ytyp";
}

bool isArchive(std::string_view fileName) {
    return lowerSuffix(fileName) == ".rpf";
}

std::string_view baseName(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");

    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace oxymp::client::game
