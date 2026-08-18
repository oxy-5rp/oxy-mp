#include "node_library.hpp"

#include <windows.h>

#include <filesystem>

namespace oxymp::client::js {
namespace {

constexpr const wchar_t* kLibrary = L"libnode.dll";

/// Каталог, в котором лежит эта самая DLL.
///
/// Именно эта, а не исполняемый файл, и разница здесь принципиальная — в отличие
/// от сервера, где годится и то и другое. Машина клиента живёт внутри процесса
/// игры, и исполняемый файл для неё — `GTA5.exe`, лежащий в папке игры. Спроси
/// мы про него, как это делает сервер (`script-js/src/node_library.cpp`), — и
/// движок искался бы в папке игры, куда oxyMP не кладёт ничего вовсе.
///
/// Собственный адрес берётся по указателю на эту же функцию: другого способа
/// узнать свой модуль изнутри DLL нет, а хранить его из DllMain значило бы
/// заводить состояние ради того, что и так известно.
[[nodiscard]] std::filesystem::path ownDirectory() {
    HMODULE self = nullptr;

    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&ownDirectory), &self) == 0) {
        return {};
    }

    std::wstring path(MAX_PATH, L'\0');

    for (;;) {
        const DWORD written = ::GetModuleFileNameW(self, path.data(),
                                                   static_cast<DWORD>(path.size()));

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

bool ensureNodeLibrary() {
    // Уже в процессе — больше делать нечего.
    //
    // Так бывает не только при повторном обращении, но и когда движок уже
    // подняли для чего-то другого. Загрузить его вторым экземпляром нельзя:
    // Node поднимается один раз на процесс.
    if (::GetModuleHandleW(kLibrary) != nullptr) {
        return true;
    }

    const std::filesystem::path directory = ownDirectory();
    if (directory.empty()) {
        return false;
    }

    const std::filesystem::path library = directory / kLibrary;

    std::error_code failure;
    if (!std::filesystem::is_regular_file(library, failure)) {
        return false;
    }

    // Полным путём, а не именем: имя Windows искала бы рядом с `GTA5.exe`, где
    // движка нет и не будет — в папку игры oxyMP не кладёт ничего.
    //
    // LOAD_WITH_ALTERED_SEARCH_PATH заставляет её искать зависимости самой
    // библиотеки рядом с ней же, а не рядом с игрой. Без этого движок,
    // найденный по полному пути, всё равно не поднялся бы: свои зависимости он
    // тянет из своего каталога.
    return ::LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) != nullptr;
}

} // namespace oxymp::client::js
