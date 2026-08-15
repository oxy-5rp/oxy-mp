#include "node_library.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

namespace oxymp::script::js {

#ifdef _WIN32
namespace {

constexpr const wchar_t* kLibrary = L"libnode.dll";

/// Где движок лежит относительно исполняемого файла.
///
/// Тот же путь знает и сборка (OXYMP_NODE_SUBDIRECTORY в cmake/node.cmake), и
/// раскладка по каталогам раздачи. Разъехаться им нельзя: разъехавшись, они дают
/// сервер, который собрался, разложился и не нашёл движка.
constexpr const wchar_t* kModules = L"modules";
constexpr const wchar_t* kModuleName = L"js";

/// Каталог с исполняемым файлом.
///
/// Не текущий каталог: сервер запускают откуда угодно — ярлыком, службой, из
/// другой папки, — и modules/js лежит рядом с ним, а не рядом с тем, кто его
/// позвал.
[[nodiscard]] std::filesystem::path executableDirectory() {
    std::wstring path(MAX_PATH, L'\0');

    for (;;) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

        if (written == 0) {
            return {};
        }

        if (written < path.size()) {
            path.resize(written);
            break;
        }

        path.resize(path.size() * 2);
    }

    return std::filesystem::path{path}.parent_path();
}

} // namespace

bool ensureNodeLibrary(std::string& error) {
    // Уже в процессе — больше делать нечего. Так бывает при повторном подъёме
    // движка: сам Node поднимается раз на процесс, но спросить об этом нас могут
    // и дважды.
    if (::GetModuleHandleW(kLibrary) != nullptr) {
        return true;
    }

    const std::filesystem::path directory = executableDirectory();
    if (directory.empty()) {
        error = "не удалось узнать, где лежит сам сервер";
        return false;
    }

    const std::filesystem::path library = directory / kModules / kModuleName / kLibrary;

    std::error_code ec;
    if (!std::filesystem::exists(library, ec)) {
        error = std::format("движка скриптов нет: {} не найден", library.string());
        return false;
    }

    // Признаки поиска обязательны, и каждый нужен по своему поводу.
    //
    // DLL_LOAD_DIR — чтобы зависимости самого движка искались в его каталоге:
    // положенное туда рядом с ним и должно находиться.
    // DEFAULT_DIRS — чтобы там же нашлись библиотеки Visual C++, лежащие рядом
    // с сервером, и системные из System32.
    //
    // Без признаков вовсе Windows искала бы по старым правилам, среди которых
    // есть и текущий каталог: библиотеку с таким именем можно было бы подсунуть
    // серверу, запустив его из чужой папки.
    const HMODULE handle = ::LoadLibraryExW(library.c_str(), nullptr,
                                            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);

    if (handle == nullptr) {
        error = std::format("движок скриптов не загрузился ({}): код ошибки Windows {}",
                            library.string(), ::GetLastError());
        return false;
    }

    spdlog::debug("движок скриптов подгружен: {}", library.string());
    return true;
}

#else

bool ensureNodeLibrary(std::string& /*error*/) {
    // Подгрузка по требованию — свойство Windows и её отложенных ссылок. Там,
    // где сборка обходится без них, библиотеку разрешает сам загрузчик.
    return true;
}

#endif

} // namespace oxymp::script::js
