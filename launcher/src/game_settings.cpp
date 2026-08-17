#include "game_settings.hpp"

#include <spdlog/spdlog.h>

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include <windows.h>

#include <shlobj.h>

namespace oxymp::launcher {
namespace {

/// Как называются каталоги настроек у разных изданий игры.
///
/// «GTA V» — обычное, «GTA V Legacy» — то, во что Rockstar переименовала
/// прежнюю версию, выпустив улучшенную. Перебираются оба: какая версия стоит у
/// игрока, лаунчер не знает, а лишний несуществующий путь ничего не стоит.
constexpr const wchar_t* kSettingsFolders[] = {
    L"Rockstar Games\\GTA V\\settings.xml",
    L"Rockstar Games\\GTA V Legacy\\settings.xml",
};

/// Поле, которым игра хранит оконный режим.
///
/// Ищется без учёта регистра, и это исправление, стоившее всей затеи. Написано
/// оно было строчными, а игра пишет `<Windowed value="2" />` — с заглавной, — и
/// поле не находилось ни разу. В журнале это выглядело безобидной строкой «в
/// настройках игры нет поля оконного режима», а на деле означало, что
/// безрамочный режим не выставлялся никогда: игра шла обычным окном с рамкой,
/// теряла вид при сворачивании, а слой интерфейса поверх полноэкранной игры не
/// показывался вовсе.
constexpr std::string_view kField = "<windowed value=\"";

/// Ищет подстроку без учёта регистра.
///
/// Своими руками, а не через готовое: сравнение без учёта регистра в
/// стандартной библиотеке есть только для строк целиком, а нам нужно место
/// внутри файла.
[[nodiscard]] std::size_t findIgnoringCase(std::string_view where, std::string_view what) {
    if (what.empty() || where.size() < what.size()) {
        return std::string::npos;
    }

    const auto lower = [](char symbol) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(symbol)));
    };

    for (std::size_t at = 0; at + what.size() <= where.size(); ++at) {
        std::size_t matched = 0;
        while (matched < what.size() && lower(where[at + matched]) == lower(what[matched])) {
            ++matched;
        }

        if (matched == what.size()) {
            return at;
        }
    }

    return std::string::npos;
}

std::filesystem::path documentsDirectory() {
    PWSTR path = nullptr;

    // Именно известная папка, а не USERPROFILE\Documents: документы часто
    // переставлены — в OneDrive или на другой диск, — и собранный вручную путь
    // указал бы в пустоту.
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) {
        return {};
    }

    std::filesystem::path documents{path};
    ::CoTaskMemFree(path);

    return documents;
}

std::filesystem::path findSettings() {
    const std::filesystem::path documents = documentsDirectory();
    if (documents.empty()) {
        return {};
    }

    for (const wchar_t* candidate : kSettingsFolders) {
        std::filesystem::path path = documents / candidate;

        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return path;
        }
    }

    return {};
}

} // namespace

bool preferBorderlessWindow(const std::filesystem::path& backupDirectory, std::string& note) {
    const std::filesystem::path settings = findSettings();
    if (settings.empty()) {
        note = "настройки игры не найдены — игра ещё ни разу не запускалась";
        return true;
    }

    std::string contents;
    {
        std::ifstream file(settings, std::ios::binary);
        if (!file) {
            note = "настройки игры не читаются";
            return false;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        contents = std::move(buffer).str();
    }

    const std::size_t field = findIgnoringCase(contents, kField);
    if (field == std::string::npos) {
        note = "в настройках игры нет поля оконного режима";
        return true;
    }

    const std::size_t valueAt = field + kField.size();
    const std::size_t valueEnd = contents.find('"', valueAt);
    if (valueEnd == std::string::npos) {
        note = "поле оконного режима испорчено";
        return false;
    }

    int current = -1;
    std::from_chars(contents.data() + valueAt, contents.data() + valueEnd, current);

    if (current == static_cast<int>(WindowMode::Borderless)) {
        note = "игра уже в оконном режиме без рамки";
        return true;
    }

    // Копия делается до правки и только один раз: повторный запуск не должен
    // затирать настоящее значение уже подменённым.
    std::error_code ec;
    std::filesystem::create_directories(backupDirectory, ec);

    const std::filesystem::path backup = backupDirectory / "settings.xml";
    if (!std::filesystem::exists(backup, ec)) {
        std::filesystem::copy_file(settings, backup, ec);

        if (ec) {
            spdlog::warn("копия настроек игры не сделана: {}", ec.message());
        }
    }

    contents.replace(valueAt, valueEnd - valueAt,
                     std::to_string(static_cast<int>(WindowMode::Borderless)));

    std::ofstream file(settings, std::ios::binary | std::ios::trunc);
    if (!file) {
        note = "настройки игры не перезаписываются";
        return false;
    }

    file << contents;
    if (!file) {
        note = "настройки игры записались не полностью";
        return false;
    }

    note = "игра переведена в оконный режим без рамки (было " + std::to_string(current) +
           "), копия в backup";

    spdlog::info("оконный режим игры: {} → {}, копия {}", current,
                 static_cast<int>(WindowMode::Borderless), backup.string());

    return true;
}

} // namespace oxymp::launcher
