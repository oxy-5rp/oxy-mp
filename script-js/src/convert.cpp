#include "convert.hpp"

#include <cmath>

namespace oxymp::script::js {

std::string fromJs(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    if (value.IsEmpty()) {
        return {};
    }

    v8::Local<v8::String> text;
    if (!value->ToString(isolate->GetCurrentContext()).ToLocal(&text)) {
        return {};
    }

    // Длина в байтах, а не в знаках: строка UTF-16 в UTF-8 может стать длиннее
    // втрое, и места нужно ровно столько, сколько скажет сам движок.
    const std::size_t size = text->Utf8LengthV2(isolate);

    std::string collected(size, '\0');
    if (size != 0) {
        text->WriteUtf8V2(isolate, collected.data(), collected.size());
    }

    return collected;
}

namespace {

/// Число из свойства объекта. Пусто — свойства нет или оно не число.
[[nodiscard]] std::optional<double> numberAt(v8::Local<v8::Context> context,
                                             v8::Local<v8::Object> object,
                                             std::string_view name) {
    v8::Local<v8::Value> value;
    if (!object->Get(context, toJs(context->GetIsolate(), name)).ToLocal(&value)) {
        return std::nullopt;
    }

    // IsNumber, а не NumberValue: последний охотно превратит в число и строку, и
    // логическое значение, и пустоту. Координата, полученная приведением из
    // undefined, — это ноль, то есть точка посреди океана.
    if (!value->IsNumber()) {
        return std::nullopt;
    }

    double number = 0.0;
    if (!value->NumberValue(context).To(&number)) {
        return std::nullopt;
    }

    // NaN и бесконечность до игры доходить не должны: игра примет их молча, а
    // персонаж уедет туда, откуда его не вернуть.
    if (!std::isfinite(number)) {
        return std::nullopt;
    }

    return number;
}

} // namespace

std::optional<shared::Vec3> vec3FromJs(v8::Local<v8::Context> context,
                                       v8::Local<v8::Value> value) {
    if (value.IsEmpty() || !value->IsObject()) {
        return std::nullopt;
    }

    const v8::Local<v8::Object> object = value.As<v8::Object>();

    const std::optional<double> x = numberAt(context, object, "x");
    const std::optional<double> y = numberAt(context, object, "y");
    const std::optional<double> z = numberAt(context, object, "z");

    if (!x || !y || !z) {
        return std::nullopt;
    }

    return shared::Vec3{
        .x = static_cast<float>(*x),
        .y = static_cast<float>(*y),
        .z = static_cast<float>(*z),
    };
}

std::optional<std::int64_t> intFromJs(v8::Local<v8::Context> context,
                                      v8::Local<v8::Value> value) {
    if (value.IsEmpty() || !value->IsNumber()) {
        return std::nullopt;
    }

    double number = 0.0;
    if (!value->NumberValue(context).To(&number) || !std::isfinite(number)) {
        return std::nullopt;
    }

    return static_cast<std::int64_t>(number);
}

} // namespace oxymp::script::js
