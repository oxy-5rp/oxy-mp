#include "bindings.hpp"

#include "convert.hpp"
#include "resource.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

namespace oxymp::script::js {
namespace {

/// Внутреннее поле, в котором сущность держит свой номер.
constexpr int kIdField = 0;

[[nodiscard]] Resource& resourceOf(v8::Isolate* isolate) {
    Resource* const resource = Resource::of(isolate);

    // Изолят заводим мы сами и сами же кладём в него ресурс. Пустота здесь
    // означала бы, что обработчик позвали из чужого изолята, — а это не ошибка
    // скрипта, а поломка в нас.
    return *resource;
}

void fail(v8::Isolate* isolate, std::string_view reason) {
    isolate->ThrowException(v8::Exception::TypeError(toJs(isolate, reason)));
}

/// Номер сущности из объекта. Пусто — объект не наш.
template<typename Id>
[[nodiscard]] std::optional<Id> idOf(v8::Local<v8::Value> value) {
    if (value.IsEmpty() || !value->IsObject()) {
        return std::nullopt;
    }

    const v8::Local<v8::Object> object = value.As<v8::Object>();
    if (object->InternalFieldCount() <= kIdField) {
        return std::nullopt;
    }

    const v8::Local<v8::Data> field = object->GetInternalField(kIdField);
    if (!field->IsValue()) {
        return std::nullopt;
    }

    const v8::Local<v8::Value> stored = field.As<v8::Value>();
    if (!stored->IsUint32()) {
        return std::nullopt;
    }

    return static_cast<Id>(stored.As<v8::Uint32>()->Value());
}

/// Номер игрока из this. Пусто — вызвали не на игроке.
[[nodiscard]] std::optional<shared::PlayerId> selfPlayer(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
    return idOf<shared::PlayerId>(info.This());
}

[[nodiscard]] std::optional<shared::PlayerId> selfPlayer(
    const v8::PropertyCallbackInfo<v8::Value>& info) {
    return idOf<shared::PlayerId>(info.This());
}

// --- Игрок ------------------------------------------------------------------

/// Свойство игрока, взятое из его снимка.
///
/// Снимок берётся заново на каждое обращение, и это не расточительство, а
/// единственный честный способ: слой не хранит правду о мире, она лежит в
/// реестрах сервера. Кешируй мы снимок в объекте — и здоровье, показанное
/// скрипту, разошлось бы с настоящим в первую же секунду боя.
template<auto Field>
void playerField(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);
    if (!player) {
        // Вышедший игрок отвечает пустотой, а не нулём. Ноль здоровья означал бы
        // «мёртв», и скрипт, лечащий мёртвых, лечил бы вышедших.
        info.GetReturnValue().SetUndefined();
        return;
    }

