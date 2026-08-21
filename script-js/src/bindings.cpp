#include "bindings.hpp"

#include "convert.hpp"
#include "resource.hpp"

#include <oxymp/shared/math/joaat.hpp>
#include <oxymp/shared/protocol/protocol_version.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
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

/// Модель персонажа, назначенная сервером.
///
/// Принимает и хеш числом, и имя строкой — так же, как alt:V: режимы пишут
/// `player.model = 'mp_m_freemode_01'` куда чаще, чем считают хеш руками.
void setPlayerModel(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                    const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = idOf<shared::PlayerId>(info.This());
    if (!id) {
        return;
    }

    // Числом или строкой — решается по числу, а не по строке, и это не
    // вкусовщина: `v8::Value::IsString` объявлен экспортируемым из библиотеки,
    // но определён прямо в заголовке, и линковщик отказывается выбирать между
    // двумя его телами (LNK2005). `IsNumber` таким не страдает — через него и
    // спрашиваем.
    const std::optional<std::int64_t> number = intFromJs(isolate->GetCurrentContext(), value);

    const std::uint32_t model = number ? static_cast<std::uint32_t>(*number)
                                       : shared::joaat(fromJs(isolate, value));

    if (model == 0) {
        return;
    }

    (void)resourceOf(isolate).core().setModel(*id, model);
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

/// Число из довода по месту. Пусто — довода нет или он не число.
[[nodiscard]] std::optional<std::int64_t> argAt(const v8::FunctionCallbackInfo<v8::Value>& info,
                                                int index) {
    if (info.Length() <= index) {
        return std::nullopt;
    }

    return intFromJs(info.GetIsolate()->GetCurrentContext(), info[index]);
}

/// Одевает игрока: слот, вещь, расцветка, набор цветов.
void playerSetClothes(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> component = argAt(info, 0);
    const std::optional<std::int64_t> drawable = argAt(info, 1);

    if (!component || !drawable) {
        fail(isolate, "setClothes ждёт слот и вещь числами");
        return;
    }

    // Расцветка и набор цветов необязательны: у alt:V они с умолчаниями, и
    // ресурсы почти всегда зовут это двумя доводами.
    const std::int64_t texture = argAt(info, 2).value_or(0);
    const std::int64_t palette = argAt(info, 3).value_or(0);

    const bool done = resourceOf(isolate).core().setClothes(
        *id, static_cast<std::uint8_t>(*component), static_cast<std::uint8_t>(*drawable),
        static_cast<std::uint8_t>(texture), static_cast<std::uint8_t>(palette));

    info.GetReturnValue().Set(done);
}

/// Надевает аксессуар: шляпу, очки, серьги, часы, браслет.
void playerSetProp(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> index = argAt(info, 0);
    const std::optional<std::int64_t> drawable = argAt(info, 1);

    if (!index || !drawable) {
        fail(isolate, "setProp ждёт место и вещь числами");
        return;
    }

    const std::int64_t texture = argAt(info, 2).value_or(0);

    const bool done = resourceOf(isolate).core().setProp(
        *id, static_cast<std::uint8_t>(*index), static_cast<std::int8_t>(*drawable),
        static_cast<std::int8_t>(texture));

    info.GetReturnValue().Set(done);
}

/// Снимает аксессуар. Минус единица — то, чем «ничего не надето» зовётся у игры.
void playerClearProp(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> index = argAt(info, 0);
    if (!index) {
        fail(isolate, "clearProp ждёт место числом");
        return;
    }

    const bool done =
        resourceOf(isolate).core().setProp(*id, static_cast<std::uint8_t>(*index), -1, 0);

    info.GetReturnValue().Set(done);
}

/// Переставляет машину: точка и, необязательно, поворот.
void vehicleTeleport(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        fail(isolate, "teleport зовётся у машины");
        return;
    }

    const std::optional<shared::Vec3> position =
        info.Length() >= 1 ? vec3FromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!position) {
        fail(isolate, "teleport ждёт точку первым доводом");
        return;
    }

    Core& core = resourceOf(isolate).core();

    // Поворот необязателен: чаще машину просто переставляют. Не названный, он
    // берётся у неё же — иначе всякая перестановка разворачивала бы машину на
    // север, о чём просивший не подозревал бы.
    const std::optional<VehicleInfo> vehicle = core.vehicle(*id);
    double heading = vehicle ? static_cast<double>(vehicle->rotation.z) : 0.0;

    if (info.Length() >= 2) {
        (void)info[1]->NumberValue(isolate->GetCurrentContext()).To(&heading);
    }

    info.GetReturnValue().Set(
        core.teleportVehicle(*id, *position, static_cast<float>(heading)));
}

