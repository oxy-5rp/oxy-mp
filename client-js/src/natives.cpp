#include "natives.hpp"

#include "convert.hpp"
#include "resource.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace oxymp::client::js {
namespace {

/// Ячейки, в которые натив пишет ответ через указатель.
///
/// Свои, а не выделяемые на каждый вызов: нативы зовут тысячами в секунду, и
/// выделение памяти под каждый выходной довод стоило бы дороже самого вызова.
/// Одного набора хватает, потому что вызов не бывает вложенным — движок
/// однопоточный, и пока натив не вернулся, второго вызова из скрипта не будет.
struct Scratch {
    /// По четыре ячейки на довод: вектору нужно три подряд плюс выравнивание.
    static constexpr std::size_t kCellsPerArgument = 4;

    std::array<std::uint64_t, OXYMP_JS_NATIVE_CELLS * kCellsPerArgument> cells{};
};

Scratch& scratch() {
    static Scratch instance;
    return instance;
}

/// Биты дробного числа в ячейке.
///
/// Именно биты, а не значение: игра читает ячейку как float, а не приводит её.
/// Положи мы туда 1 целым — натив прочёл бы его как 1.4e-45.
[[nodiscard]] std::uint64_t floatBits(double value) {
    const auto narrowed = static_cast<float>(value);

    std::uint32_t bits = 0;
    std::memcpy(&bits, &narrowed, sizeof(bits));

    return bits;
}

[[nodiscard]] float floatFrom(std::uint64_t cell) {
    const auto bits = static_cast<std::uint32_t>(cell);

    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));

    return value;
}

[[nodiscard]] std::uint64_t integerFrom(v8::Local<v8::Context> context,
                                        v8::Local<v8::Value> value) {
    if (value->IsBigInt()) {
        return value.As<v8::BigInt>()->Uint64Value();
    }

    double number = 0.0;
    if (!value->NumberValue(context).To(&number)) {
        return 0;
    }

    // Через знаковое: дескрипторы игры бывают отрицательными (−1 означает
    // «нет»), и приведение прямо к беззнаковому дало бы огромное число.
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(number));
}

/// Указатель как ячейка.
[[nodiscard]] std::uint64_t pointerTo(const void* address) {
    return reinterpret_cast<std::uint64_t>(address);
}

} // namespace