    if constexpr (std::is_same_v<std::decay_t<decltype((*player).*Field)>, std::string>) {
        info.GetReturnValue().Set(toJs(isolate, (*player).*Field));
    } else if constexpr (std::is_same_v<std::decay_t<decltype((*player).*Field)>, bool>) {
        info.GetReturnValue().Set((*player).*Field);
    } else if constexpr (std::is_same_v<std::decay_t<decltype((*player).*Field)>, shared::Vec3>) {
        info.GetReturnValue().Set(toJs(isolate->GetCurrentContext(), (*player).*Field));
    } else {
        info.GetReturnValue().Set(static_cast<double>((*player).*Field));
    }
}

/// Есть ли ещё такой игрок.
void playerValid(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    const std::optional<shared::PlayerId> id = selfPlayer(info);

    info.GetReturnValue().Set(id.has_value() &&
                              resourceOf(info.GetIsolate()).core().player(*id).has_value());
}

/// Здоровье и броня ставятся вместе: ядро меняет их одним распоряжением.
///
/// Причина не в удобстве ядра, а в сети: и то и другое уходит игроку одним
/// сообщением, и разделять их значило бы слать два там, где хватает одного.
/// Отсюда и здесь: сеттер одного читает второе из снимка.
void setPlayerHealth(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                     const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = idOf<shared::PlayerId>(info.This());
    if (!id) {
        return;
    }

    Core& core = resourceOf(isolate).core();

    const std::optional<PlayerInfo> player = core.player(*id);
    const std::optional<std::int64_t> health = intFromJs(isolate->GetCurrentContext(), value);

    if (!player || !health) {
        return;
    }

    (void)core.setHealth(*id, static_cast<std::uint16_t>(std::clamp<std::int64_t>(*health, 0, 200)),
                         player->armour);
}

void setPlayerArmour(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                     const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = idOf<shared::PlayerId>(info.This());
    if (!id) {
        return;
    }

    Core& core = resourceOf(isolate).core();

    const std::optional<PlayerInfo> player = core.player(*id);
    const std::optional<std::int64_t> armour = intFromJs(isolate->GetCurrentContext(), value);

    if (!player || !armour) {
        return;
    }

    (void)core.setHealth(*id, player->health,
                         static_cast<std::uint16_t>(std::clamp<std::int64_t>(*armour, 0, 100)));
}

/// Машина, в которой едет игрок. Пусто — идёт пешком.
void playerVehicle(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    Resource& resource = resourceOf(isolate);

    const std::optional<PlayerInfo> player = resource.core().player(*id);
    if (!player || player->vehicle == shared::kInvalidVehicleId) {
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(
        wrapVehicle(resource, isolate->GetCurrentContext(), player->vehicle));
}

void playerTeleport(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<shared::Vec3> where =
        info.Length() >= 1 ? vec3FromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!where) {
        fail(isolate, "teleport ждёт точку вида { x, y, z }");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().teleport(*id, *where));
}

void playerGiveWeapon(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> weapon =
        info.Length() >= 1 ? intFromJs(context, info[0]) : std::nullopt;

    if (!weapon) {
        fail(isolate, "giveWeapon ждёт числовой хеш оружия");
        return;
    }

    // Боезапас необязателен: выдать оружие без патронов — осмысленное действие.
    const std::optional<std::int64_t> ammo =
        info.Length() >= 2 ? intFromJs(context, info[1]) : std::nullopt;

    info.GetReturnValue().Set(resourceOf(isolate).core().giveWeapon(
        *id, static_cast<std::uint32_t>(*weapon),
        static_cast<std::uint16_t>(std::clamp<std::int64_t>(ammo.value_or(0), 0, 0xFFFF))));
}

void playerClearWeapons(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    info.GetReturnValue().Set(resourceOf(info.GetIsolate()).core().clearWeapons(*id));
}

/// Именованное событие этому игроку — то есть его клиентской части ресурса.
void playerEmit(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    if (info.Length() < 1) {
        fail(isolate, "emit ждёт имя события");
        return;
    }

    const std::string name = fromJs(isolate, info[0]);
    const std::string payload = info.Length() >= 2 ? fromJs(isolate, info[1]) : std::string{};

    info.GetReturnValue().Set(resourceOf(isolate).core().emit(*id, name, payload));
}

/// Строка в чат одному игроку.
void playerTell(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().tell(*id, info.Length() >= 1 ? fromJs(isolate, info[0]) : ""));
}

// --- Машина -----------------------------------------------------------------

template<auto Field>
void vehicleField(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<VehicleInfo> vehicle = resourceOf(isolate).core().vehicle(*id);
    if (!vehicle) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    if constexpr (std::is_same_v<std::decay_t<decltype((*vehicle).*Field)>, shared::Vec3>) {
        info.GetReturnValue().Set(toJs(isolate->GetCurrentContext(), (*vehicle).*Field));
    } else {
        info.GetReturnValue().Set(static_cast<double>((*vehicle).*Field));
    }
}

void vehicleValid(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());

    info.GetReturnValue().Set(id.has_value() &&
                              resourceOf(info.GetIsolate()).core().vehicle(*id).has_value());
}

/// Кто её ведёт. Пусто — никто, машина стоит.
void vehicleOwner(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        return;
    }

    Resource& resource = resourceOf(isolate);

    const std::optional<VehicleInfo> vehicle = resource.core().vehicle(*id);
    if (!vehicle || vehicle->owner == shared::kInvalidPlayerId) {
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(wrapPlayer(resource, isolate->GetCurrentContext(), vehicle->owner));
}

