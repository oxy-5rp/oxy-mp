#pragma once

#include <cstdint>
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

/// Читает строковое значение из HKEY_CURRENT_USER.
///
/// Steam записывает свой путь именно сюда: он ставится на пользователя, а не на
/// машину, и у разных людей за одним компьютером пути разные.
[[nodiscard]] std::optional<std::wstring> readCurrentUserString(const wchar_t* path,
                                                                const wchar_t* name);

/// Читает числовое значение из HKEY_CURRENT_USER.
///
/// Ради одного вопроса: вошёл ли кто-нибудь в Steam. Ответ на него Steam держит
/// числом, а не строкой, и читать его строковым запросом бесполезно —
/// RegGetValueW откажет по несовпадению разряда.
[[nodiscard]] std::optional<std::uint32_t> readCurrentUserNumber(const wchar_t* path,
                                                                 const wchar_t* name);

} // namespace oxymp::launcher
