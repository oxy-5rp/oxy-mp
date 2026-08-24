#include "game_locator.hpp"

#include "game_store.hpp"
#include "registry.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include <windows.h>

#include <bcrypt.h>

namespace oxymp::launcher {
namespace {

/// Игра ставится 32-разрядным установщиком, поэтому её ключ лежит в ветке
/// WOW6432Node, даже когда сама игра 64-разрядная.
constexpr const wchar_t* kRegistryPath = L"SOFTWARE\\WOW6432Node\\Rockstar Games\\Grand Theft Auto V";

/// Тот же ключ без WOW6432Node: на некоторых установках он лежит и там.
constexpr const wchar_t* kRegistryPathNative =
    L"SOFTWARE\\Rockstar Games\\Grand Theft Auto V";

/// Где Windows держит запись Steam об установленной игре. 271590 — её номер в
/// магазине Steam; он же стоит в ссылке запуска.
constexpr const wchar_t* kSteamUninstallPath =
    L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 271590";
constexpr const wchar_t* kSteamUninstallPathNative =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 271590";

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

/// Расшифровывает список игр Rockstar Games Launcher.
///
/// AES-256-CBC на нулевом ключе и нулевом векторе. Это не взлом и не подбор:
/// ключ у самого Rockstar нулевой, файл им не заперт, а прикрыт. Первые
/// шестнадцать байт расшифрованного — не текст, и отбрасываются.
///
/// Пусто, если файла нет или он оказался не тем, чем мы его считали. Отказывать
/// из-за него нельзя ни в коем случае: это одно из мест поиска, а не
/// единственное.
std::string decryptTitles(std::vector<std::uint8_t>& data) {
    // Шифр блочный: длина не кратна блоку — значит перед нами не то, что мы
    // думаем, и расшифровывать нечего.
    constexpr std::size_t kBlockSize = 16;

    if (data.empty() || data.size() % kBlockSize != 0) {
        return {};
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }

    std::string text;

    // Сцепление блоков задаётся отдельно: по умолчанию у поставщика режим CBC не
    // выбран, и без этой строки расшифровался бы каждый блок сам по себе.
    if (::BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                            sizeof(BCRYPT_CHAIN_MODE_CBC), 0) == 0) {
        std::array<std::uint8_t, 32> key{};
        std::array<std::uint8_t, kBlockSize> startVector{};

        BCRYPT_KEY_HANDLE handle = nullptr;

        if (::BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, key.data(),
                                         static_cast<ULONG>(key.size()), 0) == 0) {
            ULONG written = 0;

            if (::BCryptDecrypt(handle, data.data(), static_cast<ULONG>(data.size()), nullptr,
                                startVector.data(), static_cast<ULONG>(startVector.size()), data.data(),
                                static_cast<ULONG>(data.size()), &written, 0) == 0 &&
                written > kBlockSize) {
                text.assign(reinterpret_cast<const char*>(data.data()) + kBlockSize,
                            written - kBlockSize);
            }

            ::BCryptDestroyKey(handle);
        }
    }

    ::BCryptCloseAlgorithmProvider(algorithm, 0);

    return text;
}

