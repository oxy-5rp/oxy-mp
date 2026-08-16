#include "environment.hpp"

#include <oxymp/gamesig/catalog.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <dxgi.h>
#include <psapi.h>
#include <winver.h>

namespace oxymp::client::game {
namespace {

/// Путь к модулю по его описателю. Пустой путь означает, что узнать не вышло.
std::filesystem::path modulePath(HMODULE module) {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (written == 0) {
            return {};
        }

        if (written < buffer.size()) {
            buffer.resize(written);
            return std::filesystem::path{buffer};
        }

        buffer.resize(buffer.size() * 2);
    }
}

/// Описатель нашего собственного модуля.
///
/// Спрашивается по адресу здешней функции, а не берётся у точки входа: клиент
/// внедрён в чужой процесс, и «текущий модуль» для него — GTA5.exe.
HMODULE ownModule() {
    HMODULE module = nullptr;

    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&ownModule), &module);

    return module;
}

/// Версия исполняемого файла, как её показывает проводник: `1.0.3889.0`.
std::string fileVersion(const std::filesystem::path& file) {
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0) {
        return {};
    }

    std::vector<std::uint8_t> block(size);
    if (::GetFileVersionInfoW(file.c_str(), 0, size, block.data()) == 0) {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;

    if (::VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) == 0 ||
        info == nullptr) {
        return {};
    }

    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionLS));
}

/// Чем игра защищена и откуда она куплена.
///
/// Различается по соседям исполняемого файла: у каждой площадки свои. Знать это
/// нужно не из любопытства — доводы запуска, набор файлов и поведение лаунчера у
/// них разные, и половина «у меня не запускается» объясняется именно площадкой.
std::string_view gamePlatform(const std::filesystem::path& directory) {
    std::error_code ec;

    if (std::filesystem::exists(directory / "steam_api64.dll", ec)) {
        return "Steam";
    }

    if (std::filesystem::exists(directory / "EOSSDK-Win64-Shipping.dll", ec)) {
        return "Epic Games";
    }

    // Social Club есть у всех, поэтому проверяется последним: он не отличает
    // площадку, а лишь означает, что не нашлось ничего определённее.
    if (std::filesystem::exists(directory / "GTAVLauncher.exe", ec) ||
        std::filesystem::exists(directory / "PlayGTAV.exe", ec)) {
        return "Social Club";
    }

    return "неизвестно";
}

/// Видеокарта, на которой всё это рисуется.
///
/// Спрашивается у DXGI, а не у устройства игры: своего устройства у нас в этот
/// миг ещё нет, а перечень переходников есть всегда. Берётся первый — тот же,
/// который возьмёт и игра.
void reportAdapter() {
    IDXGIFactory* factory = nullptr;

    if (FAILED(::CreateDXGIFactory(__uuidof(IDXGIFactory),
                                   reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        spdlog::debug("видеокарту опросить не вышло: DXGI не отозвался");
        return;
    }

    IDXGIAdapter* adapter = nullptr;

    if (SUCCEEDED(factory->EnumAdapters(0, &adapter)) && adapter != nullptr) {
        DXGI_ADAPTER_DESC description{};

        if (SUCCEEDED(adapter->GetDesc(&description))) {
            const std::wstring wide{description.Description};
            const std::filesystem::path name{wide};

            // В гигабайтах: точное число байт здесь не значит ничего, а
            // «7 ГБ» отвечает на единственный вопрос, ради которого это и
            // пишется, — хватает ли памяти.
            const auto gigabytes = description.DedicatedVideoMemory / (1024ull * 1024ull * 1024ull);

            spdlog::info("видеокарта: {}", name.string());
            spdlog::info("своей памяти: {} ГБ, изготовитель {:04x}", gigabytes,
                         description.VendorId);
        }

        adapter->Release();
    }

    factory->Release();
}

/// Что ещё загружено в процесс игры.
///
/// Пишется целиком и намеренно. Чужой модуль в процессе — самая частая причина
/// поломок, которых нет ни у кого другого: античиты, оверлеи, средства записи,
/// чужие модификации. По одному только перечню половина таких случаев
/// объясняется без единого вопроса.
void reportLoadedModules() {
    const HANDLE process = ::GetCurrentProcess();

    std::vector<HMODULE> modules(512);

    for (;;) {
        DWORD needed = 0;

        if (::EnumProcessModules(process, modules.data(),
                                 static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                                 &needed) == 0) {
            spdlog::debug("перечень загруженных модулей не получен");
            return;
        }

        const std::size_t count = needed / sizeof(HMODULE);
        if (count <= modules.size()) {
            modules.resize(count);
            break;
        }

        modules.resize(count);
    }

    std::vector<std::string> paths;
    paths.reserve(modules.size());

    for (const HMODULE module : modules) {
        if (const std::filesystem::path path = modulePath(module); !path.empty()) {
            paths.push_back(path.string());
        }
    }

    // По алфавиту, а не в порядке загрузки: журналы двух запусков так
    // сравниваются построчно, а порядок загрузки от запуска к запуску пляшет и
    // сам по себе не значит ничего.
    std::sort(paths.begin(), paths.end());

    spdlog::info("загружено модулей: {}", paths.size());

    for (const std::string& path : paths) {
        spdlog::info("  {}", path);
    }
}

} // namespace

std::filesystem::path clientDirectory() {
    const std::filesystem::path path = modulePath(ownModule());
    return path.empty() ? std::filesystem::path{} : path.parent_path();
}

void reportEnvironment() {
    const std::filesystem::path game = modulePath(nullptr);
    const std::filesystem::path client = modulePath(ownModule());

    if (!game.empty()) {
        const std::filesystem::path directory = game.parent_path();

        spdlog::info("игра: {}", directory.string());

        const std::string version = fileVersion(game);

        spdlog::info("сборка игры: {} на {}", version.empty() ? "неизвестна" : version,
                     gamePlatform(directory));

        // Расхождение со сборкой, под которую выверены сигнатуры, объявляется
        // прямо здесь и заранее.
        //
        // Само по себе оно ничего не ломает: сигнатуры ищутся по образцам, и на
        // соседней сборке чаще всего находятся. Но когда они однажды не найдутся,
        // причина будет именно эта — а выглядеть это будет как «клиент внедрился
        // и ничего не делает». Строкой выше в журнале такой разбор заканчивается,
        // не начавшись.
        //
        // Так же поступает alt:V, объявляя расхождение версий своих архивов.
        if (!version.empty() && version != gamesig::kTargetGameVersion) {
            spdlog::warn("сигнатуры выверены под {} — эта сборка другая",
                         gamesig::kTargetGameVersion);
        }
    }

    if (!client.empty()) {
        spdlog::info("клиент: {}", client.parent_path().string());
    }

    reportAdapter();
    reportLoadedModules();
}

} // namespace oxymp::client::game