/// Чинит машину: кузов, двигатель, стёкла, двери, колёса и вмятины.
void vehicleRepair(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        fail(isolate, "repair зовётся у машины");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().repairVehicle(*id));
}

/// Сажает игрока в машину: сама машина и место в ней.
void playerSetIntoVehicle(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    // Машина приходит сущностью, как её и передают у alt:V. Числом — тоже
    // принимаем: номер машины у нас и есть её имя, и ресурс вправе держать его
    // у себя.
    std::optional<shared::VehicleId> vehicle;

    if (info.Length() >= 1) {
        vehicle = idOf<shared::VehicleId>(info[0]);

        if (!vehicle) {
            if (const std::optional<std::int64_t> number =
                    intFromJs(isolate->GetCurrentContext(), info[0])) {
                vehicle = static_cast<shared::VehicleId>(*number);
            }
        }
    }

    if (!vehicle) {
        fail(isolate, "setIntoVehicle ждёт машину первым доводом");
        return;
    }

    // Место необязательно: не названное, оно означает «за руль» — так же
    // толкует его и alt:V.
    const std::int64_t seat = argAt(info, 1).value_or(shared::kNoSeat);

    info.GetReturnValue().Set(resourceOf(isolate).core().setIntoVehicle(
        *id, *vehicle, static_cast<std::int8_t>(seat)));
}

/// В каком слое мира игрок. Ставится числом; всё, что не число, пропускается.
void setPlayerDimension(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                        const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = idOf<shared::PlayerId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> dimension = intFromJs(isolate->GetCurrentContext(), value);
    if (!dimension) {
        return;
    }

    (void)resourceOf(isolate).core().setDimension(*id, static_cast<std::int32_t>(*dimension));
}

