#include "bindings.hpp"

#include "convert.hpp"
#include "natives.hpp"
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
