#include "bindings.hpp"

#include "convert.hpp"
#include "resource.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace oxymp::client::js {
namespace {

[[nodiscard]] Resource& resourceOf(v8::Isolate* isolate) {
    // Изолят заводим мы сами и сами же кладём в него ресурс. Пустота здесь
    // означала бы, что обработчик позвали из чужого изолята, — а это не ошибка
    // скрипта, а поломка в нас.
    return *Resource::of(isolate);
}

void fail(v8::Isolate* isolate, std::string_view reason) {
    isolate->ThrowException(v8::Exception::TypeError(toJs(isolate, reason)));
}

/// Собирает доводы в одну строку через пробел.
[[nodiscard]] std::string joinArguments(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    std::string line;

    for (int i = 0; i < info.Length(); ++i) {
        if (i != 0) {
            line += ' ';
        }
        line += fromJs(isolate, info[i]);
    }

    return line;
}

template<OxympJsLogLevel Level>
void log(const v8::FunctionCallbackInfo<v8::Value>& info) {
    resourceOf(info.GetIsolate()).log(Level, joinArguments(info));
}

void onEvent(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    if (info.Length() < 2 || !info[1]->IsFunction()) {
        fail(isolate, "on ждёт имя события и обработчик");
        return;
    }

    resourceOf(isolate).subscribe(fromJs(isolate, info[0]), info[1].As<v8::Function>());
}

/// Именованное событие серверу.
void emitServer(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    if (info.Length() < 1) {
        fail(isolate, "emitServer ждёт имя события");
        return;
    }

    Resource& resource = resourceOf(isolate);
    const OxympJsHost& host = resource.host();

    if (host.emitServer == nullptr) {
        return;
    }

    const std::string name = fromJs(isolate, info[0]);
    const std::string payload = info.Length() >= 2 ? fromJs(isolate, info[1]) : std::string{};

    host.emitServer(host.context, toAbi(name),
                    OxympJsBytes{reinterpret_cast<const std::uint8_t*>(payload.data()),
                                 static_cast<std::uint32_t>(payload.size())});
}

// --- Нативы -----------------------------------------------------------------

/// Вызов натива игры.
///
/// Доводы приходят уже приведёнными к ячейкам: слой на JavaScript знает, что
/// натив ждёт целое, дробное или строку, и кладёт биты сам. Здесь остаётся
/// перенести их через границу.
///
/// Так, а не с разбором подписи натива в C++, потому что подписей этих несколько
/// тысяч и живут они в порождённой таблице у слоя. Держать вторую такую таблицу
/// здесь значило бы обязаться поддерживать их в согласии вечно.
void callNative(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    if (info.Length() < 2 || !info[1]->IsArray()) {
        fail(isolate, "callNative ждёт хеш и массив ячеек");
        return;
    }

    Resource& resource = resourceOf(isolate);
    const OxympJsHost& host = resource.host();

    if (host.callNative == nullptr) {
        fail(isolate, "нативы этому клиенту недоступны");
        return;
    }

    // Хеш натива не помещается в double без потери: он шестнадцатеричный и
    // старшие разряды у него значащие. BigInt здесь обязателен.
    std::uint64_t hash = 0;
    if (info[0]->IsBigInt()) {
        hash = static_cast<std::uint64_t>(info[0].As<v8::BigInt>()->Uint64Value());
    } else {
        double asNumber = 0.0;
        if (!info[0]->NumberValue(context).To(&asNumber)) {
            fail(isolate, "хеш натива непонятен");
            return;
        }
        hash = static_cast<std::uint64_t>(asNumber);
    }

    const v8::Local<v8::Array> cells = info[1].As<v8::Array>();
    const std::uint32_t count = cells->Length();

    if (count > OXYMP_JS_NATIVE_CELLS) {
        fail(isolate, "нативу передано больше доводов, чем он вмещает");
        return;
    }

    std::array<std::uint64_t, OXYMP_JS_NATIVE_CELLS> arguments{};

    for (std::uint32_t i = 0; i < count; ++i) {
        v8::Local<v8::Value> cell;
        if (!cells->Get(context, i).ToLocal(&cell)) {
            fail(isolate, "ячейка довода не читается");
            return;
        }

        if (cell->IsBigInt()) {
            arguments[i] = static_cast<std::uint64_t>(cell.As<v8::BigInt>()->Uint64Value());
        } else {
            double asNumber = 0.0;
            (void)cell->NumberValue(context).To(&asNumber);
            arguments[i] = static_cast<std::uint64_t>(static_cast<std::int64_t>(asNumber));
        }
    }

    std::array<std::uint64_t, OXYMP_JS_NATIVE_CELLS> results{};

    if (host.callNative(host.context, hash, arguments.data(), count, results.data(),
                        OXYMP_JS_NATIVE_CELLS) == 0) {
        // Отказ, а не пустой ответ: натив не разрешился в адрес, и вернуть ноль
        // значило бы выдать «не нашли» за «вернул ноль».
        info.GetReturnValue().SetNull();
        return;
    }

    // Ответ отдаётся ячейками — тем же, чем и принимался. Толкует их слой на
    // JavaScript, который один и знает подпись натива.
    const v8::Local<v8::Array> answer = v8::Array::New(isolate, OXYMP_JS_NATIVE_CELLS);

    for (std::uint32_t i = 0; i < OXYMP_JS_NATIVE_CELLS; ++i) {
        (void)answer->Set(context, i, v8::BigInt::NewFromUnsigned(isolate, results[i]));
    }

    info.GetReturnValue().Set(answer);
}

// --- Окна интерфейса --------------------------------------------------------

void createWebView(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    Resource& resource = resourceOf(isolate);
    const OxympJsHost& host = resource.host();

    if (host.createWebView == nullptr) {
        fail(isolate, "окна интерфейса этому клиенту недоступны");
        return;
    }

    const std::string url = info.Length() >= 1 ? fromJs(isolate, info[0]) : std::string{};

    info.GetReturnValue().Set(
        host.createWebView(host.context, toAbi(resource.name()), toAbi(url)));
}

/// Достаёт номер окна первым доводом. Ноль — не назван.
[[nodiscard]] std::uint32_t viewOf(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length() < 1) {
        return 0;
    }