/// То же для машины.
void setVehicleDimension(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                         const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> dimension = intFromJs(isolate->GetCurrentContext(), value);
    if (!dimension) {
        return;
    }

    (void)resourceOf(isolate).core().setVehicleDimension(*id,
                                                         static_cast<std::int32_t>(*dimension));
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

/// Выгоняет игрока из сессии.
void playerKick(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    // Причина необязательна: не всякий отказ нужно объяснять.
    info.GetReturnValue().Set(
        resourceOf(isolate).core().kick(*id, info.Length() >= 1 ? fromJs(isolate, info[0]) : ""));
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

// --- Предмет ----------------------------------------------------------------
//
// Устроен проще машины, и это не упрощение ради экономии, а следствие того, чем
// он является. У машины есть ведущий — клиент, считающий её физику, — потому что
// машина едет, мнётся и переворачивается. Предмет стоит.

template<auto Field>
void objectField(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::ObjectId> id = idOf<shared::ObjectId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<ObjectInfo> object = resourceOf(isolate).core().object(*id);
    if (!object) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    if constexpr (std::is_same_v<std::decay_t<decltype((*object).*Field)>, shared::Vec3>) {
        info.GetReturnValue().Set(toJs(isolate->GetCurrentContext(), (*object).*Field));
    } else {
        info.GetReturnValue().Set(static_cast<double>((*object).*Field));
    }
}

void objectValid(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    const std::optional<shared::ObjectId> id = idOf<shared::ObjectId>(info.This());

    info.GetReturnValue().Set(id.has_value() &&
                              resourceOf(info.GetIsolate()).core().object(*id).has_value());
}

void setObjectDimensionValue(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                             const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::ObjectId> id = idOf<shared::ObjectId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> dimension = intFromJs(isolate->GetCurrentContext(), value);
    if (!dimension) {
        return;
    }

    (void)resourceOf(isolate).core().setObjectDimension(*id,
                                                        static_cast<std::int32_t>(*dimension));
}

void objectDestroy(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const std::optional<shared::ObjectId> id = idOf<shared::ObjectId>(info.This());
    if (!id) {
        return;
    }

    info.GetReturnValue().Set(resourceOf(info.GetIsolate()).core().removeObject(*id));
}

// --- Описания объектом ------------------------------------------------------
//
// Всё, у чего доводов больше пяти, скрипт задаёт объектом, а не россыпью:
// движение, метка, маркер, контрольная точка. Читаются они одинаково, и читает
// их одно и то же.

/// Чтение полей описания, присланного скриптом.
///
/// Заведено на всех сразу, и это не обобщение ради обобщения: отсутствующее
/// поле у любого из них означает «как принято у alt:V», а не ошибку. Ресурс
/// волен задать три поля из пятнадцати, и остальные обязаны встать сами.
class Fields {
public:
    Fields(v8::Local<v8::Context> context, v8::Local<v8::Object> object) noexcept
        : context_(context), object_(object) {}

    /// Число. Нечисло и бесконечность считаются отсутствием: пришедший оттуда
    /// NaN разошёлся бы по протоколу и всплыл бы у рисующего.
    [[nodiscard]] double number(const char* name, double fallback) const {
        v8::Local<v8::Value> field;

        if (!object_->Get(context_, toJs(context_->GetIsolate(), name)).ToLocal(&field)) {
            return fallback;
        }

        double got = fallback;
        (void)field->NumberValue(context_).To(&got);

        return std::isfinite(got) ? got : fallback;
    }

    [[nodiscard]] std::uint8_t byte(const char* name, double fallback) const {
        return static_cast<std::uint8_t>(std::clamp(number(name, fallback), 0.0, 255.0));
    }

    /// Целое со знаком в один байт: место тюнинга, раскраска, тонировка.
    ///
    /// Отдельно от byte, а не заодно с ним: минус единица означает у них
    /// «заводское», и обрезав её до нуля, мы поставили бы машине первую попавшуюся
    /// деталь вместо того, чтобы оставить её как есть.
    [[nodiscard]] std::int8_t signedByte(const char* name, double fallback) const {
        return static_cast<std::int8_t>(std::clamp(number(name, fallback), -128.0, 127.0));
    }

    /// Набор чисел. Отсутствующее поле и всё, что набором не является, — пустой.
    [[nodiscard]] std::vector<double> numbers(const char* name) const {
        std::vector<double> got;

        v8::Local<v8::Value> field;
        if (!object_->Get(context_, toJs(context_->GetIsolate(), name)).ToLocal(&field) ||
            !field->IsArray()) {
            return got;
        }

        const v8::Local<v8::Array> array = field.As<v8::Array>();
        got.reserve(array->Length());

        for (std::uint32_t i = 0; i < array->Length(); ++i) {
            v8::Local<v8::Value> item;
            double value = 0.0;

            if (array->Get(context_, i).ToLocal(&item) && item->NumberValue(context_).To(&value) &&
                std::isfinite(value)) {
                got.push_back(value);
                continue;
            }

            // Дырка в наборе — не повод бросить остальное: место, которого скрипт
            // не назвал, остаётся заводским, и ноль здесь означал бы первую
            // попавшуюся деталь.
            got.push_back(shared::kStockMod);
        }

        return got;
    }

    /// Признак. Отсутствующий — ложь, как и у alt:V.
    [[nodiscard]] bool flag(const char* name) const {
        v8::Local<v8::Value> field;

        if (!object_->Get(context_, toJs(context_->GetIsolate(), name)).ToLocal(&field)) {
            return false;
        }

        return field->BooleanValue(context_->GetIsolate());
    }

    /// Признак, которого у alt:V по умолчанию нет: `visible` там истина.
    [[nodiscard]] bool flagOr(const char* name, bool fallback) const {
        v8::Local<v8::Value> field;

        if (!object_->Get(context_, toJs(context_->GetIsolate(), name)).ToLocal(&field) ||
            field->IsUndefined()) {
            return fallback;
        }

        return field->BooleanValue(context_->GetIsolate());
    }

    [[nodiscard]] shared::Vec3 point(const char* name, const shared::Vec3& fallback) const {
        v8::Local<v8::Value> field;

        if (!object_->Get(context_, toJs(context_->GetIsolate(), name)).ToLocal(&field)) {
            return fallback;
        }

        return vec3FromJs(context_, field).value_or(fallback);
    }

    [[nodiscard]] std::string text(const char* name) const {
        v8::Local<v8::Value> field;

        if (!object_->Get(context_, toJs(context_->GetIsolate(), name)).ToLocal(&field) ||
            field->IsUndefined()) {
            return {};
        }

        return fromJs(context_->GetIsolate(), field);
    }

private:
    v8::Local<v8::Context> context_;
    v8::Local<v8::Object> object_;
};

/// Описание картинки объектом, каким его прислал скрипт. Пусто — не объект.
[[nodiscard]] std::optional<Fields> fieldsOf(v8::Local<v8::Context> context,
                                             v8::Local<v8::Value> value) {
    if (value.IsEmpty() || !value->IsObject()) {
        return std::nullopt;
    }

    return Fields{context, value.As<v8::Object>()};
}

// --- Движения персонажа -----------------------------------------------------

void playAnimation(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(context, info[0]) : std::nullopt;

    const std::optional<Fields> fields =
        info.Length() >= 2 ? fieldsOf(context, info[1]) : std::nullopt;

    if (!id || !fields) {
        fail(isolate, "playAnimation ждёт номер игрока и описание движения объектом");
        return;
    }

    AnimationInfo animation;
    animation.dictionary = fields->text("dictionary");
    animation.name = fields->text("name");

    // Восьмёрка по умолчанию — не наша выдумка, а значение alt:V: режим,
    // назвавший только набор и движение, ждёт именно такого перехода.
    animation.blendIn = static_cast<float>(fields->number("blendIn", 8.0));
    animation.blendOut = static_cast<float>(fields->number("blendOut", 8.0));
    animation.duration = static_cast<std::int32_t>(fields->number("duration", -1.0));
    animation.flags = static_cast<std::int32_t>(fields->number("flags", 0.0));
    animation.playbackRate = static_cast<float>(fields->number("playbackRate", 1.0));
    animation.lockX = fields->flag("lockX");
    animation.lockY = fields->flag("lockY");
    animation.lockZ = fields->flag("lockZ");

    if (animation.dictionary.empty() || animation.name.empty()) {
        fail(isolate, "playAnimation ждёт непустые набор движений и имя движения");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().playAnimation(
        static_cast<shared::PlayerId>(*id), animation));
}

void clearTasks(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!id) {
        fail(isolate, "clearTasks ждёт номер игрока");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().clearTasks(static_cast<shared::PlayerId>(*id)));
}

// --- Внешность машины -------------------------------------------------------
//
// Ходит объектом и целиком, а не свойством на поле. Причина та же, что и у
// метки: тюнинг ставят сразу помногу — покрасил, обул, навесил, — и поле за
// полем означало бы по сообщению клиенту на каждое.
//
// Имена полей здесь наши, а не альтивишные: `alt-server` переводит их у себя.
// Так и должно быть — разница между `colour` и `color` и между `tyre` и `tire`
// принадлежит слою совместимости, а не мосту.

[[nodiscard]] v8::Local<v8::Object> appearanceToJs(v8::Local<v8::Context> context,
                                                   const VehicleAppearanceInfo& look) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::Object> object = v8::Object::New(isolate);

    const auto put = [&](const char* name, double value) {
        (void)object->Set(context, toJs(isolate, name), v8::Number::New(isolate, value));
    };

    put("primaryColour", look.primaryColour);
    put("secondaryColour", look.secondaryColour);
    put("pearlescentColour", look.pearlescentColour);
    put("wheelColour", look.wheelColour);
    put("plateStyle", look.plateStyle);
    put("livery", look.livery);
    put("wheelType", look.wheelType);
    put("windowTint", look.windowTint);
    put("dirtLevel", look.dirtLevel);
    put("toggleMods", look.toggleMods);
    put("tyreSmokeRed", look.tyreSmokeRed);
    put("tyreSmokeGreen", look.tyreSmokeGreen);
    put("tyreSmokeBlue", look.tyreSmokeBlue);
    put("neonSides", look.neonSides);
    put("neonRed", look.neonRed);
    put("neonGreen", look.neonGreen);
    put("neonBlue", look.neonBlue);
    put("extras", look.extras);

    (void)object->Set(context, toJs(isolate, "plate"), toJs(isolate, look.plate));
    (void)object->Set(context, toJs(isolate, "customTyres"),
                      v8::Boolean::New(isolate, look.customTyres));

    const v8::Local<v8::Array> mods =
        v8::Array::New(isolate, static_cast<int>(look.mods.size()));

    for (std::size_t slot = 0; slot < look.mods.size(); ++slot) {
        (void)mods->Set(context, static_cast<std::uint32_t>(slot),
                        v8::Integer::New(isolate, look.mods[slot]));
    }

    (void)object->Set(context, toJs(isolate, "mods"), mods);

    return object;
}