void vehicleDestroy(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        return;
    }

    info.GetReturnValue().Set(resourceOf(info.GetIsolate()).core().removeVehicle(*id));
}

// --- Объект oxymp -----------------------------------------------------------

void onEvent(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    if (info.Length() < 2 || !info[1]->IsFunction()) {
        fail(isolate, "on ждёт имя события и обработчик");
        return;
    }

    resourceOf(isolate).subscribe(fromJs(isolate, info[0]), info[1].As<v8::Function>());
}

/// Событие от клиента. Отдельным именем, а не общим `on`, и это не сахар.
///
/// Событие от клиента ничем не подтверждено: пакет собрать может кто угодно, и
/// прислать его — тоже. Отдельное имя каждый раз напоминает об этом тому, кто
/// пишет обработчик: сюда приходит чужой ввод, а не своё событие сессии.
void onClientEvent(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    if (info.Length() < 2 || !info[1]->IsFunction()) {
        fail(isolate, "onClient ждёт имя события и обработчик");
        return;
    }

    resourceOf(isolate).subscribe("client:" + fromJs(isolate, info[0]),
                                  info[1].As<v8::Function>());
}

void emitClient(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = idOf<shared::PlayerId>(info.Length() >= 1
                                                                          ? info[0]
                                                                          : v8::Local<v8::Value>{});
    if (!id) {
        fail(isolate, "emitClient ждёт игрока первым доводом");
        return;
    }

    if (info.Length() < 2) {
        fail(isolate, "emitClient ждёт имя события");
        return;
    }

    const std::string name = fromJs(isolate, info[1]);
    const std::string payload = info.Length() >= 3 ? fromJs(isolate, info[2]) : std::string{};

    info.GetReturnValue().Set(resourceOf(isolate).core().emit(*id, name, payload));
}

void broadcast(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    resourceOf(isolate).core().broadcast(info.Length() >= 1 ? fromJs(isolate, info[0]) : "");
}

/// Строка в журнал сервера, с именем ресурса.
///
/// Имя обязательно: на сервере с десятком ресурсов строка без него бесполезна —
/// непонятно, кого спрашивать. Так же поступает и alt:V.
void log(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    std::string line;

    for (int i = 0; i < info.Length(); ++i) {
        if (i != 0) {
            line += ' ';
        }
        line += fromJs(isolate, info[i]);
    }

    spdlog::info("[{}] {}", resourceOf(isolate).name(), line);
}

void players(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    Resource& resource = resourceOf(isolate);

    const std::vector<PlayerInfo> all = resource.core().players();
    const v8::Local<v8::Array> array = v8::Array::New(isolate, static_cast<int>(all.size()));

    for (std::size_t i = 0; i < all.size(); ++i) {
        (void)array->Set(context, static_cast<std::uint32_t>(i),
                         wrapPlayer(resource, context, all[i].id));
    }

    info.GetReturnValue().Set(array);
}

void vehicles(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    Resource& resource = resourceOf(isolate);

    const std::vector<VehicleInfo> all = resource.core().vehicles();
    const v8::Local<v8::Array> array = v8::Array::New(isolate, static_cast<int>(all.size()));

    for (std::size_t i = 0; i < all.size(); ++i) {
        (void)array->Set(context, static_cast<std::uint32_t>(i),
                         wrapVehicle(resource, context, all[i].id));
    }

    info.GetReturnValue().Set(array);
}

