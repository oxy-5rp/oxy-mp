#pragma once

#include <optional>
#include <string>

namespace oxymp::launcher {

/// Читает строковое значение из HKEY_LOCAL_MACHINE.
///
/// Отдельный модуль, потому что через реестр разыскиваются две независимые
/// вещи — установленная игра и лаунчер Rockstar, — а обвязка над RegGetValueW
/// у них одна и та же.
[[nodiscard]] std::optional<std::wstring> readLocalMachineString(const wchar_t* path,
                                                                 const wchar_t* name);

} // namespace oxymp::launcher