[[nodiscard]] std::optional<VehicleAppearanceInfo> appearanceFromJs(
    v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
    const std::optional<Fields> fields = fieldsOf(context, value);
    if (!fields) {
        return std::nullopt;
    }

    VehicleAppearanceInfo look;
    look.primaryColour = fields->byte("primaryColour", 0);
    look.secondaryColour = fields->byte("secondaryColour", 0);
    look.pearlescentColour = fields->byte("pearlescentColour", 0);
    look.wheelColour = fields->byte("wheelColour", 0);
    look.plate = fields->text("plate");
    look.plateStyle = fields->byte("plateStyle", 0);
    look.livery = fields->signedByte("livery", shared::kStockMod);
    look.wheelType = fields->signedByte("wheelType", shared::kStockMod);
    look.windowTint = fields->signedByte("windowTint", shared::kStockMod);
    look.dirtLevel = static_cast<float>(fields->number("dirtLevel", 0.0));
    look.toggleMods = static_cast<std::uint32_t>(fields->number("toggleMods", 0.0));
    look.customTyres = fields->flag("customTyres");
    look.tyreSmokeRed = fields->byte("tyreSmokeRed", 255);
    look.tyreSmokeGreen = fields->byte("tyreSmokeGreen", 255);
    look.tyreSmokeBlue = fields->byte("tyreSmokeBlue", 255);
    look.neonSides = fields->byte("neonSides", 0);
    look.neonRed = fields->byte("neonRed", 255);
    look.neonGreen = fields->byte("neonGreen", 255);
    look.neonBlue = fields->byte("neonBlue", 255);
    look.extras = static_cast<std::uint16_t>(fields->number("extras", 0.0));

    // Набор мест тюнинга короче нашего — остальные остаются заводскими; длиннее —
    // лишнее отбрасывается. Отказать было бы неверно: длина набора задана
    // протоколом, скрипт о ней не знает и знать не должен.
    const std::vector<double> mods = fields->numbers("mods");

    for (std::size_t slot = 0; slot < look.mods.size() && slot < mods.size(); ++slot) {
        look.mods[slot] = static_cast<std::int8_t>(std::clamp(mods[slot], -128.0, 127.0));
    }

    return look;
}

