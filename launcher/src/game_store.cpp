#include "game_store.hpp"

#include "process_lookup.hpp"
#include "registry.hpp"

#include <format>

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Библиотека Steamworks. Кладётся только в копию, купленную в Steam.
constexpr const wchar_t* kSteamMarker = L"steam_api64.dll";

/// Библиотека Epic Online Services — тем же признаком для копии из Epic.
constexpr const wchar_t* kEpicMarker = L"EOSSDK-Win64-Shipping.dll";

constexpr const wchar_t* kSteamProcessName = L"steam.exe";

/// Steam ставится на пользователя, а не на машину: за одним компьютером у
/// разных людей и пути разные, и ветка поэтому пользовательская.
constexpr const wchar_t* kSteamRegistryPath = L"Software\\Valve\\Steam";
constexpr const wchar_t* kSteamExecutableName = L"SteamExe";

/// Кто сейчас вошёл в Steam. Ноль — не вошёл никто.
constexpr const wchar_t* kSteamActiveProcessPath = L"Software\\Valve\\Steam\\ActiveProcess";
constexpr const wchar_t* kSteamActiveUserName = L"ActiveUser";

constexpr DWORD kPollIntervalMs = 500;

/// Готов ли Steam выдать игре права.
///
/// Спрашивается двумя вопросами, и оба нужны. Запись о вошедшем Steam обнуляет,
/// заканчивая работу, но упавший Steam обнулить её не успевает — по одной записи
/// мы приняли бы мёртвого за живого. А один лишь процесс ничего не значит:
/// поднятый Steam, в который никто не вошёл, для игры то же самое, что
/// незапущенный.
bool steamSignedIn() {
    if (findProcessByName(kSteamProcessName) == 0) {
        return false;
    }

    const auto user = readCurrentUserNumber(kSteamActiveProcessPath, kSteamActiveUserName);

    return user.has_value() && *user != 0;
}

bool startSteam(std::string& error) {
    const auto executable = readCurrentUserString(kSteamRegistryPath, kSteamExecutableName);
    if (!executable) {
        error = "the game is a Steam copy, but Steam itself is not installed here.\n"
                "Install Steam and sign in.";
        return false;
    }

    std::filesystem::path path{*executable};
    path.make_preferred();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        error = std::format("Steam is listed as installed, but {} is not there.\n"
                            "Reinstall Steam.",
                            path.string());
        return false;
    }

    // -silent: Steam поднимается в область уведомлений и не выскакивает окном
    // поверх лаунчера. Игре довольно того, что он запущен и в него вошли.
    std::wstring commandLine = L"\"" + path.wstring() + L"\" -silent";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION information{};

    if (::CreateProcessW(path.wstring().c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                         nullptr, path.parent_path().wstring().c_str(), &startup,
                         &information) == FALSE) {
        error = std::format("could not start Steam: Windows error {}", ::GetLastError());
        return false;
    }

    // Steam нам не принадлежит: он переживает и игру, и лаунчер.
    ::CloseHandle(information.hThread);
    ::CloseHandle(information.hProcess);

    return true;
}

bool ensureSteamReady(std::chrono::seconds timeout, std::string& error) {
    if (steamSignedIn()) {
        return true;
    }

    // Поднимаем только если процесса нет вовсе. Запущенный, но пустой Steam
    // поднимать заново незачем: вторая копия сама закроется, а войти за человека
    // мы всё равно не можем — остаётся ждать.
    if (findProcessByName(kSteamProcessName) == 0 && !startSteam(error)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (steamSignedIn()) {
            return true;
        }

        ::Sleep(kPollIntervalMs);
    }

    error = "Steam is running, but nobody is signed in - it has no one to get the "
            "entitlement from.\nSign in to Steam and try again.";

    return false;
}

} // namespace

GameStore storeOf(const std::filesystem::path& directory) {
    std::error_code ec;

    if (std::filesystem::exists(directory / kSteamMarker, ec)) {
        return GameStore::Steam;
    }

    if (std::filesystem::exists(directory / kEpicMarker, ec)) {
        return GameStore::Epic;
    }

    // Ни той, ни другой библиотеки — значит копия Rockstar. Отдельного признака
    // у неё нет: у Rockstar игра лежит без чужих переходников, и «ничего
    // лишнего» здесь и есть признак.
    return GameStore::Rockstar;
}

std::string_view storeName(GameStore store) noexcept {
    // Названия дословно те же, что у alt:V: человек читает их и в наших
    // отказах, и в его, и должен узнавать одно и то же.
    switch (store) {
    case GameStore::Steam:
        return "Steam";
    case GameStore::Epic:
        return "Epic Games Store";
    case GameStore::Rockstar:
        break;
    }

    return "Rockstar Games Launcher";
}

std::string_view storeKey(GameStore store) noexcept {
    switch (store) {
    case GameStore::Steam:
        return "steam";
    case GameStore::Epic:
        return "epic";
    case GameStore::Rockstar:
        break;
    }

    return "rgl";
}

GameStore storeFromKey(std::string_view key) noexcept {
    if (key == "steam") {
        return GameStore::Steam;
    }

    if (key == "epic") {
        return GameStore::Epic;
    }

    return GameStore::Rockstar;
}

bool ensureStoreReady(GameStore store, std::chrono::seconds timeout, std::string& error) {
    if (store == GameStore::Steam) {
        return ensureSteamReady(timeout, error);
    }

    // Копию Epic поднимает тот же PlayGTAV.exe, что и копию Rockstar, а права ей
    // выдаёт Rockstar Games Launcher — его готовность спрашивается отдельно и
    // для всех трёх площадок сразу. Своего клиента Epic здесь не требует, и
    // поднимать его на всякий случай мы не станем: правка по рассуждению, а не
    // по наблюдению, в этом проекте уже обходилась дорого.
    return true;
}

} // namespace oxymp::launcher