void createVehicle(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<std::int64_t> model =
        info.Length() >= 1 ? intFromJs(context, info[0]) : std::nullopt;
    const std::optional<shared::Vec3> where =
        info.Length() >= 2 ? vec3FromJs(context, info[1]) : std::nullopt;

    if (!model || !where) {
        fail(isolate, "createVehicle ждёт хеш модели и точку вида { x, y, z }");
        return;
    }

    double heading = 0.0;
    if (info.Length() >= 3) {
        (void)info[2]->NumberValue(context).To(&heading);
    }

    Resource& resource = resourceOf(isolate);

    const shared::VehicleId id = resource.core().createVehicle(
        static_cast<std::uint32_t>(*model), *where, static_cast<float>(heading));

    if (id == shared::kInvalidVehicleId) {
        // Пустота, а не исключение: отказ здесь — обычный ход дела (исчерпан
        // предел машин в сессии), и обрывать им скрипт незачем.
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(wrapVehicle(resource, context, id));
}

void setWeather(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    info.GetReturnValue().Set(resourceOf(isolate).core().setWeather(
        info.Length() >= 1 ? fromJs(isolate, info[0]) : ""));
}

void setTime(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<std::int64_t> hour =
        info.Length() >= 1 ? intFromJs(context, info[0]) : std::nullopt;
    const std::optional<std::int64_t> minute =
        info.Length() >= 2 ? intFromJs(context, info[1]) : std::nullopt;

    if (!hour || !minute) {
        fail(isolate, "setTime ждёт час и минуту числами");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().setTime(
        static_cast<std::uint8_t>(*hour), static_cast<std::uint8_t>(*minute)));
}

// --- Сборка классов ---------------------------------------------------------

/// Заготовка класса сущности.
///
/// Конструктор отдан такой, что бросает: заводить игрока или машину из скрипта
/// нельзя — игроков заводит соединение, машины заводит ядро. Без этого запрета
/// `new oxymp.Player()` дал бы объект с пустым внутренним полем, и всякое
/// обращение к нему выглядело бы как поломка привязок.
[[nodiscard]] v8::Local<v8::FunctionTemplate> entityTemplate(v8::Isolate* isolate,
                                                              std::string_view name) {
    const v8::Local<v8::FunctionTemplate> shape = v8::FunctionTemplate::New(
        isolate, [](const v8::FunctionCallbackInfo<v8::Value>& info) {
            fail(info.GetIsolate(), "сущности сессии не заводятся из скрипта");
        });

    shape->SetClassName(toJs(isolate, name));
    shape->InstanceTemplate()->SetInternalFieldCount(kIdField + 1);

    return shape;
}

void addMethod(v8::Isolate* isolate, const v8::Local<v8::FunctionTemplate>& shape,
               std::string_view name, v8::FunctionCallback callback) {
    shape->PrototypeTemplate()->Set(toJs(isolate, name),
                                    v8::FunctionTemplate::New(isolate, callback));
}

void addGetter(v8::Isolate* isolate, const v8::Local<v8::FunctionTemplate>& shape,
               std::string_view name, v8::AccessorNameGetterCallback getter,
               v8::AccessorNameSetterCallback setter = nullptr) {
    shape->InstanceTemplate()->SetNativeDataProperty(toJs(isolate, name), getter, setter);
}

[[nodiscard]] v8::Local<v8::FunctionTemplate> buildPlayerShape(v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::FunctionTemplate> shape = entityTemplate(isolate, "Player");

    addGetter(isolate, shape, "id", playerField<&PlayerInfo::id>);
    addGetter(isolate, shape, "name", playerField<&PlayerInfo::nickname>);
    addGetter(isolate, shape, "position", playerField<&PlayerInfo::position>);
    addGetter(isolate, shape, "heading", playerField<&PlayerInfo::heading>);
    addGetter(isolate, shape, "health", playerField<&PlayerInfo::health>, setPlayerHealth);
    addGetter(isolate, shape, "armour", playerField<&PlayerInfo::armour>, setPlayerArmour);
    addGetter(isolate, shape, "seat", playerField<&PlayerInfo::seat>);
    addGetter(isolate, shape, "admin", playerField<&PlayerInfo::admin>);
    addGetter(isolate, shape, "vehicle", playerVehicle);
    addGetter(isolate, shape, "valid", playerValid);

    addMethod(isolate, shape, "teleport", playerTeleport);
    addMethod(isolate, shape, "giveWeapon", playerGiveWeapon);
    addMethod(isolate, shape, "clearWeapons", playerClearWeapons);
    addMethod(isolate, shape, "emit", playerEmit);
    addMethod(isolate, shape, "tell", playerTell);

    return shape;
}

[[nodiscard]] v8::Local<v8::FunctionTemplate> buildVehicleShape(v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::FunctionTemplate> shape = entityTemplate(isolate, "Vehicle");

    addGetter(isolate, shape, "id", vehicleField<&VehicleInfo::id>);
    addGetter(isolate, shape, "model", vehicleField<&VehicleInfo::model>);
    addGetter(isolate, shape, "position", vehicleField<&VehicleInfo::position>);
    addGetter(isolate, shape, "rotation", vehicleField<&VehicleInfo::rotation>);
    addGetter(isolate, shape, "owner", vehicleOwner);
    addGetter(isolate, shape, "valid", vehicleValid);

    addMethod(isolate, shape, "destroy", vehicleDestroy);

    return shape;
}

void addFunction(v8::Local<v8::Context> context, const v8::Local<v8::Object>& object,
                 std::string_view name, v8::FunctionCallback callback) {
    v8::Isolate* const isolate = context->GetIsolate();

    (void)object->Set(context, toJs(isolate, name),
                      v8::Function::New(context, callback).ToLocalChecked());
}

/// Заводит объект сущности с проставленным номером.
[[nodiscard]] v8::Local<v8::Value> instantiate(v8::Local<v8::Context> context,
                                                v8::Local<v8::FunctionTemplate> shape,
                                                std::uint32_t id) {
    if (shape.IsEmpty()) {
        return v8::Null(context->GetIsolate());
    }

    // Объект заводится от заготовки, а не вызовом функции-конструктора, и это
    // не придирка к способу. Function::NewInstance добросовестно вызывает
    // конструктор, а конструктор у сущностей нарочно бросает: заводить игроков и
    // машины из скрипта нельзя. Заготовка же отдаёт объект с той же цепочкой
    // прототипов, никого не спрашивая, — то есть ровно то, что нужно.
    v8::Local<v8::Object> object;
    if (!shape->InstanceTemplate()->NewInstance(context).ToLocal(&object)) {
        return v8::Null(context->GetIsolate());
    }

    object->SetInternalField(kIdField, v8::Integer::NewFromUnsigned(context->GetIsolate(), id));
    return object;
}

} // namespace