/// Как машина выглядит. Пусто — машины уже нет.
void vehicleAppearance(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<VehicleAppearanceInfo> look =
        resourceOf(isolate).core().vehicleAppearance(*id);

    if (!look) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(appearanceToJs(isolate->GetCurrentContext(), *look));
}

void vehicleSetAppearance(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        fail(isolate, "setAppearance зовётся у машины");
        return;
    }

    const std::optional<VehicleAppearanceInfo> look =
        info.Length() >= 1 ? appearanceFromJs(isolate->GetCurrentContext(), info[0])
                           : std::nullopt;

    if (!look) {
        fail(isolate, "setAppearance ждёт описание внешности объектом");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().setVehicleAppearance(*id, *look));
}

// --- Метка на карте ---------------------------------------------------------
//
// Мост нарочно плоский: метка приходит и уходит объектом с полями, а не
// сущностью с методами. Причина в том, что метку правят целиком и редко —
// покрасил, переименовал, подвинул, — а поле за полем означало бы по сообщению
// клиенту на каждое, и он увидел бы метку поправленной наполовину.
//
// Сущностью её делает слой alt:V, поверх этого.

[[nodiscard]] std::optional<BlipInfo> blipFromJs(v8::Local<v8::Context> context,
                                                 v8::Local<v8::Value> value) {
    const std::optional<Fields> fields = fieldsOf(context, value);
    if (!fields) {
        return std::nullopt;
    }

    BlipInfo blip;
    blip.position = fields->point("position", {});
    blip.sprite = static_cast<std::uint16_t>(fields->number("sprite", 1));
    blip.colour = fields->byte("color", 0);
    blip.alpha = fields->byte("alpha", 255);
    blip.display = fields->byte("display", 2);
    blip.scale = static_cast<float>(fields->number("scale", 1.0));
    blip.dimension = static_cast<std::int32_t>(fields->number("dimension", 0));
    blip.shortRange = fields->flag("shortRange");
    blip.name = fields->text("name");

    return blip;
}

[[nodiscard]] std::optional<MarkerInfo> markerFromJs(v8::Local<v8::Context> context,
                                                     v8::Local<v8::Value> value) {
    const std::optional<Fields> fields = fieldsOf(context, value);
    if (!fields) {
        return std::nullopt;
    }

    MarkerInfo marker;
    marker.type = fields->byte("markerType", 0);
    marker.position = fields->point("position", {});
    marker.rotation = fields->point("rotation", {});
    marker.direction = fields->point("direction", {});

    // Единица по всем осям, а не ноль: маркер размера ноль не рисуется вовсе, и
    // режим, не назвавший размера, увидел бы пустое место.
    marker.scale = fields->point("scale", shared::Vec3{1.0F, 1.0F, 1.0F});

    marker.red = fields->byte("red", 255);
    marker.green = fields->byte("green", 255);
    marker.blue = fields->byte("blue", 255);
    marker.alpha = fields->byte("alpha", 255);

    marker.visible = fields->flagOr("visible", true);
    marker.bobUpAndDown = fields->flag("bobUpAndDown");
    marker.faceCamera = fields->flag("faceCamera");
    marker.rotate = fields->flag("rotate");

    marker.streamingDistance = static_cast<float>(fields->number("streamingDistance", 0.0));
    marker.dimension = static_cast<std::int32_t>(fields->number("dimension", 0));

    return marker;
}

