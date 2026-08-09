#pragma once

#include "pattern.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace oxymp::memscan {

/// Непрерывный блок байт для поиска.
///
/// Намеренно не привязан к источнику: одинаково работает и по файлу, прочитанному
/// с диска, и по секции модуля, загруженного в память процесса.
using ByteView = std::span<const std::uint8_t>;

/// Все вхождения сигнатуры. Результат — смещения от начала блока.
///
/// limit ограничивает число находок; 0 означает «искать все». Ограничение полезно,
/// когда нужно лишь понять, однозначна сигнатура или нет: считать все совпадения
/// в 40-мегабайтной секции ради этого незачем.
[[nodiscard]] std::vector<std::size_t> findAll(ByteView haystack, const Pattern& pattern,
                                               std::size_t limit = 0);

/// Первое вхождение либо nullopt.
[[nodiscard]] std::optional<std::size_t> findFirst(ByteView haystack, const Pattern& pattern);

} // namespace oxymp::memscan