/// Куда Rockstar Games Launcher поставил игры.
///
/// Он запускает игру любой площадки, и его собственный список — единственное
/// место, где путь к копии из Steam или Epic записан наверняка. В реестре они
/// лежат под другими именами значений, и имена эти у площадок разные; здесь же
/// путь один и тот же для всех.
///
/// Возвращаются все установленные игры подряд, без разбора, какая из них наша:
/// GTA5.exe лежит ровно в одной, и отобрать её проще проверкой каталога, чем
/// разбором чужого формата, который вправе смениться.
std::vector<std::filesystem::path> rockstarLauncherInstalls() {
    std::vector<std::filesystem::path> installs;

    std::wstring programData(MAX_PATH, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(L"PROGRAMDATA", programData.data(),
                                                    static_cast<DWORD>(programData.size()));
    if (written == 0 || written >= programData.size()) {
        return installs;
    }
    programData.resize(written);

    const std::filesystem::path titles =
        std::filesystem::path{programData} / "Rockstar Games" / "Launcher" / "titles.dat";

    const std::string raw = readTextFile(titles);
    if (raw.empty()) {
        return installs;
    }

    std::vector<std::uint8_t> data(raw.begin(), raw.end());

    const std::string list = decryptTitles(data);
    if (list.empty()) {
        return installs;
    }

    // `il` — install location. У неустановленных игр он пуст, и valuesOf такие
    // значения не возвращает вовсе.
    for (const std::string& path : valuesOf(list, "il")) {
        installs.emplace_back(path);
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
        error = std::format("there is no GTA5.exe in {}", directory.string());
        return std::nullopt;
    }

    location.version = readGameVersion(location.executable);
    location.store = storeOf(location.directory);

    return location;
}

std::vector<GameLocation> findGames() {
    // Куда смотреть. Название места — не площадка, а лишь пометка для журнала:
    // где искали и где нашли. Чья это копия, спрашивается потом у самого
    // каталога — библиотека площадки лежит в нём и не врёт.
    struct Candidate {
        std::filesystem::path directory;
        std::string origin;
    };

    std::vector<Candidate> candidates;

    // Список Rockstar Games Launcher — первым, и это не вкусовщина. Игру любой
    // площадки запускает он, поэтому запись о ней у него есть всегда, а всё
    // остальное здесь — обходные пути на случай, если списка нет.
    for (const std::filesystem::path& directory : rockstarLauncherInstalls()) {
        candidates.push_back(Candidate{directory, "Rockstar Games Launcher"});
    }

    // Три значения в одном ключе реестра, по одному на площадку. Имена не наши:
    // так их пишет установщик Rockstar, и так их читает alt:V.
    for (const std::wstring& root : {std::wstring{kRegistryPath},
                                     std::wstring{kRegistryPathNative}}) {
        for (const wchar_t* value : {L"InstallFolder", L"InstallFolderSteam", L"InstallFolderEpic"}) {
            if (const auto folder = readLocalMachineString(root.c_str(), value)) {
                candidates.push_back(Candidate{*folder, "the registry"});
            }
        }
    }

    // Steam записывает свою копию ещё и туда, где Windows держит список
    // установленного, — под номером игры в её магазине.
    for (const wchar_t* uninstall : {kSteamUninstallPath, kSteamUninstallPathNative}) {
        if (const auto folder = readLocalMachineString(uninstall, L"InstallLocation")) {
            candidates.push_back(Candidate{*folder, "Steam"});
        }
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
        candidates.push_back(Candidate{directory, "Epic Games Store"});
    }

    std::vector<GameLocation> found;

    for (const Candidate& candidate : candidates) {
        std::string ignored;

        auto location = gameInDirectory(candidate.directory, ignored);
        if (!location) {
            continue;
        }

        // Одна и та же копия находится сразу несколькими путями — списком
        // Rockstar, реестром и библиотекой Steam, — и показать её человеку
        // трижды значило бы предложить выбор там, где выбора нет.
        const bool known =
            std::any_of(found.begin(), found.end(), [&location](const GameLocation& seen) {
                std::error_code ec;
                return std::filesystem::equivalent(seen.directory, location->directory, ec);
            });

        if (known) {
            continue;
        }

        spdlog::debug("game found through {}", candidate.origin);
        spdlog::info("Game found ({}): {}", storeName(location->store),
                     location->directory.string());

        found.push_back(*location);
    }

    return found;
}

std::optional<GameLocation> locateGame(std::string& error) {
    const std::vector<GameLocation> found = findGames();

    if (!found.empty()) {
        return found.front();
    }

    error = "the game was not found: Rockstar, Steam and Epic all know nothing about it";
    return std::nullopt;
}

} // namespace oxymp::launcher