[[nodiscard]] std::optional<CheckpointInfo> checkpointFromJs(v8::Local<v8::Context> context,
                                                             v8::Local<v8::Value> value) {
    const std::optional<Fields> fields = fieldsOf(context, value);
    if (!fields) {
        return std::nullopt;
    }

    CheckpointInfo point;
    point.type = fields->byte("checkpointType", 0);
    point.position = fields->point("position", {});
    point.nextPosition = fields->point("nextPosition", {});
    point.radius = static_cast<float>(fields->number("radius", 1.0));
    point.height = static_cast<float>(fields->number("height", 2.0));

    point.red = fields->byte("red", 255);
    point.green = fields->byte("green", 255);
    point.blue = fields->byte("blue", 255);
    point.alpha = fields->byte("alpha", 255);

    point.iconRed = fields->byte("iconRed", 255);
    point.iconGreen = fields->byte("iconGreen", 255);
    point.iconBlue = fields->byte("iconBlue", 255);
    point.iconAlpha = fields->byte("iconAlpha", 255);

    point.visible = fields->flagOr("visible", true);
    point.streamingDistance = static_cast<float>(fields->number("streamingDistance", 0.0));
    point.dimension = static_cast<std::int32_t>(fields->number("dimension", 0));

    return point;
}

void createBlip(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<BlipInfo> blip =
        info.Length() >= 1 ? blipFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!blip) {
        fail(isolate, "createBlip ждёт описание метки объектом");
        return;
    }

    const shared::BlipId id = resourceOf(isolate).core().createBlip(*blip);

    if (id == shared::kInvalidBlipId) {
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(static_cast<double>(id));
}

void updateBlip(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    const std::optional<BlipInfo> blip =
        info.Length() >= 2 ? blipFromJs(isolate->GetCurrentContext(), info[1]) : std::nullopt;

    if (!id || !blip) {
        fail(isolate, "updateBlip ждёт номер метки и её описание");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().updateBlip(static_cast<shared::BlipId>(*id), *blip));
}

void removeBlip(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!id) {
        fail(isolate, "removeBlip ждёт номер метки");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().removeBlip(static_cast<shared::BlipId>(*id)));
}

// --- Нарисованное в мире ----------------------------------------------------
//
// Маркер и контрольная точка ходят тем же плоским мостом, что и метка, и по той
// же причине: правят их целиком, а не по полю.

void createMarker(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<MarkerInfo> marker =
        info.Length() >= 1 ? markerFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!marker) {
        fail(isolate, "createMarker ждёт описание маркера объектом");
        return;
    }

    const shared::MarkerId id = resourceOf(isolate).core().createMarker(*marker);

    if (id == shared::kInvalidMarkerId) {
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(static_cast<double>(id));
}

void updateMarker(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    const std::optional<MarkerInfo> marker =
        info.Length() >= 2 ? markerFromJs(isolate->GetCurrentContext(), info[1]) : std::nullopt;

    if (!id || !marker) {
        fail(isolate, "updateMarker ждёт номер маркера и его описание");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().updateMarker(static_cast<shared::MarkerId>(*id), *marker));
}

void removeMarker(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!id) {
        fail(isolate, "removeMarker ждёт номер маркера");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().removeMarker(static_cast<shared::MarkerId>(*id)));
}

