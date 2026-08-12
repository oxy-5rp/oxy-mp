#pragma once

#include <oxymp/gamesig/image_source.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oxymp::sigcheck::peheaders {

struct Headers {
    /// База загрузки, записанная в самом образе.
    std::uint64_t imageBase = 0;
    std::vector<gamesig::Section> sections;
};

/// Разбирает заголовки PE из начала образа.
///
/// data должен начинаться с заголовка DOS и содержать таблицу секций целиком.
/// Для файла это его первые байты, для процесса — первая страница модуля:
/// заголовки лежат в начале и там, и там.
[[nodiscard]] std::optional<Headers> parse(memscan::ByteView data, std::string& error);

} // namespace oxymp::sigcheck::peheaders
