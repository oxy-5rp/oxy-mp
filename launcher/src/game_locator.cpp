#include "game_locator.hpp"

#include "registry.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

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

/// Читает файл целиком в строку. Пусто — прочитать не вышло.
std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// Достаёт все значения указанного ключа из текста вида `"ключ"  "значение"`.
///
/// Разбор нарочно грубый: Steam хранит настройки в VDF, Epic в JSON, и заводить
/// два разбирателя ради одного поля значило бы притащить в лаунчер две чужие
/// библиотеки. Нам нужно одно поле, и найти его поиском по имени надёжнее, чем
/// разобрать формат, который вправе смениться.
std::vector<std::string> valuesOf(const std::string& text, std::string_view key) {
    std::vector<std::string> values;

    const std::string quoted = std::format("\"{}\"", key);

    for (std::size_t at = text.find(quoted); at != std::string::npos;
         at = text.find(quoted, at + 1)) {
        // Значение — первая строка в кавычках после имени ключа.
        const std::size_t open = text.find('"', at + quoted.size());
        if (open == std::string::npos) {
            break;
        }

        const std::size_t close = text.find('"', open + 1);
        if (close == std::string::npos) {
            break;
        }

        std::string value = text.substr(open + 1, close - open - 1);

        // В обоих форматах разделитель пути экранирован удвоением.
        for (std::size_t slash = value.find("\\\\"); slash != std::string::npos;
             slash = value.find("\\\\", slash + 1)) {
            value.erase(slash, 1);
        }

        if (!value.empty()) {
            values.push_back(std::move(value));
        }
    }

    return values;
}

/// Где Steam держит свои библиотеки.
///
/// Их бывает несколько: игры ставят на большой диск, а сам Steam живёт на
/// системном. Список лежит в libraryfolders.vdf рядом с играми.
std::vector<std::filesystem::path> steamLibraries() {
    std::vector<std::filesystem::path> libraries;

    const auto steam = readCurrentUserString(L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam) {
        return libraries;
    }

    const std::filesystem::path root{*steam};
    libraries.push_back(root);

    const std::string list = readTextFile(root / "steamapps" / "libraryfolders.vdf");
    if (list.empty()) {
        return libraries;
    }

    for (const std::string& path : valuesOf(list, "path")) {
        libraries.emplace_back(path);
    }

    return libraries;
}

/// Что установлено через Epic Games.
///
/// Epic держит по файлу на игру в общем каталоге. Ищем среди них тот, что назван
/// Grand Theft Auto V, и берём его каталог установки.
std::vector<std::filesystem::path> epicInstalls() {
    std::vector<std::filesystem::path> installs;

    std::wstring programData(MAX_PATH, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(L"PROGRAMDATA", programData.data(),
                                                    static_cast<DWORD>(programData.size()));
    if (written == 0 || written >= programData.size()) {
        return installs;
    }
    programData.resize(written);

    const std::filesystem::path manifests = std::filesystem::path{programData} / "Epic" /
                                            "EpicGamesLauncher" / "Data" / "Manifests";

    std::error_code ec;
    if (!std::filesystem::exists(manifests, ec)) {
        return installs;
    }

    for (const auto& entry : std::filesystem::directory_iterator{manifests, ec}) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".item") {
            continue;
        }

        const std::string manifest = readTextFile(entry.path());

        // Отбираем по названию: у Epic в этом каталоге лежит всё установленное,
        // и открывать каждую игру подряд незачем.
        if (manifest.find("Grand Theft Auto V") == std::string::npos) {
            continue;
        }

        for (const std::string& path : valuesOf(manifest, "InstallLocation")) {
            installs.emplace_back(path);
        }
    }

    return installs;
}

} // namespace

std::string readGameVersion(const std::filesystem::path& executable) {
    const DWORD size = ::GetFileVersionInfoSizeW(executable.c_str(), nullptr);
    if (size == 0) {
        return {};
    }

    std::vector<std::uint8_t> block(size);

    if (::GetFileVersionInfoW(executable.c_str(), 0, size, block.data()) == 0) {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;

    if (::VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) == 0 ||
        info == nullptr) {
        return {};
    }

    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

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

    location.version = readGameVersion(location.executable);

    return location;
}

std::optional<GameLocation> locateGame(std::string& error) {
    // По очереди, в порядке того, как часто встречается. Первая найденная и
    // берётся: двух установок одной игры не бывает, а если у человека всё же
    // две, он укажет нужную ключом --game.
    struct Candidate {
        std::filesystem::path directory;
        std::string source;
    };

    std::vector<Candidate> candidates;

    if (const auto folder = readLocalMachineString(kRegistryPath, L"InstallFolder")) {
        candidates.push_back(Candidate{*folder, "Rockstar Games Launcher"});
    }

    for (const std::filesystem::path& library : steamLibraries()) {
        candidates.push_back(
            Candidate{library / "steamapps" / "common" / "Grand Theft Auto V", "Steam"});

        // Steam переименовал каталог, когда вышло переиздание, и у людей
        // встречаются оба.
        candidates.push_back(Candidate{
            library / "steamapps" / "common" / "Grand Theft Auto V Legacy", "Steam"});
    }

    for (const std::filesystem::path& directory : epicInstalls()) {
        candidates.push_back(Candidate{directory, "Epic Games"});
    }

    for (const Candidate& candidate : candidates) {
        std::string ignored;

        if (auto location = gameInDirectory(candidate.directory, ignored)) {
            location->source = candidate.source;

            spdlog::info("игра найдена ({}): {}", location->source,
                         location->directory.string());

            return location;
        }
    }

    error = candidates.empty()
                ? "игра не найдена: ни Rockstar, ни Steam, ни Epic о ней не знают.\n"
                  "Укажите каталог ключом --game."
                : "игра числится установленной, но GTA5.exe на месте нет.\n"
                  "Укажите каталог ключом --game.";

    return std::nullopt;
}

} // namespace oxymp::launcher
