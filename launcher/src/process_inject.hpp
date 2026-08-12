#pragma once

#include <filesystem>
#include <string>

namespace oxymp::launcher {

/// Права, которых достаточно для внедрения и наблюдения за процессом.
inline constexpr unsigned long kInjectAccess = 0x0002 /* CREATE_THREAD */
                                               | 0x0400 /* QUERY_INFORMATION */
                                               | 0x0008 /* VM_OPERATION */
                                               | 0x0020 /* VM_WRITE */
                                               | 0x0010 /* VM_READ */
                                               | 0x00100000 /* SYNCHRONIZE */;

/// Загружает наш модуль в чужой процесс.
///
/// Отдельный модуль, потому что внедрять приходится в двоих: в игру — клиент, в
/// Rockstar Games Launcher — подмену звена BattlEye. Разойдись эти два пути,
/// одну и ту же ошибку пришлось бы чинить дважды.
///
/// Способ самый обычный: путь к модулю записывается в память чужого процесса, и
/// там же создаётся поток на LoadLibraryW. Адрес LoadLibraryW одинаков во всех
/// процессах сеанса, потому что kernel32 загружается по одной и той же базе.
[[nodiscard]] bool injectModule(void* process, const std::filesystem::path& module,
                                std::string& error);

} // namespace oxymp::launcher
