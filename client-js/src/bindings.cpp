#include "bindings.hpp"

#include "convert.hpp"
#include "natives.hpp"
#include "resource.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
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

// --- Сущности сессии --------------------------------------------------------

/// Сколько сущностей помещается в буфер без обращения к куче.
///
/// Столько игроков в сессии не бывает почти никогда, а если бывает — список
/// спрашивается заново, с буфером по размеру. Молчаливого обрезания здесь нет:
/// обрезанный список выглядит как полный.
constexpr std::uint32_t kEntitiesOnStack = 256;

/// Род сущности, названный первым доводом.
[[nodiscard]] OxympJsEntityKind kindOf(const v8::FunctionCallbackInfo<v8::Value>& info) {
    double asNumber = 0.0;

    if (info.Length() >= 1) {
        (void)info[0]->NumberValue(info.GetIsolate()->GetCurrentContext()).To(&asNumber);
    }

    return static_cast<int>(asNumber) == kOxympJsEntityVehicle ? kOxympJsEntityVehicle
                                                               : kOxympJsEntityPlayer;
}

/// Целое, названное доводом под этим номером.
[[nodiscard]] std::int32_t numberAt(const v8::FunctionCallbackInfo<v8::Value>& info, int index) {
    double asNumber = 0.0;

    if (info.Length() > index) {
        (void)info[index]->NumberValue(info.GetIsolate()->GetCurrentContext()).To(&asNumber);
    }

    return static_cast<std::int32_t>(asNumber);
}

/// Сущности сессии этого рода — плоским списком «номер, тело, номер, тело».
///
/// Плоским, а не набором объектов, и это не скупость. Список этот ресурс
/// спрашивает каждый кадр, обходя игроков; набор из сотни маленьких объектов
/// означал бы сотню выделений в кадр, которые тут же станут мусором. Собрать
/// из плоского то, что нужно, — забота слоя на JavaScript, и он делает это
/// один раз на сущность, а не на каждый её опрос.
void sessionEntities(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const OxympJsHost& host = resourceOf(isolate).host();

    if (host.listEntities == nullptr) {
        info.GetReturnValue().Set(v8::Array::New(isolate, 0));
        return;
    }

    const OxympJsEntityKind kind = kindOf(info);

    std::array<OxympJsEntity, kEntitiesOnStack> room{};
    std::vector<OxympJsEntity> spare;

    OxympJsEntity* entities = room.data();
    std::uint32_t capacity = kEntitiesOnStack;
    std::uint32_t total = host.listEntities(host.context, kind, entities, capacity);

    // Не влезло — спрашиваем заново, теперь по размеру. Второй ответ может
    // оказаться и короче первого: сессия живёт между вызовами, и кто-то мог
    // выйти. Поэтому дальше берётся меньшее из двух.
    if (total > capacity) {
        spare.resize(total);
        entities = spare.data();
        capacity = total;
        total = host.listEntities(host.context, kind, entities, capacity);
    }

    const std::uint32_t shown = total < capacity ? total : capacity;

    const v8::Local<v8::Array> list = v8::Array::New(isolate, static_cast<int>(shown) * 2);

    for (std::uint32_t i = 0; i < shown; ++i) {
        (void)list->Set(context, i * 2, v8::Integer::New(isolate, entities[i].id));
        (void)list->Set(context, i * 2 + 1, v8::Integer::New(isolate, entities[i].handle));
    }

    info.GetReturnValue().Set(list);
}

/// Дескриптор игры по номеру сессии.
void sessionHandle(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const OxympJsHost& host = resourceOf(info.GetIsolate()).host();

    info.GetReturnValue().Set(host.entityHandle == nullptr
                                  ? 0
                                  : host.entityHandle(host.context, kindOf(info),
                                                      numberAt(info, 1)));
}

/// Номер сессии по дескриптору игры.
void sessionId(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const OxympJsHost& host = resourceOf(info.GetIsolate()).host();

    info.GetReturnValue().Set(host.entityId == nullptr
                                  ? -1
                                  : host.entityId(host.context, kindOf(info), numberAt(info, 1)));
}

/// Имя игрока сессии.
void playerName(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const OxympJsHost& host = resourceOf(isolate).host();

    if (host.playerName == nullptr) {
        info.GetReturnValue().Set(toJs(isolate, std::string_view{}));
        return;
    }

    // Строка принадлежит клиенту и жива до следующего вызова — поэтому она
    // копируется в V8 здесь же, а не запоминается указателем.
    const OxympJsText name = host.playerName(host.context, numberAt(info, 0));

    info.GetReturnValue().Set(toJs(isolate, fromAbi(name)));
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

    addFunction(context, native, "selfId", [](const v8::FunctionCallbackInfo<v8::Value>& info) {
        const OxympJsHost& host = Resource::of(info.GetIsolate())->host();

        info.GetReturnValue().Set(host.localPlayerId == nullptr
                                      ? -1
                                      : host.localPlayerId(host.context));
    });

    addFunction(context, native, "sessionEntities", sessionEntities);
    addFunction(context, native, "sessionHandle", sessionHandle);
    addFunction(context, native, "sessionId", sessionId);
    addFunction(context, native, "playerName", playerName);

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