v8::Local<v8::Value> wrapPlayer(Resource& resource, v8::Local<v8::Context> context,
                                shared::PlayerId id) {
    return instantiate(context, resource.playerShape(), static_cast<std::uint32_t>(id));
}

v8::Local<v8::Value> wrapVehicle(Resource& resource, v8::Local<v8::Context> context,
                                 shared::VehicleId id) {
    return instantiate(context, resource.vehicleShape(), static_cast<std::uint32_t>(id));
}

void installBindings(Resource& resource, v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();

    const v8::Local<v8::FunctionTemplate> playerShape = buildPlayerShape(context);
    const v8::Local<v8::FunctionTemplate> vehicleShape = buildVehicleShape(context);

    resource.setPlayerShape(playerShape);
    resource.setVehicleShape(vehicleShape);

    const v8::Local<v8::Object> oxymp = v8::Object::New(isolate);

    addFunction(context, oxymp, "on", onEvent);
    addFunction(context, oxymp, "onClient", onClientEvent);
    addFunction(context, oxymp, "emitClient", emitClient);
    addFunction(context, oxymp, "broadcast", broadcast);
    addFunction(context, oxymp, "log", log);
    addFunction(context, oxymp, "players", players);
    addFunction(context, oxymp, "vehicles", vehicles);
    addFunction(context, oxymp, "createVehicle", createVehicle);
    addFunction(context, oxymp, "setWeather", setWeather);
    addFunction(context, oxymp, "setTime", setTime);

    // Классы кладутся туда же: скрипту они нужны не для того, чтобы заводить
    // сущности, а для проверок вида `x instanceof oxymp.Player`.
    (void)oxymp->Set(context, toJs(isolate, "Player"),
                     playerShape->GetFunction(context).ToLocalChecked());
    (void)oxymp->Set(context, toJs(isolate, "Vehicle"),
                     vehicleShape->GetFunction(context).ToLocalChecked());

    // Имя ресурса — чтобы он мог собрать путь к своим файлам и назвать себя в
    // событии клиенту.
    (void)oxymp->Set(context, toJs(isolate, "resourceName"), toJs(isolate, resource.name()));

    (void)context->Global()->Set(context, toJs(isolate, "oxymp"), oxymp);
}

} // namespace oxymp::script::js
