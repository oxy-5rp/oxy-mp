#pragma once

// Перевод между нашими типами и типами V8.
//
// Собран в одном месте намеренно. Разбросанные по привязкам, эти три строки
// повторялись бы в каждом обработчике, и всякий писал бы их чуть по-своему —
// кто-то с проверкой на пустоту, кто-то без.

#include <oxymp/shared/math/vec3.hpp>

#include <v8.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oxymp::script::js {

/// Строка в V8.
///
/// kNormal, а не kInternalized: строки здесь по большей части одноразовые —
/// имена событий, реплики чата, — и заносить каждую в таблицу постоянных строк
/// движка значило бы копить их до конца сессии.
[[nodiscard]] inline v8::Local<v8::String> toJs(v8::Isolate* isolate, std::string_view text) {
    return v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal,
                                   static_cast<int>(text.size()))
        .ToLocalChecked();
}

/// Строка из V8. Всё, что строкой не является, приводится к ней по правилам JS.
///
/// Приведение здесь уместно: скрипт вправе передать в чат число, и требовать от
/// него `String(x)` значило бы придираться. Толковать так же число как имя
/// события — тоже не беда: имя всё равно сравнивается по строке.
///
/// Написано вручную, а не через `v8::String::Utf8Value`, и это не вкусовщина.
/// Тот объявлен экспортируемым из библиотеки, но определён прямо в заголовке:
/// подключая Node отдельной библиотекой, его тело получаешь дважды — своё и
/// импортированное, — и линковщик отказывается выбирать (LNK2005). Здесь же
/// используется то, что живёт только в библиотеке.
[[nodiscard]] std::string fromJs(v8::Isolate* isolate, v8::Local<v8::Value> value);

/// Точка в пространстве — обычным объектом с полями x, y, z.
///
/// Своего класса Vector3 здесь нет, и это осознанно. У alt:V он есть и хорош
/// тем, что умеет складываться и вычитаться; но пока ни одна привязка не просит
/// у скрипта вектор обратно с сохранением типа, класс дал бы только лишний
/// уровень, который надо заводить, беречь от подмены и проверять.
[[nodiscard]] inline v8::Local<v8::Object> toJs(v8::Local<v8::Context> context,
                                                 const shared::Vec3& point) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::Object> object = v8::Object::New(isolate);

    // Значения кладутся без проверки успеха: объект только что создан, он не
    // чужой, у него нет ни перехватчиков, ни защищённых полей — отказать здесь
    // может только исчерпанная память, а её обработает сам V8.
    (void)object->Set(context, toJs(isolate, "x"), v8::Number::New(isolate, point.x));
    (void)object->Set(context, toJs(isolate, "y"), v8::Number::New(isolate, point.y));
    (void)object->Set(context, toJs(isolate, "z"), v8::Number::New(isolate, point.z));

    return object;
}

/// Точка из объекта со свойствами x, y, z.
///
/// Пусто — значит переданное на точку не похоже. Отсутствующее поле не
/// подменяется нулём: «забыл z» и «хотел z равный нулю» — разные намерения, и
/// молча превращать первое во второе значит ронять машину под карту.
[[nodiscard]] std::optional<shared::Vec3> vec3FromJs(v8::Local<v8::Context> context,
                                                      v8::Local<v8::Value> value);

/// Целое из значения. Пусто — если числом оно не является.
[[nodiscard]] std::optional<std::int64_t> intFromJs(v8::Local<v8::Context> context,
                                                     v8::Local<v8::Value> value);

} // namespace oxymp::script::js
