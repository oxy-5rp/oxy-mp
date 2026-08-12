# Превращает текстовый файл в исходник C++ с его содержимым.
#
# Запускается через cmake -P на этапе сборки, а не настройки: иначе правка
# страницы не попадала бы в сборку до повторного запуска cmake, а это ровно тот
# сорт неожиданностей, из-за которых потом ищут ошибку не там.
#
# Ожидает EMBED_INPUT, EMBED_OUTPUT, EMBED_SYMBOL, EMBED_NAMESPACE.

file(READ "${EMBED_INPUT}" hex HEX)

# Байты, а не строковый литерал: в тексте есть и кавычки, и обратные косые, и
# переводы строк, и экранировать всё это значило бы завести второй язык поверх
# первого. Байты не требуют ничего.
#
# Приведение к char выписано явно: в тексте есть кириллица, а её байты в UTF-8
# больше 0x7F и в знаковый char не помещаются. Без приведения это сужение, и
# компилятор справедливо ругается на каждый такой байт.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "static_cast<char>(0x\\1)," bytes "${hex}")

get_filename_component(name "${EMBED_INPUT}" NAME)

file(WRITE "${EMBED_OUTPUT}"
"// Порождено из ${name}. Правьте исходный файл, а не этот.
#pragma once

#include <string_view>

namespace ${EMBED_NAMESPACE} {

inline constexpr char ${EMBED_SYMBOL}Bytes[] = {${bytes}};

inline constexpr std::string_view ${EMBED_SYMBOL}{${EMBED_SYMBOL}Bytes,
                                                 sizeof(${EMBED_SYMBOL}Bytes)};

} // namespace ${EMBED_NAMESPACE}
")
