#pragma once

// Перевод между типами границы и типами V8.
//
// Свой, а не общий со script-js, и это не недосмотр. Общего здесь ровно две
// строки — завести строку V8 и прочитать её обратно, — а всё остальное у сторон
// разное: серверу нужны снимки сущностей, клиенту — ячейки нативов и текст
// границы. Сведи мы это в одну библиотеку, она собрала бы в себе обе стороны
// сразу и стала бы местом, куда тянут всё, что не поместилось.

#include <oxymp/client/js/abi.h>

#include <v8.h>

#include <string>
#include <string_view>

namespace oxymp::client::js {

/// Строка в V8.
///
/// kNormal, а не kInternalized: строки здесь по большей части одноразовые —
/// имена событий, нагрузки, — и заносить каждую в таблицу постоянных строк
/// движка значило бы копить их до конца сессии.
[[nodiscard]] inline v8::Local<v8::String> toJs(v8::Isolate* isolate, std::string_view text) {
    return v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal,
                                   static_cast<int>(text.size()))
        .ToLocalChecked();
}

/// Часть методов V8 нельзя звать вовсе — они определены дважды.
///
/// `v8::Value::IsNullOrUndefined` и `v8::String::Utf8Value` объявлены
/// экспортируемыми из библиотеки, но определены прямо в заголовке. Подключая
/// Node отдельной библиотекой, их тело получаешь дважды — своё и
/// импортированное, — и линковщик отказывается выбирать (LNK2005).
///
/// Обходятся они своими помощниками: вместо `IsNullOrUndefined()` — isNothing
/// ниже, вместо `Utf8Value` — fromJs. Разобрать первый на `IsNull()` и
/// `IsUndefined()` не выйдет: они определены дважды точно так же.

/// Пусто ли значение — null, undefined или вовсе нет.
///
/// Через StrictEquals, а не через IsNull/IsUndefined: последние определены и в
/// заголовке, и в библиотеке, и линковщик отказывается выбирать между ними
/// (см. выше). StrictEquals же живёт только в библиотеке, и сравнение с
/// одиночками даёт ровно тот же ответ: в JavaScript `null === null` и
/// `undefined === undefined`.
[[nodiscard]] inline bool isNothing(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    return value.IsEmpty() || value->StrictEquals(v8::Null(isolate)) ||
           value->StrictEquals(v8::Undefined(isolate));
}

/// Строка из V8. Всё, что строкой не является, приводится к ней по правилам JS.
///
/// Написано вручную, а не через `v8::String::Utf8Value`, и это не вкусовщина.
/// Тот объявлен экспортируемым из библиотеки, но определён прямо в заголовке:
/// подключая Node отдельной библиотекой, его тело получаешь дважды — своё и
/// импортированное, — и линковщик отказывается выбирать (LNK2005).
[[nodiscard]] std::string fromJs(v8::Isolate* isolate, v8::Local<v8::Value> value);

/// Текст границы в строку.
///
/// Копией, а не видом на чужую память: текст с той стороны принадлежит
/// передающему и живёт до конца вызова, а нам он нужен дольше — хотя бы до
/// конца обработчика.
[[nodiscard]] inline std::string fromAbi(OxympJsText text) {
    return text.data == nullptr ? std::string{}
                                : std::string{text.data, static_cast<std::size_t>(text.length)};
}

/// Строка в текст границы.
///
/// Отдаётся видом на чужую строку, и потому она обязана пережить вызов. Здесь
/// это всегда так: текст уходит вниз по стеку, а не сохраняется.
[[nodiscard]] inline OxympJsText toAbi(std::string_view text) {
    return OxympJsText{text.data(), static_cast<std::uint32_t>(text.size())};
}

} // namespace oxymp::client::js