void callNative(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    if (info.Length() < 2) {
        isolate->ThrowException(v8::Exception::TypeError(
            toJs(isolate, "callNative ждёт хеш и подпись")));
        return;
    }

    const OxympJsHost& host = Resource::of(isolate)->host();

    if (host.callNative == nullptr) {
        isolate->ThrowException(
            v8::Exception::Error(toJs(isolate, "нативы этому клиенту недоступны")));
        return;
    }

    const std::uint64_t hash = integerFrom(context, info[0]);
    const std::string signature = fromJs(isolate, info[1]);

    const std::size_t colon = signature.find(':');
    if (colon == std::string::npos) {
        isolate->ThrowException(
            v8::Exception::TypeError(toJs(isolate, "подпись натива без двоеточия")));
        return;
    }

    const std::string_view takes{signature.data(), colon};
    const char gives = colon + 1 < signature.size() ? signature[colon + 1] : 'n';

    if (takes.size() > OXYMP_JS_NATIVE_CELLS) {
        isolate->ThrowException(
            v8::Exception::TypeError(toJs(isolate, "у натива слишком много доводов")));
        return;
    }

    std::array<std::uint64_t, OXYMP_JS_NATIVE_CELLS> cells{};

    // Строки живут до конца вызова: в ячейку уходит указатель на их байты, и
    // освободись они раньше, натив прочёл бы чужую память.
    std::vector<std::string> strings;
    strings.reserve(takes.size());

    // Куда натив пишет выходные значения и в каком они порядке.
    struct Outgoing {
        char kind = 'L';
        std::size_t at = 0;
    };

    std::vector<Outgoing> outgoing;
    Scratch& room = scratch();

    for (std::size_t i = 0; i < takes.size(); ++i) {
        const v8::Local<v8::Value> given =
            static_cast<int>(i) + 2 < info.Length() ? info[static_cast<int>(i) + 2]
                                                    : v8::Local<v8::Value>{v8::Undefined(isolate)};

        switch (takes[i]) {
        case 'i':
        case 'I':
        case 'a':
            cells[i] = integerFrom(context, given);
            break;

        case 'f': {
            double number = 0.0;
            (void)given->NumberValue(context).To(&number);
            cells[i] = floatBits(number);
            break;
        }

        case 'b':
            cells[i] = given->BooleanValue(isolate) ? 1U : 0U;
            break;

        case 's': {
            // Пустота — это нулевой указатель, а не пустая строка. Нативы
            // различают их: одному «нет имени», другому «имя длиной ноль».
            if (given->IsNullOrUndefined()) {
                cells[i] = 0;
                break;
            }

            strings.push_back(fromJs(isolate, given));
            cells[i] = pointerTo(strings.back().c_str());
            break;
        }

        case 'v': {
            // Вектор уходит указателем на три дробных подряд.
            const std::size_t at = i * Scratch::kCellsPerArgument;

            double x = 0.0;
            double y = 0.0;
            double z = 0.0;

            if (given->IsObject()) {
                const v8::Local<v8::Object> object = given.As<v8::Object>();

                v8::Local<v8::Value> field;
                if (object->Get(context, toJs(isolate, "x")).ToLocal(&field)) {
                    (void)field->NumberValue(context).To(&x);
                }
                if (object->Get(context, toJs(isolate, "y")).ToLocal(&field)) {
                    (void)field->NumberValue(context).To(&y);
                }
                if (object->Get(context, toJs(isolate, "z")).ToLocal(&field)) {
                    (void)field->NumberValue(context).To(&z);
                }
            }

            room.cells[at] = floatBits(x);
            room.cells[at + 1] = floatBits(y);
            room.cells[at + 2] = floatBits(z);

            cells[i] = pointerTo(&room.cells[at]);
            break;
        }

        case 'L':
        case 'F':
        case 'B':
        case 'V': {
            // Выходной довод: натив получает указатель на пустое место и пишет
            // туда ответ. Место обнуляется заранее — натив, ничего не
            // написавший, иначе отдал бы мусор от прошлого вызова.
            const std::size_t at = i * Scratch::kCellsPerArgument;

            room.cells[at] = 0;
            room.cells[at + 1] = 0;
            room.cells[at + 2] = 0;

            cells[i] = pointerTo(&room.cells[at]);
            outgoing.push_back(Outgoing{takes[i], at});
            break;
        }

        default:
            isolate->ThrowException(v8::Exception::TypeError(
                toJs(isolate, "в подписи натива непонятная буква")));
            return;
        }
    }

    std::array<std::uint64_t, OXYMP_JS_NATIVE_CELLS> results{};

    if (host.callNative(host.context, hash, cells.data(), static_cast<std::uint32_t>(takes.size()),
                        results.data(), OXYMP_JS_NATIVE_CELLS) == 0) {
        // Отказ, а не пустой ответ: натив не разрешился в адрес. Ноль здесь
        // выдал бы «не нашли» за «натив вернул ноль».
        info.GetReturnValue().SetNull();
        return;
    }

    // Что натив вернул сам.
    v8::Local<v8::Value> answer = v8::Undefined(isolate);

    switch (gives) {
    case 'n':
        break;

    case 'i':
        // Через знаковое 32-разрядное: дескрипторы игры бывают отрицательными.
        answer = v8::Integer::New(isolate, static_cast<std::int32_t>(results[0]));
        break;

    case 'I':
        answer = v8::BigInt::NewFromUnsigned(isolate, results[0]);
        break;

    case 'f':
        answer = v8::Number::New(isolate, floatFrom(results[0]));
        break;

    case 'b':
        answer = v8::Boolean::New(isolate, (results[0] & 1U) != 0);
        break;

    case 's': {
        const auto* const text = reinterpret_cast<const char*>(results[0]);
        answer = text == nullptr ? v8::Local<v8::Value>{v8::Null(isolate)}
                                 : v8::Local<v8::Value>{toJs(isolate, text)};
        break;
    }

    case 'v': {
        // Вектор возвращается тремя ячейками подряд, по дробному в каждой.
        const v8::Local<v8::Object> point = v8::Object::New(isolate);

        (void)point->Set(context, toJs(isolate, "x"),
                         v8::Number::New(isolate, floatFrom(results[0])));
        (void)point->Set(context, toJs(isolate, "y"),
                         v8::Number::New(isolate, floatFrom(results[1])));
        (void)point->Set(context, toJs(isolate, "z"),
                         v8::Number::New(isolate, floatFrom(results[2])));

        answer = point;
        break;
    }

    default:
        break;
    }

    // Без выходных доводов ответ отдаётся как есть — так его и ждёт скрипт.
    if (outgoing.empty()) {
        info.GetReturnValue().Set(answer);
        return;
    }

    // С выходными доводами — списком, как это делает alt:V: сперва то, что
    // вернул сам натив, затем всё, что он записал по указателям. Натив, не
    // возвращающий ничего, своего места в списке не занимает.
    const bool withAnswer = gives != 'n';

    const v8::Local<v8::Array> list =
        v8::Array::New(isolate, static_cast<int>(outgoing.size() + (withAnswer ? 1U : 0U)));

    std::uint32_t at = 0;

    if (withAnswer) {
        (void)list->Set(context, at++, answer);
    }

    for (const Outgoing& out : outgoing) {
        v8::Local<v8::Value> value = v8::Undefined(isolate);

        switch (out.kind) {
        case 'L':
            value = v8::Integer::New(isolate, static_cast<std::int32_t>(room.cells[out.at]));
            break;

        case 'F':
            value = v8::Number::New(isolate, floatFrom(room.cells[out.at]));
            break;

        case 'B':
            value = v8::Boolean::New(isolate, (room.cells[out.at] & 1U) != 0);
            break;

        case 'V': {
            const v8::Local<v8::Object> point = v8::Object::New(isolate);

            (void)point->Set(context, toJs(isolate, "x"),
                             v8::Number::New(isolate, floatFrom(room.cells[out.at])));
            (void)point->Set(context, toJs(isolate, "y"),
                             v8::Number::New(isolate, floatFrom(room.cells[out.at + 1])));
            (void)point->Set(context, toJs(isolate, "z"),
                             v8::Number::New(isolate, floatFrom(room.cells[out.at + 2])));

            value = point;
            break;
        }

        default:
            break;
        }

        (void)list->Set(context, at++, value);
    }

    info.GetReturnValue().Set(list);
}

} // namespace oxymp::client::js
