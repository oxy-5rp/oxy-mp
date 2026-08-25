#include "game_conflicts.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <string_view>

#include <windows.h>

namespace oxymp::launcher {
namespace {

/// Признаки пиратской копии.
///
/// Права на игру такая копия спрашивает у подделанной библиотеки, а не у
/// площадки, и до сессии не доходит: окно мигает и закрывается. Причина при этом
/// не видна нигде — ни в журнале игры, ни в нашем.
///
/// Список взят у gtaMP. Чинить такую копию мы не будем и не должны; сказать о
/// ней прямо — обязаны, иначе человек будет искать поломку у нас.
constexpr std::array kPirateMarkers = std::to_array<std::string_view>(
    {"socialclub.ini", "steam_api.ini", "steam000.wow", "steam001.wow"});

/// Чужой мод, мешающий запуску.
struct Conflict {
    /// Чем звать его человеку.
    std::string_view name;

    /// Файл, по которому он узнаётся.
    std::string_view file;

    /// Наименьшая версия, с которой он не мешает. Пусто — мешает любой.
    std::string_view since;

    /// Что делать.
    std::string_view advice;
};

/// Что ломает запуск, и почему именно это.
///
/// Причины взяты вместе со списком у gtaMP, и они не выдуманы:
///
/// - **ReShade до 5.9.1** — FiveM сообщает о порче кучи и вылетах внутри
///   `RtlReportFatalFailure`; 5.9.1 и новее держатся.
/// - **ENB до 0.4.8** — сам его разработчик отвечает «не поддерживается».
/// - **OpenIV** оставляет в каталоге игры `dinput8.dll` и `dsound.dll`; они
///   перехватывают загрузку и спорят с нашим внедрением.
/// - **commandline.txt** игра читает раньше наших доводов, и запуск уезжает по
///   чужой командной строке — включая ту, что поднимает BattlEye обратно.
constexpr std::array kConflicts = std::to_array<Conflict>(
    {{.name = "ReShade",
      .file = "dxgi.dll",
      .since = "5.9.1",
      .advice = "update ReShade to 5.9.1 or newer, or remove dxgi.dll"},
     {.name = "ENB",
      .file = "d3d11.dll",
      .since = "0.4.8",
      .advice = "update ENB to 0.4.8 or newer, or remove d3d11.dll"},
     {.name = "OpenIV",
      .file = "dinput8.dll",
      .since = {},
      .advice = "remove dinput8.dll from the game folder"},
     {.name = "OpenIV",
      .file = "dsound.dll",
      .since = {},
      .advice = "remove dsound.dll from the game folder"},
     {.name = "commandline.txt",
      .file = "commandline.txt",
      .since = {},
      .advice = "remove commandline.txt: the game reads it instead of our arguments"}});

/// Версия файла четырьмя числами. Пусто — версии у него нет.
///
/// Спрашивается у Windows, а не разбирается самими: у чужого мода версия лежит
/// там же, где у всякой библиотеки, и читать её своим разбором PE незачем.
[[nodiscard]] std::string versionOf(const std::filesystem::path& file) {
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(file.wstring().c_str(), &ignored);

    if (size == 0) {
        return {};
    }

    std::vector<std::byte> block(size);

    if (::GetFileVersionInfoW(file.wstring().c_str(), 0, size, block.data()) == FALSE) {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;

    if (::VerQueryValueW(block.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) == FALSE ||
        info == nullptr) {
        return {};
    }

    return std::format("{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS));
}

/// Меньше ли первая версия второй. Сравниваются числа, а не строки.
[[nodiscard]] bool older(std::string_view found, std::string_view least) {
    const auto parts = [](std::string_view text) {
        std::array<int, 3> numbers{};
        std::size_t at = 0;

        for (int& number : numbers) {
            if (at >= text.size()) {
                break;
            }

            const std::size_t dot = text.find('.', at);
            const std::string piece{text.substr(at, dot - at)};

            number = piece.empty() ? 0 : std::atoi(piece.c_str());

            if (dot == std::string_view::npos) {
                break;
            }

            at = dot + 1;
        }

        return numbers;
    };

    return parts(found) < parts(least);
}

} // namespace

std::vector<GameConflicts::Found> GameConflicts::inspect(
    const std::filesystem::path& gameDirectory) {
    std::vector<Found> found;

    if (gameDirectory.empty()) {
        return found;
    }

    std::error_code ec;

    for (const std::string_view marker : kPirateMarkers) {
        const std::filesystem::path file = gameDirectory / marker;

        if (std::filesystem::exists(file, ec)) {
            found.push_back(Found{
                .weight = Weight::Blocking,
                .name = "a pirated copy",
                .file = file,
                .advice = "oxyMP needs a licensed copy: this one never reaches the session"});
        }
    }

    for (const Conflict& conflict : kConflicts) {
        const std::filesystem::path file = gameDirectory / conflict.file;

        if (!std::filesystem::exists(file, ec)) {
            continue;
        }

        // Версия спрашивается только там, где она что-то решает. У `OpenIV` и
        // `commandline.txt` её нет и быть не может — мешает само их наличие.
        if (!conflict.since.empty()) {
            const std::string version = versionOf(file);

            // Версии нет вовсе — значит это не тот мод, а чей-то ещё файл с тем
            // же именем. Обвинять его не в чем: скажем, но не запретим.
            if (version.empty()) {
                found.push_back(Found{.weight = Weight::Warning,
                                      .name = std::string{conflict.name},
                                      .file = file,
                                      .advice = "this file has no version; if the game misbehaves, "
                                                "try removing it"});
                continue;
            }

            if (!older(version, conflict.since)) {
                continue;
            }

            found.push_back(Found{
                .weight = Weight::Blocking,
                .name = std::format("{} {}", conflict.name, version),
                .file = file,
                .advice = std::string{conflict.advice}});
            continue;
        }

        found.push_back(Found{.weight = Weight::Blocking,
                              .name = std::string{conflict.name},
                              .file = file,
                              .advice = std::string{conflict.advice}});
    }

    return found;
}

bool GameConflicts::blocked(const std::vector<Found>& found) {
    return std::ranges::any_of(
        found, [](const Found& each) { return each.weight == Weight::Blocking; });
}

} // namespace oxymp::launcher
