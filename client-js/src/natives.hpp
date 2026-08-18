#pragma once

#include <v8.h>

namespace oxymp::client::js {

/// Вызов натива игры из скрипта.
///
/// Ждёт хеш, подпись и доводы. Подпись приходит из порождённой таблицы
/// (`client-js/js/alt_natives_table.js`), а не разбирается здесь: нативов пять
/// тысяч, и вторая такая таблица в C++ означала бы обязанность держать две в
/// согласии вечно. Здесь остаётся перевод — то, чего на JavaScript не сделать:
/// биты дробного, указатель на строку, место под выходной довод.
void callNative(const v8::FunctionCallbackInfo<v8::Value>& info);

} // namespace oxymp::client::js
