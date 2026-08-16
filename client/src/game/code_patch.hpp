#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace oxymp::client::game {

/// Правка машинного кода игры на месте.
///
/// Отдельно от MinHook, и это не дублирование. MinHook подменяет функцию
/// целиком: он читает её пролог, переносит его в трамплин и ставит переход. Здесь
/// же правится середина чужой функции — шесть байт записи состояния, пять байт
/// вызова, — и никакого пролога там нет.
///
/// Общее у всех таких правок одно: наш код лежит от игры дальше, чем достаёт
/// `call rel32`, и добраться до него можно только через переходник, выделенный
/// рядом с самой игрой.
namespace code {

/// Пишет байты в код игры, открыв его на запись и закрыв обратно.
[[nodiscard]] bool write(void* where, const void* what, std::size_t size, std::string& error);

/// Выделяет исполняемый клочок памяти в пределах досягаемости `call rel32`.
///
/// Возвращает nullptr, если рядом с игрой не нашлось свободного места. Освобождать
/// нужно `release`.
[[nodiscard]] void* allocateNear(std::uintptr_t anchor, std::size_t size);

/// Освобождает выделенное allocateNear.
void release(void* memory) noexcept;

/// Делает рядом с anchor переходник, безусловно прыгающий на target.
///
/// Нужен затем, что `call rel32` дотягивается только на два гигабайта, а наш
/// модуль Windows кладёт в память где ей вздумается — рядом с игрой он
/// оказывается разве что случайно.
[[nodiscard]] void* makeThunk(std::uintptr_t anchor, const void* target, std::string& error);

/// Уводит уже существующий вызов `call rel32` на свою функцию.
///
/// site указывает на байт `E8`. Прежняя цель вызова возвращается через original:
/// почти всегда подменивший обязан позвать то, что подменил.
[[nodiscard]] bool redirectCall(std::uint8_t* site, const void* target, void** original,
                                std::string& error);

} // namespace code
} // namespace oxymp::client::game