    double asNumber = 0.0;
    (void)info[0]->NumberValue(info.GetIsolate()->GetCurrentContext()).To(&asNumber);

    return static_cast<std::uint32_t>(asNumber);
}

void destroyWebView(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const OxympJsHost& host = resourceOf(info.GetIsolate()).host();

    if (host.destroyWebView != nullptr) {
        host.destroyWebView(host.context, viewOf(info));
    }
}

void emitWebView(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const OxympJsHost& host = resourceOf(isolate).host();

    if (host.emitWebView == nullptr || info.Length() < 2) {
        return;
    }

    const std::string name = fromJs(isolate, info[1]);
    const std::string payload = info.Length() >= 3 ? fromJs(isolate, info[2]) : std::string{};

    host.emitWebView(host.context, viewOf(info), toAbi(name),
                     OxympJsBytes{reinterpret_cast<const std::uint8_t*>(payload.data()),
                                  static_cast<std::uint32_t>(payload.size())});
}

void setWebViewVisible(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const OxympJsHost& host = resourceOf(info.GetIsolate()).host();

    if (host.setWebViewVisible != nullptr) {
        host.setWebViewVisible(host.context, viewOf(info),
                               info.Length() >= 2 && info[1]->BooleanValue(info.GetIsolate()));
    }
}

void setWebViewFocused(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const OxympJsHost& host = resourceOf(info.GetIsolate()).host();

    if (host.setWebViewFocused != nullptr) {
        host.setWebViewFocused(host.context, viewOf(info),
                               info.Length() >= 2 && info[1]->BooleanValue(info.GetIsolate()));
    }
}

void addFunction(v8::Local<v8::Context> context, const v8::Local<v8::Object>& object,
                 std::string_view name, v8::FunctionCallback callback) {
    v8::Isolate* const isolate = context->GetIsolate();

    (void)object->Set(context, toJs(isolate, name),
                      v8::Function::New(context, callback).ToLocalChecked());
}

} // namespace

void installBindings(Resource& resource, v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();

    const v8::Local<v8::Object> native = v8::Object::New(isolate);

    addFunction(context, native, "on", onEvent);
    addFunction(context, native, "emitServer", emitServer);

    addFunction(context, native, "log", log<kOxympJsLogInfo>);
    addFunction(context, native, "logWarning", log<kOxympJsLogWarning>);
    addFunction(context, native, "logError", log<kOxympJsLogError>);

    addFunction(context, native, "callNative", callNative);

    addFunction(context, native, "createWebView", createWebView);
    addFunction(context, native, "destroyWebView", destroyWebView);
    addFunction(context, native, "emitWebView", emitWebView);
    addFunction(context, native, "setWebViewVisible", setWebViewVisible);
    addFunction(context, native, "setWebViewFocused", setWebViewFocused);

    (void)native->Set(context, toJs(isolate, "resourceName"), toJs(isolate, resource.name()));
    (void)native->Set(context, toJs(isolate, "resourcePath"),
                      toJs(isolate, resource.root().string()));

    const v8::Local<v8::Object> internals = v8::Object::New(isolate);
    (void)internals->Set(context, toJs(isolate, "native"), native);

    (void)context->Global()->Set(context, toJs(isolate, "__oxympAlt"), internals);
}

} // namespace oxymp::client::js
