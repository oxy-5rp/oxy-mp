#include "game_platform.hpp"

#include "text.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <format>

#include <windows.h>

#include <shellapi.h>

namespace oxymp::launcher {
namespace {

/// Пускатель игры из каталога Rockstar. Не `PlayGTAV.exe`: alt:V зовёт именно
/// этот, и он же лежит в каталоге у всех трёх площадок.
constexpr const wchar_t* kRockstarStarter = L"GTAVLauncher.exe";

/// Довод самой игры, которым не поднимается BattlEye.
///
/// Не наша находка: alt:V передаёт его на всех трёх площадках, и на Steam с Epic
/// это единственное, чем защиту можно не пустить, — командную строку там
/// составляет площадка, и вклиниться в неё нечем.
constexpr const wchar_t* kNoBattlEye = L"-nobattleye";

/// Как площадка просит саму себя запустить игру.
///
/// Номер приложения в Steam и опознаватель у Epic — не подобранные, а взятые из
/// `altv.exe`: там они лежат готовыми строками.
constexpr const wchar_t* kSteamRunUrl = L"steam://run/271590/-nobattleye/";
constexpr const wchar_t* kEpicRunUrl =
    L"com.epicgames.launcher://apps/9d2d0eb64d5c44529cece33fe2a46482?action=launch";

/// Открывает ссылку средствами Windows — то есть отдаёт её той программе,
/// которая объявила себя хозяйкой этой схемы.
bool openUrl(const wchar_t* url, std::string& error) {
    // ShellExecuteW отвечает числом, и «больше 32» означает удачу; это её
    // собственный обычай, а не наше соглашение.
    constexpr INT_PTR kSuccessAbove = 32;

    const INT_PTR result =
        reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOW));

    if (result > kSuccessAbove) {
        return true;
    }

    error = std::format("ShellExecute failed with code {}", result);
    return false;
}

bool startThroughRockstar(const GameLocation& location, std::string& error) {
    const std::filesystem::path starter = location.directory / kRockstarStarter;

    std::error_code ec;
    if (!std::filesystem::exists(starter, ec)) {
        error = std::format("{} is not in the game folder", starter.filename().string());
        return false;
    }

    std::wstring commandLine = L'"' + starter.wstring() + L"\" " + kNoBattlEye;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    if (::CreateProcessW(starter.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                         nullptr, location.directory.wstring().c_str(), &startup,
                         &information) == FALSE) {
        error = std::format("Windows error {}", ::GetLastError());
        return false;
    }

    // Пускатель кончается сразу, передав просьбу дальше: игру создаёт не он, и
    // держать его описатели незачем.
    ::CloseHandle(information.hThread);
    ::CloseHandle(information.hProcess);

    return true;
}

} // namespace

bool startGameThroughPlatform(const GameLocation& location, std::string& error) {
    std::string trouble;
    bool started = false;

    switch (location.store) {
    case GameStore::Steam:
        started = openUrl(kSteamRunUrl, trouble);
        break;

    case GameStore::Epic:
        started = openUrl(kEpicRunUrl, trouble);
        break;

    case GameStore::Rockstar:
        started = startThroughRockstar(location, trouble);
        break;
    }

    if (!started) {
        error = std::format(text::kErrFailedToLaunchPlatform, storeName(location.store));

        spdlog::error("could not ask {} to start the game: {}", storeName(location.store), trouble);
        return false;
    }

    spdlog::debug("{} was asked to start the game", storeName(location.store));
    return true;
}

} // namespace oxymp::launcher