void createCheckpoint(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<CheckpointInfo> point =
        info.Length() >= 1 ? checkpointFromJs(isolate->GetCurrentContext(), info[0])
                           : std::nullopt;

    if (!point) {
        fail(isolate, "createCheckpoint ждёт описание точки объектом");
        return;
    }

    const shared::CheckpointId id = resourceOf(isolate).core().createCheckpoint(*point);

    if (id == shared::kInvalidCheckpointId) {
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(static_cast<double>(id));
}

void updateCheckpoint(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    const std::optional<CheckpointInfo> point =
        info.Length() >= 2 ? checkpointFromJs(isolate->GetCurrentContext(), info[1])
                           : std::nullopt;

    if (!id || !point) {
        fail(isolate, "updateCheckpoint ждёт номер точки и её описание");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().updateCheckpoint(
        static_cast<shared::CheckpointId>(*id), *point));
}

void removeCheckpoint(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!id) {
        fail(isolate, "removeCheckpoint ждёт номер точки");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().removeCheckpoint(static_cast<shared::CheckpointId>(*id)));
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

/// Объявляет событие всем поднятым ресурсам, включая свой.
///
/// Тем ресурсы и говорят друг с другом. Доводы приходят одной строкой, и иначе
/// быть не может: у каждого ресурса свой изолят, и значение одного в чужом не
/// живёт вовсе. Укладывает и разбирает их слой alt:V — здесь строка проходит
/// насквозь, не толкуясь.
void emitEvent(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    if (info.Length() < 1) {
        fail(isolate, "emit ждёт имя события");
        return;
    }

    const std::string name = fromJs(isolate, info[0]);
    if (name.empty()) {
        fail(isolate, "emit ждёт непустое имя события");
        return;
    }

    const std::string payload =
        info.Length() >= 2 ? fromJs(isolate, info[1]) : std::string{};

    resourceOf(isolate).announce(name, payload);
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

/// Собирает доводы журнала в одну строку через пробел.
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

/// Строка в журнал сервера, с именем ресурса.
///
/// Имя обязательно: на сервере с десятком ресурсов строка без него бесполезна —
/// непонятно, кого спрашивать. Так же поступает и alt:V.
void log(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    spdlog::info("[{}] {}", resourceOf(isolate).name(), joinArguments(info));
}

/// То же уровнем предупреждения.
///
/// Отдельными привязками, а не доводом-уровнем у одной: уровень уходит в spdlog
/// на этапе сборки строки, и передавать его числом из скрипта значило бы завести
/// разбор числа там, где хватает трёх имён.
void logWarning(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    spdlog::warn("[{}] {}", resourceOf(isolate).name(), joinArguments(info));
}

void logError(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    spdlog::error("[{}] {}", resourceOf(isolate).name(), joinArguments(info));
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

void objects(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    Resource& resource = resourceOf(isolate);

    const std::vector<ObjectInfo> all = resource.core().objects();
    const v8::Local<v8::Array> array = v8::Array::New(isolate, static_cast<int>(all.size()));

    for (std::size_t i = 0; i < all.size(); ++i) {
        (void)array->Set(context, static_cast<std::uint32_t>(i),
                         wrapObject(resource, context, all[i].id));
    }

    info.GetReturnValue().Set(array);
}

void createObject(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<std::int64_t> model =
        info.Length() >= 1 ? intFromJs(context, info[0]) : std::nullopt;

    const std::optional<shared::Vec3> where =
        info.Length() >= 2 ? vec3FromJs(context, info[1]) : std::nullopt;

    if (!model || !where) {
        fail(isolate, "createObject ждёт модель и точку");
        return;
    }

    // Поворот необязателен: предмет чаще ставят как есть.
    const shared::Vec3 rotation =
        info.Length() >= 3 ? vec3FromJs(context, info[2]).value_or(shared::Vec3{}) : shared::Vec3{};

    Resource& resource = resourceOf(isolate);

    const shared::ObjectId id = resource.core().createObject(
        static_cast<std::uint32_t>(*model), *where, rotation);

    if (id == shared::kInvalidObjectId) {
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(wrapObject(resource, context, id));
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
    addGetter(isolate, shape, "model", playerField<&PlayerInfo::model>, setPlayerModel);
    addGetter(isolate, shape, "seat", playerField<&PlayerInfo::seat>);
    addGetter(isolate, shape, "admin", playerField<&PlayerInfo::admin>);
    addGetter(isolate, shape, "dimension", playerField<&PlayerInfo::dimension>,
              setPlayerDimension);
    addGetter(isolate, shape, "vehicle", playerVehicle);
    addGetter(isolate, shape, "valid", playerValid);

    addMethod(isolate, shape, "teleport", playerTeleport);
    addMethod(isolate, shape, "giveWeapon", playerGiveWeapon);
    addMethod(isolate, shape, "clearWeapons", playerClearWeapons);
    addMethod(isolate, shape, "setClothes", playerSetClothes);
    addMethod(isolate, shape, "setProp", playerSetProp);
    addMethod(isolate, shape, "clearProp", playerClearProp);
    addMethod(isolate, shape, "setIntoVehicle", playerSetIntoVehicle);
    addMethod(isolate, shape, "emit", playerEmit);
    addMethod(isolate, shape, "tell", playerTell);
    addMethod(isolate, shape, "kick", playerKick);

    return shape;
}

[[nodiscard]] v8::Local<v8::FunctionTemplate> buildObjectShape(v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::FunctionTemplate> shape = entityTemplate(isolate, "Object");

    addGetter(isolate, shape, "id", objectField<&ObjectInfo::id>);
    addGetter(isolate, shape, "model", objectField<&ObjectInfo::model>);
    addGetter(isolate, shape, "position", objectField<&ObjectInfo::position>);
    addGetter(isolate, shape, "rotation", objectField<&ObjectInfo::rotation>);
    addGetter(isolate, shape, "dimension", objectField<&ObjectInfo::dimension>,
              setObjectDimensionValue);
    addGetter(isolate, shape, "valid", objectValid);

    addMethod(isolate, shape, "destroy", objectDestroy);

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
    addGetter(isolate, shape, "dimension", vehicleField<&VehicleInfo::dimension>,
              setVehicleDimension);
    addGetter(isolate, shape, "valid", vehicleValid);

    addGetter(isolate, shape, "appearance", vehicleAppearance);

    addMethod(isolate, shape, "destroy", vehicleDestroy);
    addMethod(isolate, shape, "teleport", vehicleTeleport);
    addMethod(isolate, shape, "repair", vehicleRepair);
    addMethod(isolate, shape, "setAppearance", vehicleSetAppearance);

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

v8::Local<v8::Value> wrapObject(Resource& resource, v8::Local<v8::Context> context,
                                shared::ObjectId id) {
    return instantiate(context, resource.objectShape(), static_cast<std::uint32_t>(id));
}

void installBindings(Resource& resource, v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();

    const v8::Local<v8::FunctionTemplate> playerShape = buildPlayerShape(context);
    const v8::Local<v8::FunctionTemplate> vehicleShape = buildVehicleShape(context);
    const v8::Local<v8::FunctionTemplate> objectShape = buildObjectShape(context);

    resource.setPlayerShape(playerShape);
    resource.setVehicleShape(vehicleShape);
    resource.setObjectShape(objectShape);

    const v8::Local<v8::Object> oxymp = v8::Object::New(isolate);

    addFunction(context, oxymp, "on", onEvent);
    addFunction(context, oxymp, "emit", emitEvent);
    addFunction(context, oxymp, "onClient", onClientEvent);
    addFunction(context, oxymp, "emitClient", emitClient);
    addFunction(context, oxymp, "broadcast", broadcast);
    addFunction(context, oxymp, "log", log);
    addFunction(context, oxymp, "logWarning", logWarning);
    addFunction(context, oxymp, "logError", logError);
    addFunction(context, oxymp, "players", players);
    addFunction(context, oxymp, "vehicles", vehicles);
    addFunction(context, oxymp, "createVehicle", createVehicle);
    addFunction(context, oxymp, "objects", objects);
    addFunction(context, oxymp, "createObject", createObject);
    addFunction(context, oxymp, "playAnimation", playAnimation);
    addFunction(context, oxymp, "clearTasks", clearTasks);
    addFunction(context, oxymp, "createBlip", createBlip);
    addFunction(context, oxymp, "updateBlip", updateBlip);
    addFunction(context, oxymp, "removeBlip", removeBlip);
    addFunction(context, oxymp, "createMarker", createMarker);
    addFunction(context, oxymp, "updateMarker", updateMarker);
    addFunction(context, oxymp, "removeMarker", removeMarker);
    addFunction(context, oxymp, "createCheckpoint", createCheckpoint);
    addFunction(context, oxymp, "updateCheckpoint", updateCheckpoint);
    addFunction(context, oxymp, "removeCheckpoint", removeCheckpoint);
    addFunction(context, oxymp, "setWeather", setWeather);
    addFunction(context, oxymp, "setTime", setTime);

    // Классы кладутся туда же: скрипту они нужны не для того, чтобы заводить
    // сущности, а для проверок вида `x instanceof oxymp.Player`.
    (void)oxymp->Set(context, toJs(isolate, "Player"),
                     playerShape->GetFunction(context).ToLocalChecked());
    (void)oxymp->Set(context, toJs(isolate, "Vehicle"),
                     vehicleShape->GetFunction(context).ToLocalChecked());
    (void)oxymp->Set(context, toJs(isolate, "Object"),
                     objectShape->GetFunction(context).ToLocalChecked());

    // Имя ресурса — чтобы он мог собрать путь к своим файлам и назвать себя в
    // событии клиенту.
    (void)oxymp->Set(context, toJs(isolate, "resourceName"), toJs(isolate, resource.name()));

    // Корень ресурса: слою alt:V он нужен, чтобы отдать `alt.Resource.path`, а
    // ресурсу — чтобы дотянуться до своих файлов, не гадая о рабочем каталоге
    // сервера.
    (void)oxymp->Set(context, toJs(isolate, "resourcePath"),
                     toJs(isolate, resource.root().string()));

    (void)oxymp->Set(context, toJs(isolate, "version"),
                     toJs(isolate, std::to_string(shared::kProtocolVersion)));

    (void)oxymp->Set(context, toJs(isolate, "tickRate"),
                     v8::Number::New(isolate, shared::kDefaultTickRate));

    (void)context->Global()->Set(context, toJs(isolate, "oxymp"), oxymp);

    // Тот же объект — под именем, которым им пользуется слой alt:V.
    //
    // Отдельным именем, а не переиспользованием `oxymp`, по двум причинам. Слой
    // alt:V складывает сюда же и своё (shared, server, enums), и мешать это с
    // тем, что видит скрипт, значило бы показать ресурсу свою кухню. А имя с
    // двумя подчёркиваниями впереди говорит читающему ресурс, что трогать это
    // не следует, — тогда как `oxymp` трогать как раз можно и нужно.
    const v8::Local<v8::Object> internals = v8::Object::New(isolate);
    (void)internals->Set(context, toJs(isolate, "native"), oxymp);

    (void)context->Global()->Set(context, toJs(isolate, "__oxympAlt"), internals);
}

} // namespace oxymp::script::js
