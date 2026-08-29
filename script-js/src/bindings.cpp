#include "bindings.hpp"

#include "convert.hpp"
#include "resource.hpp"

#include <oxymp/script/js/alt_seat.hpp>

#include <oxymp/shared/math/joaat.hpp>
#include <oxymp/shared/protocol/protocol_version.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
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

/// Число в шестьдесят четыре разряда, отданное строкой.
///
/// Строкой, потому что строкой отдаёт его alt:V (`readonly hwidHash: string`), и
/// потому что иначе его не отдать вовсе: числа в JS хранятся как double, и
/// отпечаток машины потерял бы младшие разряды — два разных игрока сошлись бы в
/// одном числе там, где различались бы последними цифрами.
template<auto Field>
void playerBigNumber(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);
    if (!player) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    // Ноль означает «неизвестно», и наружу он выходит пустой строкой, а не
    // нулём: «0» читалось бы как настоящий отпечаток, и режим, запрещающий по
    // нему вход, запретил бы всем сразу.
    const auto value = (*player).*Field;

    info.GetReturnValue().Set(
        toJs(isolate, value == 0 ? std::string{} : std::to_string(value)));
}

/// Скорость игрока: полная, вперёд и вбок.
///
/// Считается из той же скорости, что едет в снимке, а не спрашивается отдельно:
/// второго источника у неё нет, и заводить его значило бы завести второй ответ
/// на один вопрос.
///
/// Вперёд и вбок — это та же скорость, повёрнутая в сторону взгляда: у alt:V
/// `forwardSpeed` и `strafeSpeed` именно таковы. Знак у `forwardSpeed`
/// отрицателен, когда человек пятится, — иначе идущий назад был бы неотличим от
/// идущего вперёд.
enum class SpeedKind : std::uint8_t { Total, Forward, Strafe };

template<SpeedKind Kind>
void playerSpeed(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);
    if (!player) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    const shared::Vec3& speed = player->velocity;

    // Ветвями, а не выходом посреди: выход из первой оставлял бы остальное
    // недостижимым для неё, и /W4 справедливо ругался (C4702).
    if constexpr (Kind == SpeedKind::Total) {
        info.GetReturnValue().Set(
            std::sqrt((speed.x * speed.x) + (speed.y * speed.y) + (speed.z * speed.z)));
    } else {
        // Направление взгляда у игры считается от севера по часовой стрелке, а
        // синус с косинусом — от востока против неё. Отсюда и перестановка осей:
        // «вперёд» это (-sin, cos), а не (cos, sin).
        const float radians = player->heading * shared::kRadians;
        const float forwardX = -std::sin(radians);
        const float forwardY = std::cos(radians);

        if constexpr (Kind == SpeedKind::Forward) {
            info.GetReturnValue().Set((speed.x * forwardX) + (speed.y * forwardY));
        } else {
            // Вбок — та же скорость, спроецированная на перпендикуляр к взгляду.
            info.GetReturnValue().Set((speed.x * forwardY) - (speed.y * forwardX));
        }
    }
}

/// Стоит ли у игрока этот признак состояния.
///
/// Отдельным обработчиком на признак, а не одним числом наружу: у alt:V это
/// два десятка отдельных полей — `isAiming`, `isDead`, `isReloading`, — и
/// режим спрашивает именно их. Отдай мы число, всякий ресурс начинался бы с
/// собственного разбора наших битов, которых он не знает.
///
/// Признак читается из снимка на каждое обращение, как и всё остальное здесь:
/// запомненный, он врал бы тем убедительнее, чем дольше его не трогали.
template<shared::PlayerFlag Flag>
void playerFlag(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);
    if (!player) {
        // Вышедший отвечает пустотой, а не «нет»: «нет» означало бы, что игрок
        // есть и не целится, — а его нет вовсе.
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(shared::has(player->flags, Flag));
}

/// В воде ли он.
///
/// Своим обработчиком, потому что у нас это два признака, а у alt:V один:
/// плывущий по поверхности и ушедший под воду для него одинаково `isInWater`.
void playerInWater(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);
    if (!player) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(shared::has(player->flags, shared::PlayerFlag::Swimming) ||
                              shared::has(player->flags, shared::PlayerFlag::Diving));
}

/// Вышел ли игрок в мир.
///
/// У alt:V `isSpawned` означает «персонаж заведён и стоит в мире». Ближайшая
/// правда, которая у нас есть, — объявил ли игрок свою внешность: до этого
/// мгновения его персонажа нет ни у кого, включая его самого. Догадкой это не
/// назовёшь: модель приезжает ровно тогда, когда игра завела тело.
void playerSpawned(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);

    info.GetReturnValue().Set(player.has_value() && player->model != 0);
}

/// Место в машине, в нумерации alt:V.
///
/// Своим обработчиком, а не через playerField: в снимке место лежит в
/// нумерации игры, а наружу обязано уйти в нумерации alt:V (alt_seat.hpp).
void playerSeat(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);
    if (!player) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(static_cast<double>(toAltSeat(player->seat)));
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

    // Обрезается по пределу самого игрока, а не по сотне: тяжёлый бронежилет в
    // режимах — это поднятый предел, и обрезка по общей сотне обращала бы его в
    // украшение. Сотня же обрезала здесь раньше, и `maxArmour = 200` не значил
    // ничего: броня всё равно вставала на сто.
    //
    // Само ядро обрежет ещё раз, и это не лишнее: сюда попадает не всякое
    // изменение брони.
    (void)core.setHealth(
        *id, player->health,
        static_cast<std::uint16_t>(std::clamp<std::int64_t>(*armour, 0, player->maxArmour)));
}

/// Ставит предел брони.
///
/// Отдельно от самой брони, как и у alt:V: поднявший предел не обязан тут же
/// выдавать бронежилет. Уже надетая при этом обрезается по новому пределу.
void setPlayerMaxArmour(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                        const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = idOf<shared::PlayerId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> limit = intFromJs(isolate->GetCurrentContext(), value);
    if (!limit) {
        return;
    }

    (void)resourceOf(isolate).core().setMaxArmour(
        *id, static_cast<std::uint16_t>(std::clamp<std::int64_t>(*limit, 0, 0xFFFF)));
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
        *id, static_cast<std::uint8_t>(*component), static_cast<std::uint16_t>(*drawable),
        static_cast<std::uint16_t>(texture), static_cast<std::uint8_t>(palette));

    info.GetReturnValue().Set(done);
}

/// Кладёт поле в объект. Помощник для ответов, состоящих из нескольких чисел.
void putNumber(v8::Local<v8::Context> context, const v8::Local<v8::Object>& target,
               const char* name, double value) {
    (void)target->Set(context, toJs(context->GetIsolate(), std::string{name}),
                      v8::Number::New(context->GetIsolate(), value));
}

/// То же для признака. Своим помощником, а не через putNumber: единица и ноль
/// вместо true и false — законные числа, и ресурс, сравнивший их через `===`,
/// не сошёлся бы ни разу.
void putFlag(v8::Local<v8::Context> context, const v8::Local<v8::Object>& target,
             const char* name, bool value) {
    (void)target->Set(context, toJs(context->GetIsolate(), std::string{name}),
                      v8::Boolean::New(context->GetIsolate(), value));
}

/// Ставит признак распоряжения о теле: заморозку или неуязвимость.
///
/// Шаблоном на оба, потому что отличаются они одним вызовом ядра: подпись,
/// разбор довода и отказ у них одни и те же, а два почти одинаковых тела — это
/// два места, где легко разойтись.
template<bool (Core::*Method)(shared::PlayerId, bool)>
void playerSetSwitch(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                     const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = idOf<shared::PlayerId>(info.This());
    if (!id) {
        return;
    }

    // Любое значение годится: у alt:V это признак, и ресурсы пишут туда что
    // угодно осмысленное — `1`, `!!x`, `state`. Отказывать на «не то» здесь
    // было бы строже самого alt:V.
    (resourceOf(isolate).core().*Method)(*id, value->BooleanValue(isolate));
}

/// Стоит ли у игрока признак распоряжения о теле.
template<shared::PlayerControlFlag Flag>
void playerSwitch(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<PlayerInfo> player = resourceOf(isolate).core().player(*id);
    if (!player) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(shared::has(player->control, Flag));
}

/// Есть ли у игрока такой ствол.
void playerHasWeapon(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> weapon = argAt(info, 0);

    if (!id || !weapon) {
        fail(isolate, "hasWeapon ждёт хеш оружия числом");
        return;
    }

    const std::vector<shared::WeaponSlot> carried = resourceOf(isolate).core().loadout(*id);
    const auto hash = static_cast<std::uint32_t>(*weapon);

    info.GetReturnValue().Set(
        std::ranges::find(carried, hash, &shared::WeaponSlot::weapon) != carried.end());
}

/// Сколько патронов сервер выдал к этому стволу.
///
/// Именно выдал, а не сколько осталось в магазине сию секунду: тратит их игра у
/// владельца, и сервер узнаёт остаток из снимков. Число здесь — то, что уйдёт
/// игроку, если вернуть ему снаряжение.
/// Состав и расцветка того ствола, что сейчас в руках.
///
/// Ствол в руках называет снимок, а его подробности лежат в снаряжении: это
/// два разных источника, и сводятся они здесь. Пустота означает «в руках
/// ничего, о чём мы знаем» — кулаки в снаряжении не числятся.
template<bool Components>
void playerHeldWeaponDetail(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    Core& core = resourceOf(isolate).core();

    const std::optional<PlayerInfo> player = core.player(*id);
    if (!player) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    const std::vector<shared::WeaponSlot> carried = core.loadout(*id);
    const auto found = std::ranges::find(carried, player->weapon, &shared::WeaponSlot::weapon);

    if (found == carried.end()) {
        if constexpr (Components) {
            info.GetReturnValue().Set(v8::Array::New(isolate, 0));
        } else {
            info.GetReturnValue().Set(0);
        }

        return;
    }

    if constexpr (Components) {
        const v8::Local<v8::Array> list =
            v8::Array::New(isolate, static_cast<int>(found->components.size()));

        std::uint32_t at = 0;
        for (const std::uint32_t component : found->components) {
            (void)list->Set(context, at++, v8::Number::New(isolate, component));
        }

        info.GetReturnValue().Set(list);
    } else {
        info.GetReturnValue().Set(static_cast<double>(found->tint));
    }
}

/// Меняет боезапас у оружия, которое у игрока уже есть.
void playerSetWeaponAmmo(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> weapon = argAt(info, 0);
    const std::optional<std::int64_t> ammo = argAt(info, 1);

    if (!id || !weapon || !ammo) {
        fail(isolate, "setWeaponAmmo ждёт хеш оружия и число патронов");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().setWeaponAmmo(
        *id, static_cast<std::uint32_t>(*weapon),
        static_cast<std::uint16_t>(std::clamp<std::int64_t>(*ammo, 0, 0xFFFF))));
}

void playerWeaponAmmo(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> weapon = argAt(info, 0);

    if (!id || !weapon) {
        fail(isolate, "getWeaponAmmo ждёт хеш оружия числом");
        return;
    }

    const std::vector<shared::WeaponSlot> carried = resourceOf(isolate).core().loadout(*id);
    const auto found =
        std::ranges::find(carried, static_cast<std::uint32_t>(*weapon), &shared::WeaponSlot::weapon);

    if (found == carried.end()) {
        // Ствола нет — пустота, а не ноль. Ноль означал бы «есть и пуст», и
        // режим, решающий по нему выдать патроны, выдал бы их в никуда.
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(static_cast<double>(found->ammo));
}

/// Чем игрок вооружён: список того же вида, что у alt:V.
void playerWeapons(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::vector<shared::WeaponSlot> carried = resourceOf(isolate).core().loadout(*id);

    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Array> list = v8::Array::New(isolate, static_cast<int>(carried.size()));

    for (std::size_t at = 0; at < carried.size(); ++at) {
        const shared::WeaponSlot& slot = carried[at];
        const v8::Local<v8::Object> entry = v8::Object::New(isolate);

        // Имена полей — alt:V: `hash`, `tintIndex`, `components`.
        putNumber(context, entry, "hash", slot.weapon);
        putNumber(context, entry, "ammo", slot.ammo);
        putNumber(context, entry, "tintIndex", slot.tint);

        const v8::Local<v8::Array> parts =
            v8::Array::New(isolate, static_cast<int>(slot.components.size()));

        for (std::size_t which = 0; which < slot.components.size(); ++which) {
            (void)parts->Set(context, static_cast<std::uint32_t>(which),
                             v8::Number::New(isolate,
                                             static_cast<double>(slot.components[which])));
        }

        (void)entry->Set(context, toJs(isolate, std::string{"components"}), parts);
        (void)list->Set(context, static_cast<std::uint32_t>(at), entry);
    }

    info.GetReturnValue().Set(list);
}

/// Отбирает один ствол.
void playerRemoveWeapon(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> weapon = argAt(info, 0);

    if (!id || !weapon) {
        fail(isolate, "removeWeapon ждёт хеш оружия числом");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().removeWeapon(*id, static_cast<std::uint32_t>(*weapon)));
}

/// Дробное из довода по месту. Пусто — довода нет или он не число.
///
/// Отдельно от argAt, потому что доли смешения лица дробные, а целочисленное
/// чтение обратило бы 0.75 в ноль — то есть в другое лицо, без единой жалобы.
[[nodiscard]] std::optional<double> realAt(const v8::FunctionCallbackInfo<v8::Value>& info,
                                           int index) {
    if (info.Length() <= index) {
        return std::nullopt;
    }

    double value = 0.0;
    if (!info[index]->NumberValue(info.GetIsolate()->GetCurrentContext()).To(&value)) {
        return std::nullopt;
    }

    return value;
}

/// Собирает лицо: два родителя, третий вклад и три доли смешения.
void playerSetHeadBlend(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    // Порядок доводов — alt:V: shapeFirstID, shapeSecondID, shapeThirdID,
    // skinFirstID, skinSecondID, skinThirdID, shapeMix, skinMix, thirdMix.
    // Переставить их местами значило бы собрать чужое лицо без единой жалобы.
    const auto whole = [&](int at) {
        return static_cast<std::uint8_t>(argAt(info, at).value_or(0));
    };

    const bool done = resourceOf(isolate).core().setHeadBlend(
        *id, whole(0), whole(1), whole(2), whole(3), whole(4), whole(5),
        static_cast<float>(realAt(info, 6).value_or(0.0)),
        static_cast<float>(realAt(info, 7).value_or(0.0)),
        static_cast<float>(realAt(info, 8).value_or(0.0)));

    info.GetReturnValue().Set(done);
}

/// Ставит слой лица: бороду, брови, макияж, шрамы.
void playerSetHeadOverlay(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> slot = argAt(info, 0);
    const std::optional<std::int64_t> index = argAt(info, 1);

    if (!slot || !index) {
        fail(isolate, "setHeadOverlay ждёт слой и его номер числами");
        return;
    }

    // Заметность необязательна: у alt:V она с умолчанием в единицу.
    const double opacity = realAt(info, 2).value_or(1.0);

    info.GetReturnValue().Set(resourceOf(isolate).core().setHeadOverlay(
        *id, static_cast<std::uint8_t>(*slot), static_cast<std::uint8_t>(*index),
        static_cast<float>(opacity)));
}

/// Красит слой лица.
void playerSetHeadOverlayColour(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> slot = argAt(info, 0);
    const std::optional<std::int64_t> kind = argAt(info, 1);
    const std::optional<std::int64_t> colour = argAt(info, 2);

    if (!slot || !kind || !colour) {
        fail(isolate, "setHeadOverlayColor ждёт слой, род цвета и сам цвет числами");
        return;
    }

    const std::int64_t second = argAt(info, 3).value_or(*colour);

    info.GetReturnValue().Set(resourceOf(isolate).core().setHeadOverlayColour(
        *id, static_cast<std::uint8_t>(*slot), static_cast<std::uint8_t>(*kind),
        static_cast<std::uint8_t>(*colour), static_cast<std::uint8_t>(second)));
}

/// Цвет волос и мелирования.
void playerSetHairColour(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> colour = argAt(info, 0);
    if (!colour) {
        fail(isolate, "setHairColor ждёт цвет числом");
        return;
    }

    // Мелирование необязательно: не названное, оно берётся тем же цветом —
    // тогда его попросту не видно. Ноль здесь означал бы чёрную прядь.
    const std::int64_t highlight = argAt(info, 1).value_or(*colour);

    info.GetReturnValue().Set(resourceOf(isolate).core().setHairColour(
        *id, static_cast<std::uint8_t>(*colour), static_cast<std::uint8_t>(highlight)));
}

/// Хеш из довода: число как есть либо имя, посчитанное joaat.
///
/// У alt:V набор и рисунок татуировки принимаются и числом, и строкой — так и
/// зовут их режимы: `addDecoration('mpbeach_overlays', 'FM_Hair_Fuzz')`.
[[nodiscard]] std::optional<std::uint32_t> hashAt(
    const v8::FunctionCallbackInfo<v8::Value>& info, int index) {
    if (info.Length() <= index) {
        return std::nullopt;
    }

    v8::Isolate* const isolate = info.GetIsolate();

    // Решается по числу, а не по строке, и это не вкусовщина: `IsString`
    // объявлен экспортируемым из библиотеки, но определён прямо в заголовке, и
    // линковщик отказывается выбирать между двумя его телами (LNK2005). Тот же
    // приём стоит у `player.model` — см. setPlayerModel.
    const std::optional<std::int64_t> number =
        intFromJs(isolate->GetCurrentContext(), info[index]);

    if (number) {
        return static_cast<std::uint32_t>(*number);
    }

    const std::uint32_t hashed = shared::joaat(fromJs(isolate, info[index]));

    // Пустое имя даёт нулевой хеш, и он же означает «ничего»: отличить одно от
    // другого нечем, и оба одинаково негодны.
    return hashed == 0 ? std::nullopt : std::optional{hashed};
}

/// Ставит татуировку.
void playerAddDecoration(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::uint32_t> collection = hashAt(info, 0);
    const std::optional<std::uint32_t> overlay = hashAt(info, 1);

    if (!id || !collection || !overlay) {
        fail(isolate, "addDecoration ждёт набор и рисунок — именем или хешем");
        return;
    }

    // Третий довод alt:V — сколько раз наложить рисунок. Хранится татуировка у
    // нас парой «набор и рисунок», и повторить её нечем: два одинаковых рисунка
    // на одном месте — это тот же самый рисунок. Молчать об этом нельзя, и
    // отказывать тоже: просивший один раз получил бы отказ ни за что.
    if (const std::optional<std::int64_t> count = argAt(info, 2); count && *count != 1) {
        static bool told = false;

        if (!told) {
            told = true;
            spdlog::warn("player.addDecoration: повтор рисунка не хранится — "
                         "татуировка описана набором и рисунком, и повторённая "
                         "она та же самая");
        }
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().addDecoration(*id, *collection, *overlay));
}

/// Снимает одну татуировку.
void playerRemoveDecoration(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::uint32_t> collection = hashAt(info, 0);
    const std::optional<std::uint32_t> overlay = hashAt(info, 1);

    if (!id || !collection || !overlay) {
        fail(isolate, "removeDecoration ждёт набор и рисунок — именем или хешем");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().removeDecoration(*id, *collection, *overlay));
}

/// Снимает все татуировки.
void playerClearDecorations(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().clearDecorations(*id));
}

/// Какие татуировки на нём стоят.
void playerDecorations(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<shared::PlayerAppearance> look = resourceOf(isolate).core().appearance(*id);

    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const std::size_t many = look ? look->decorations.size() : 0;

    const v8::Local<v8::Array> list = v8::Array::New(isolate, static_cast<int>(many));

    for (std::size_t at = 0; at < many; ++at) {
        const v8::Local<v8::Object> entry = v8::Object::New(isolate);

        // Имена полей — из `IDecoration` у alt:V.
        putNumber(context, entry, "collection", look->decorations[at].collection);
        putNumber(context, entry, "overlay", look->decorations[at].overlay);

        (void)list->Set(context, static_cast<std::uint32_t>(at), entry);
    }

    info.GetReturnValue().Set(list);
}

/// Двигает черту лица.
void playerSetFaceFeature(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> index = argAt(info, 0);

    if (!id || !index) {
        fail(isolate, "setFaceFeature ждёт номер черты числом");
        return;
    }

    // Доля дробная, и читать её целым нельзя: 0.75 обратилось бы в ноль, то
    // есть в другое лицо — без единой жалобы.
    const double scale = realAt(info, 1).value_or(0.0);

    info.GetReturnValue().Set(resourceOf(isolate).core().setFaceFeature(
        *id, static_cast<std::uint8_t>(*index), static_cast<float>(scale)));
}

/// Насколько сдвинута черта лица. Пусто — игрок не объявлял внешности.
void playerFaceFeature(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> index = argAt(info, 0);

    if (!id || !index) {
        fail(isolate, "getFaceFeatureScale ждёт номер черты числом");
        return;
    }

    const std::optional<shared::PlayerAppearance> look = resourceOf(isolate).core().appearance(*id);

    if (!look || *index < 0 ||
        *index >= static_cast<std::int64_t>(shared::kPedFaceFeatureCount)) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    // Обратно в долю от минус единицы до единицы — тем же числом ступеней,
    // каким её укладывали.
    const double scale =
        static_cast<double>(look->faceFeatures[static_cast<std::size_t>(*index)]) / 127.0;

    info.GetReturnValue().Set(scale);
}

/// Цвет глаз.
void playerSetEyeColour(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> colour = argAt(info, 0);
    if (!colour) {
        fail(isolate, "setEyeColor ждёт цвет числом");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().setEyeColour(*id, static_cast<std::uint8_t>(*colour)));
}

/// Что на игроке надето в этом слоте: вещь, расцветка, набор цветов.
///
/// Вопрос, а не распоряжение, и потому отсутствие ответа здесь — пустота, а не
/// молчание: у игрока, не объявившего внешности, одежды нет вовсе, и выдать за
/// неё нули значило бы соврать про голого как про одетого.
void playerGetClothes(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> slot = argAt(info, 0);

    if (!id || !slot) {
        fail(isolate, "getClothes ждёт слот числом");
        return;
    }

    const std::optional<shared::PlayerAppearance> look = resourceOf(isolate).core().appearance(*id);

    if (!look || *slot < 0 || *slot >= static_cast<std::int64_t>(shared::kPedComponentCount)) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    const shared::PedComponent& worn = look->components[static_cast<std::size_t>(*slot)];
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Object> answer = v8::Object::New(isolate);

    // Имена полей — alt:V: `drawable`, `texture`, `palette`. Своё написание
    // означало бы, что ответ есть, а читают из него пустоту.
    putNumber(context, answer, "drawable", worn.drawable);
    putNumber(context, answer, "texture", worn.texture);
    putNumber(context, answer, "palette", worn.palette);

    info.GetReturnValue().Set(answer);
}

/// Что на игроке за аксессуар в этом месте.
void playerGetProp(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> slot = argAt(info, 0);

    if (!id || !slot) {
        fail(isolate, "getProp ждёт место числом");
        return;
    }

    const std::optional<shared::PlayerAppearance> look = resourceOf(isolate).core().appearance(*id);

    if (!look || *slot < 0 || *slot >= static_cast<std::int64_t>(shared::kPedPropCount)) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    const shared::PedProp& worn = look->props[static_cast<std::size_t>(*slot)];
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Object> answer = v8::Object::New(isolate);

    putNumber(context, answer, "drawable", worn.drawable);
    putNumber(context, answer, "texture", worn.texture);

    info.GetReturnValue().Set(answer);
}

/// Из чего собрано лицо.
void playerGetHeadBlend(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<shared::PlayerAppearance> look = resourceOf(isolate).core().appearance(*id);
    if (!look) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Object> answer = v8::Object::New(isolate);

    // Имена полей взяты у alt:V из `IHeadBlendData` дословно.
    putNumber(context, answer, "shapeFirstID", look->shapeFirst);
    putNumber(context, answer, "shapeSecondID", look->shapeSecond);
    putNumber(context, answer, "shapeThirdID", look->shapeThird);
    putNumber(context, answer, "skinFirstID", look->skinFirst);
    putNumber(context, answer, "skinSecondID", look->skinSecond);
    putNumber(context, answer, "skinThirdID", look->skinThird);
    putNumber(context, answer, "shapeMix", look->shapeMix);
    putNumber(context, answer, "skinMix", look->skinMix);
    putNumber(context, answer, "thirdMix", look->thirdMix);

    info.GetReturnValue().Set(answer);
}

/// Что за слой стоит на лице и каким он покрашен.
void playerGetHeadOverlay(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> slot = argAt(info, 0);

    if (!id || !slot) {
        fail(isolate, "getHeadOverlay ждёт слой числом");
        return;
    }

    const std::optional<shared::PlayerAppearance> look = resourceOf(isolate).core().appearance(*id);

    if (!look || *slot < 0 || *slot >= static_cast<std::int64_t>(shared::kPedOverlayCount)) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    const shared::PedOverlay& layer = look->overlays[static_cast<std::size_t>(*slot)];
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Object> answer = v8::Object::New(isolate);

    // Имена полей — из `IHeadOverlay` у alt:V.
    putNumber(context, answer, "index", layer.index);
    putNumber(context, answer, "opacity", layer.opacity);
    putNumber(context, answer, "colorType", layer.colourType);
    putNumber(context, answer, "colorIndex", layer.colour);
    putNumber(context, answer, "secondColorIndex", layer.secondColour);

    info.GetReturnValue().Set(answer);
}

/// Отдельные числа внешности: цвет волос, мелирования и глаз.
template<std::uint8_t shared::PlayerAppearance::* Field>
void playerLookField(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::optional<shared::PlayerAppearance> look = resourceOf(isolate).core().appearance(*id);
    if (!look) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(static_cast<double>((*look).*Field));
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
        *id, static_cast<std::uint8_t>(*index), static_cast<std::int16_t>(*drawable),
        static_cast<std::int16_t>(texture));

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

/// Снимает всю одежду: каждый слот в нулевую вещь.
///
/// Нулевая вещь, а не «ничего»: у персонажа нет состояния «без слота» — есть
/// нулевой рисунок, и именно его игра показывает голым телом. Ровно так же
/// поступает и `clearClothes` у alt:V.
void playerClearClothes(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    // Слот называется доводом, и это не придирка. У alt:V объявлено
    // `clearClothes(component: number)` — снять одну вещь, а не раздеть
    // человека. Довод здесь однажды не читался вовсе, и всякий
    // `player.clearClothes(11)` снимал с игрока всё: и штаны, и обувь, и маску.
    // Ошибка молчала — вызов удавался, ответ был `true`.
    const std::optional<std::int64_t> slot = argAt(info, 0);

    if (!slot) {
        fail(isolate, "clearClothes ждёт номер слота одежды");
        return;
    }

    if (*slot < 0 || *slot >= static_cast<std::int64_t>(shared::kPedComponentCount)) {
        fail(isolate, "clearClothes: такого слота одежды у персонажа нет");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().setClothes(*id, static_cast<std::uint8_t>(*slot), 0, 0, 0));
}

/// Цвет мелирования отдельно от цвета волос.
///
/// У alt:V это два метода, а у ядра один вызов на оба цвета: игре они уходят
/// одним нативом. Поэтому цвет волос здесь спрашивается у самого игрока и
/// ставится обратно как есть — иначе просьба о мелировании перекрасила бы
/// заодно и волосы.
void playerSetHairHighlight(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const std::optional<std::int64_t> colour = argAt(info, 0);

    if (!id || !colour) {
        fail(isolate, "setHairHighlightColor ждёт цвет числом");
        return;
    }

    Core& core = resourceOf(isolate).core();

    const std::optional<shared::PlayerAppearance> look = core.appearance(*id);
    const std::uint8_t hair = look ? look->hairColour : 0;

    info.GetReturnValue().Set(
        core.setHairColour(*id, hair, static_cast<std::uint8_t>(*colour)));
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

/// Присваивание, которого мы не умеем исполнить.
///
/// Говорит о себе один раз в журнал и возвращается — не бросает. Разница не в
/// громкости, а в цене: присваивание стоит посреди чужого обработчика, и
/// брошенное отсюда исключение уносит с собой всё, что шло следом. Так уже было
/// дважды — `player.model` унёс показ интерфейса при входе, `vehicle.repair`
/// унёс починку машин целиком.
///
/// В C++, а не в слое на JavaScript, и это вынужденно: аксессоры ядра ставятся
/// на **экземпляр** (`InstanceTemplate`), а не на прототип, — и определение
/// того же имени на прототипе оказалось бы мёртвым кодом. Заметить это можно
/// было бы только по тому, что отказ молчит.
template<const char* What, const char* Why>
void refuseAssignment(v8::Local<v8::Name>, v8::Local<v8::Value>,
                      const v8::PropertyCallbackInfo<void>&) {
    static bool told = false;

    if (told) {
        return;
    }

    told = true;
    spdlog::warn("{}: {}", What, Why);
}

// Строки для отказов. Отдельными переменными, потому что доводом шаблона
// строковый литерал быть не может, а внешняя связь у них нужна затем, чтобы имя
// годилось в довод шаблона.
extern const char kBodyHealthWhat[] = "vehicle.bodyHealth";
extern const char kEngineHealthWhat[] = "vehicle.engineHealth";
extern const char kTankHealthWhat[] = "vehicle.petrolTankHealth";
extern const char kEngineOnWhat[] = "vehicle.engineOn";
extern const char kSirenWhat[] = "vehicle.sirenActive";

extern const char kLeaderOwnsIt[] =
    "машина живёт в игре у своего ведущего, и это назначает он, а не сервер; "
    "починить её целиком умеет vehicle.repair()";

/// Насколько открыта дверь. Числа — те же, что у alt:V: ноль закрыта, семёрка
/// настежь.
void vehicleDoorState(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    const std::optional<std::int64_t> door = argAt(info, 0);

    if (!id || !door) {
        fail(isolate, "getDoorState ждёт номер двери числом");
        return;
    }

    const std::optional<VehicleInfo> vehicle = resourceOf(isolate).core().vehicle(*id);

    if (!vehicle || *door < 0 || *door >= shared::kVehicleDoorCount) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set(static_cast<double>(
        shared::doorLevel(vehicle->doorLevels, static_cast<int>(*door))));
}

/// Открывает дверь на заданную степень или закрывает её.
void vehicleSetDoorState(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    const std::optional<std::int64_t> door = argAt(info, 0);
    const std::optional<std::int64_t> level = argAt(info, 1);

    if (!id || !door || !level) {
        fail(isolate, "setDoorState ждёт номер двери и её состояние числами");
        return;
    }

    if (*door < 0 || *door >= shared::kVehicleDoorCount || *level < 0 ||
        *level > static_cast<std::int64_t>(shared::kDoorFullyOpen)) {
        fail(isolate, "setDoorState: дверей шесть, состояний восемь — от 0 до 7");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().setVehicleDoor(
        *id, static_cast<std::uint8_t>(*door), static_cast<std::uint8_t>(*level)));
}

/// Опущено ли это стекло.
void vehicleWindowOpened(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    const std::optional<std::int64_t> window = argAt(info, 0);

    if (!id || !window) {
        fail(isolate, "isWindowOpened ждёт номер стекла числом");
        return;
    }

    if (*window < 0 || *window >= shared::kVehicleWindowCount) {
        fail(isolate, "isWindowOpened: стёкол у машины восемь — от 0 до 7");
        return;
    }

    const std::optional<VehicleInfo> vehicle = resourceOf(isolate).core().vehicle(*id);
    if (!vehicle) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    info.GetReturnValue().Set((vehicle->windowsOpen & (1U << *window)) != 0);
}

/// Опускает стекло или поднимает его.
void vehicleSetWindowOpened(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    const std::optional<std::int64_t> window = argAt(info, 0);

    if (!id || !window || info.Length() < 2) {
        fail(isolate, "setWindowOpened ждёт номер стекла и признак");
        return;
    }

    if (*window < 0 || *window >= shared::kVehicleWindowCount) {
        fail(isolate, "setWindowOpened: стёкол у машины восемь — от 0 до 7");
        return;
    }

    Core& core = resourceOf(isolate).core();

    const std::optional<VehicleInfo> vehicle = core.vehicle(*id);
    if (!vehicle) {
        info.GetReturnValue().Set(false);
        return;
    }

    // Правится одно стекло, а уходит весь набор: помнит их сервер целиком, и
    // отправлять по стеклу значило бы слать восемь сообщений об одной машине.
    const auto bit = static_cast<std::uint8_t>(1U << *window);
    const std::uint8_t wanted = info[1]->BooleanValue(isolate)
                                    ? static_cast<std::uint8_t>(vehicle->windowsOpen | bit)
                                    : static_cast<std::uint8_t>(vehicle->windowsOpen & ~bit);

    info.GetReturnValue().Set(core.setVehicleWindows(*id, wanted));
}

/// Запирает машину или отпирает её.
void setVehicleLock(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                    const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<std::int64_t> state = intFromJs(isolate->GetCurrentContext(), value);
    if (!state) {
        return;
    }

    // Числа замка — от нуля до семи, и все они значащие. Восьмёрка и дальше не
    // значат ничего: игра истолковала бы их по-своему, а по-своему — значит
    // непредсказуемо.
    if (*state < 0 || *state > 7) {
        fail(isolate, "lockState: у замка машины восемь значений, от 0 до 7");
        return;
    }

    (void)resourceOf(isolate).core().setVehicleLock(*id, static_cast<std::uint8_t>(*state));
}

/// Стоит ли у машины этот признак. Пара к playerFlag, и по той же причине.
template<shared::VehicleFlag Flag>
void vehicleFlag(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
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

    info.GetReturnValue().Set(shared::has(vehicle->flags, Flag));
}

/// Кто в машине сидит: объект «место — игрок», как у alt:V.
///
/// Места в его нумерации: единица — водитель. У alt:V это `passengers`, и он
/// же приводит в описании пример с `Object.entries` — значит объект, а не
/// массив, и ключи в нём строки.
void vehiclePassengers(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
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

    const v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const v8::Local<v8::Object> answer = v8::Object::New(isolate);

    for (const auto& [seat, player] : vehicle->passengers) {
        (void)answer->Set(context, toJs(isolate, std::to_string(toAltSeat(seat))),
                          wrapPlayer(resourceOf(isolate), context, player));
    }

    info.GetReturnValue().Set(answer);
}

/// Переставляет предмет: точка и, если названа, поворот.
///
/// Одним действием на то и другое, а не двумя свойствами: до клиента и то и
/// другое доходит одним объявлением, и разделять их значило бы слать два там,
/// где хватает одного. Свойства `pos` и `rot` слой alt:V строит поверх этого.
void objectPlace(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::ObjectId> id = idOf<shared::ObjectId>(info.This());
    if (!id) {
        fail(isolate, "place зовётся у предмета");
        return;
    }

    Core& core = resourceOf(isolate).core();

    const std::optional<ObjectInfo> object = core.object(*id);
    if (!object) {
        info.GetReturnValue().Set(false);
        return;
    }

    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    // Не названное берётся у самого предмета: перестановка не должна
    // разворачивать его на север, а разворот — утаскивать в начало координат.
    const std::optional<shared::Vec3> position =
        info.Length() >= 1 ? vec3FromJs(context, info[0]) : std::nullopt;
    const std::optional<shared::Vec3> rotation =
        info.Length() >= 2 ? vec3FromJs(context, info[1]) : std::nullopt;

    info.GetReturnValue().Set(core.moveObject(*id, position.value_or(object->position),
                                              rotation.value_or(object->rotation)));
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

/// Назначает машине ведущего.
///
/// Игрок приходит сущностью, как у alt:V; числом — тоже принимаем: номер игрока
/// у нас и есть его имя.
void vehicleSetNetOwner(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        fail(isolate, "setNetOwner зовётся у машины");
        return;
    }

    std::optional<shared::PlayerId> owner;

    if (info.Length() >= 1) {
        owner = idOf<shared::PlayerId>(info[0]);

        if (!owner) {
            if (const std::optional<std::int64_t> number =
                    intFromJs(isolate->GetCurrentContext(), info[0])) {
                owner = static_cast<shared::PlayerId>(*number);
            }
        }
    }

    if (!owner) {
        fail(isolate, "setNetOwner ждёт игрока первым доводом");
        return;
    }

    // Второй довод у alt:V называется disableMigration и по умолчанию ложь.
    const bool sticky = info.Length() >= 2 && info[1]->BooleanValue(isolate);

    info.GetReturnValue().Set(resourceOf(isolate).core().setVehicleOwner(*id, *owner, sticky));
}

/// Возвращает выбор ведущего серверу.
void vehicleResetNetOwner(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());
    if (!id) {
        fail(isolate, "resetNetOwner зовётся у машины");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().clearVehicleOwner(*id));
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
    //
    // Число здесь в нумерации alt:V, а не игры: единица — водитель. Перевод —
    // alt_seat.hpp, и до него просьба «посади за руль» усаживала на второе
    // пассажирское место молча.
    const std::int64_t seat = argAt(info, 1).value_or(kAltDriverSeat);

    info.GetReturnValue().Set(
        resourceOf(isolate).core().setIntoVehicle(*id, *vehicle, fromAltSeat(seat)));
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

    // Третий довод — вложить ли выданное в руки. Он есть у alt:V, и терялся
    // здесь молча: режим просил вооружить человека, получал `true` и видел его
    // с пустыми руками.
    const bool equip = info.Length() >= 3 && info[2]->BooleanValue(isolate);

    info.GetReturnValue().Set(resourceOf(isolate).core().giveWeapon(
        *id, static_cast<std::uint32_t>(*weapon),
        static_cast<std::uint16_t>(std::clamp<std::int64_t>(ammo.value_or(0), 0, 0xFFFF)), equip));
}

/// Читает у вызова хеш ствола и второе число: хеш насадки либо номер расцветки.
///
/// Общее на три метода, потому что доводы у них одинаковы до последнего: два
/// числа и игрок, у которого их спрашивают. Пусто — назвали не то.
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> weaponPairFromJs(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
    const v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();

    const std::optional<std::int64_t> weapon =
        info.Length() >= 1 ? intFromJs(context, info[0]) : std::nullopt;

    const std::optional<std::int64_t> second =
        info.Length() >= 2 ? intFromJs(context, info[1]) : std::nullopt;

    if (!weapon || !second) {
        return std::nullopt;
    }

    return std::pair{static_cast<std::uint32_t>(*weapon), static_cast<std::uint32_t>(*second)};
}

/// Стоит ли на этом стволе эта насадка.
void playerHasWeaponComponent(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    const auto named = weaponPairFromJs(info);

    if (!id || !named) {
        fail(isolate, "hasWeaponComponent ждёт хеши оружия и насадки");
        return;
    }

    const std::vector<shared::WeaponSlot> carried = resourceOf(isolate).core().loadout(*id);
    const auto found = std::ranges::find(carried, named->first, &shared::WeaponSlot::weapon);

    info.GetReturnValue().Set(found != carried.end() &&
                              std::ranges::find(found->components, named->second) !=
                                  found->components.end());
}

void playerAddWeaponComponent(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const auto named = weaponPairFromJs(info);
    if (!named) {
        fail(isolate, "addWeaponComponent ждёт числовые хеши оружия и насадки");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().addWeaponComponent(*id, named->first, named->second));
}

void playerRemoveWeaponComponent(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const auto named = weaponPairFromJs(info);
    if (!named) {
        fail(isolate, "removeWeaponComponent ждёт числовые хеши оружия и насадки");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().removeWeaponComponent(*id, named->first, named->second));
}

void playerSetWeaponTint(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const auto named = weaponPairFromJs(info);
    if (!named) {
        fail(isolate, "setWeaponTintIndex ждёт хеш оружия и номер расцветки");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().setWeaponTint(
        *id, named->first, static_cast<std::uint8_t>(named->second)));
}

void playerClearWeapons(const v8::FunctionCallbackInfo<v8::Value>& info) {
    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    // Довод alt:V — отбирать ли заодно патроны. У нас патроны лежат внутри
    // самого ствола (`WeaponSlot::ammo`), и оставить их без него нечем: слот
    // уходит целиком. Просящий отобрать всё получает ровно то, о чём просит;
    // просящий оставить патроны — узнаёт, что так мы не умеем.
    if (const std::optional<std::int64_t> keepAmmo = argAt(info, 0);
        keepAmmo && *keepAmmo == 0) {
        static bool told = false;

        if (!told) {
            told = true;
            spdlog::warn("player.removeAllWeapons: патроны отбираются вместе со стволами — "
                         "они лежат внутри слота оружия, а не отдельно");
        }
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
    /// Признак с умолчанием. Умолчание нужно не для красоты: у `global` оно
    /// «да», и не назвавший его слой получил бы метку, не видимую никому.
    [[nodiscard]] bool flag(const char* name, bool fallback = false) const {
        v8::Local<v8::Value> field;

        if (!object_->Get(context_, toJs(context_->GetIsolate(), name)).ToLocal(&field)) {
            return fallback;
        }

        if (field->IsUndefined() || field->IsNull()) {
            return fallback;
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

/// Велит персонажу отыграть сценарий игры: сесть, закурить, облокотиться.
///
/// Отдельным вызовом, как у alt:V (`player.playScenario(name)`), но по сети едет
/// тем же распоряжением, что и движение: занят персонаж одинаково — задачей,
/// которую выдали не мы, — и вся обвязка вокруг движения достаётся сценарию
/// даром.
void playerPlayScenario(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PlayerId> id = selfPlayer(info);
    if (!id) {
        return;
    }

    const std::string name =
        info.Length() >= 1 ? fromJs(isolate, info[0]) : std::string{};

    if (name.empty()) {
        fail(isolate, "playScenario ждёт имя сценария строкой");
        return;
    }

    AnimationInfo animation;
    animation.scenario = name;

    info.GetReturnValue().Set(resourceOf(isolate).core().playAnimation(*id, animation));
}

/// Просит игрока сказать реплику: номер, имя реплики, настроение и голос.
void playSpeech(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<std::int64_t> id =
        info.Length() >= 1 ? intFromJs(context, info[0]) : std::nullopt;

    if (!id || info.Length() < 2) {
        fail(isolate, "playSpeech ждёт номер игрока и имя реплики");
        return;
    }

    const std::string speech = fromJs(isolate, info[1]);
    const std::string params = info.Length() >= 3 ? fromJs(isolate, info[2]) : std::string{};
    const std::string voice = info.Length() >= 4 ? fromJs(isolate, info[3]) : std::string{};

    info.GetReturnValue().Set(resourceOf(isolate).core().playSpeech(
        static_cast<shared::PlayerId>(*id), speech, params, voice));
}

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

    put("customPrimaryRed", look.customPrimaryRed);
    put("customPrimaryGreen", look.customPrimaryGreen);
    put("customPrimaryBlue", look.customPrimaryBlue);
    put("customSecondaryRed", look.customSecondaryRed);
    put("customSecondaryGreen", look.customSecondaryGreen);
    put("customSecondaryBlue", look.customSecondaryBlue);

    (void)object->Set(context, toJs(isolate, "customPrimary"),
                      v8::Boolean::New(isolate, look.customPrimary));
    (void)object->Set(context, toJs(isolate, "customSecondary"),
                      v8::Boolean::New(isolate, look.customSecondary));

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

    look.customPrimary = fields->flag("customPrimary");
    look.customPrimaryRed = fields->byte("customPrimaryRed", 0);
    look.customPrimaryGreen = fields->byte("customPrimaryGreen", 0);
    look.customPrimaryBlue = fields->byte("customPrimaryBlue", 0);

    look.customSecondary = fields->flag("customSecondary");
    look.customSecondaryRed = fields->byte("customSecondaryRed", 0);
    look.customSecondaryGreen = fields->byte("customSecondaryGreen", 0);
    look.customSecondaryBlue = fields->byte("customSecondaryBlue", 0);

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

// --- Прохожие ---------------------------------------------------------------
//
// Мост плоский, как у картинок: кукла приходит и уходит объектом с полями, а не
// сущностью с методами. Сущностью её делает слой alt:V, поверх этого.

[[nodiscard]] std::optional<PedInfo> pedFromJs(v8::Local<v8::Context> context,
                                               v8::Local<v8::Value> value) {
    const std::optional<Fields> fields = fieldsOf(context, value);
    if (!fields) {
        return std::nullopt;
    }

    PedInfo ped;
    ped.model = static_cast<std::uint32_t>(fields->number("model", 0.0));
    ped.position = fields->point("position", {});
    ped.rotation = fields->point("rotation", {});

    // Две сотни — не наша выдумка, а здоровье прохожего у самой игры: столько же
    // ставит и alt:V тому, кто о здоровье не сказал ничего.
    ped.health = static_cast<std::uint16_t>(fields->number("health", 200.0));
    ped.maxHealth = static_cast<std::uint16_t>(fields->number("maxHealth", 200.0));
    ped.armour = static_cast<std::uint16_t>(fields->number("armour", 0.0));
    ped.weapon = static_cast<std::uint32_t>(fields->number("weapon", 0.0));
    ped.dimension = static_cast<std::int32_t>(fields->number("dimension", 0.0));

    return ped;
}

/// Свойство прохожего, взятое из его снимка.
///
/// Снимок берётся заново на каждое обращение — по тому же правилу, что и у
/// игрока с машиной: слой не хранит правду о мире, она лежит в реестре сервера.
template<auto Field>
void pedField(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PedId> id = idOf<shared::PedId>(info.This());
    if (!id) {
        return;
    }

    const std::optional<PedInfo> ped = resourceOf(isolate).core().ped(*id);
    if (!ped) {
        info.GetReturnValue().SetUndefined();
        return;
    }

    if constexpr (std::is_same_v<std::decay_t<decltype((*ped).*Field)>, shared::Vec3>) {
        info.GetReturnValue().Set(toJs(isolate->GetCurrentContext(), (*ped).*Field));
    } else {
        info.GetReturnValue().Set(static_cast<double>((*ped).*Field));
    }
}

/// Правит одно число у прохожего, оставляя остальное как есть.
///
/// Свой сеттер у каждого поля — не роскошь, а необходимость, и стоила она часа
/// поисков. Свойство заготовки, объявленное одним геттером, присваиванием не
/// отказывает: V8 кладёт на его место обычное значение, и оно закрывает собой
/// геттер навсегда. Снаружи это выглядит так, будто присваивание удалось —
/// прочитанное обратно отдаёт новое число, — а до ядра оно не дошло вовсе.
///
/// Ловится это только сравнением с тем, что видит клиент, и потому не ловится
/// почти никогда.
template<auto Field>
void setPedField(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                 const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PedId> id = idOf<shared::PedId>(info.This());
    const std::optional<std::int64_t> fresh = intFromJs(isolate->GetCurrentContext(), value);

    if (!id || !fresh) {
        return;
    }

    Core& core = resourceOf(isolate).core();

    // Читается заново, а не берётся у скрипта: правится одно поле, а ядру нужно
    // описание целиком — остальное обязано остаться тем, что есть сейчас, а не
    // тем, что было, когда скрипт в последний раз смотрел.
    std::optional<PedInfo> ped = core.ped(*id);
    if (!ped) {
        return;
    }

    using FieldType = std::decay_t<decltype((*ped).*Field)>;
    (*ped).*Field = static_cast<FieldType>(*fresh);

    (void)core.updatePed(*id, *ped);
}

void pedValid(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    const std::optional<shared::PedId> id = idOf<shared::PedId>(info.This());

    info.GetReturnValue().Set(id.has_value() &&
                              resourceOf(info.GetIsolate()).core().ped(*id).has_value());
}

/// Правит прохожего целиком: описание приходит объектом.
///
/// Целиком, а не по полю, потому что целиком его правит и ядро: тюнинг куклы —
/// здоровье, броня, оружие — уходит клиентам одним сообщением. Собирает полное
/// описание слой alt:V: он читает свойства обратно и подменяет названное.
void pedUpdate(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PedId> id = idOf<shared::PedId>(info.This());
    if (!id) {
        fail(isolate, "update зовётся у прохожего");
        return;
    }

    const std::optional<PedInfo> ped =
        info.Length() >= 1 ? pedFromJs(isolate->GetCurrentContext(), info[0]) : std::nullopt;

    if (!ped) {
        fail(isolate, "update ждёт описание прохожего объектом");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().updatePed(*id, *ped));
}

void pedDestroy(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PedId> id = idOf<shared::PedId>(info.This());
    if (!id) {
        fail(isolate, "destroy зовётся у прохожего");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().removePed(*id));
}

void setPedDimensionValue(v8::Local<v8::Name>, v8::Local<v8::Value> value,
                          const v8::PropertyCallbackInfo<void>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<shared::PedId> id = idOf<shared::PedId>(info.This());
    const std::optional<std::int64_t> dimension =
        intFromJs(isolate->GetCurrentContext(), value);

    if (!id || !dimension) {
        return;
    }

    (void)resourceOf(isolate).core().setPedDimension(*id,
                                                     static_cast<std::int32_t>(*dimension));
}

void peds(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    Resource& resource = resourceOf(isolate);

    const std::vector<PedInfo> all = resource.core().peds();
    const v8::Local<v8::Array> array = v8::Array::New(isolate, static_cast<int>(all.size()));

    for (std::size_t i = 0; i < all.size(); ++i) {
        (void)array->Set(context, static_cast<std::uint32_t>(i),
                         wrapPed(resource, context, all[i].id));
    }

    info.GetReturnValue().Set(array);
}

void createPed(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<PedInfo> ped =
        info.Length() >= 1 ? pedFromJs(context, info[0]) : std::nullopt;

    if (!ped) {
        fail(isolate, "createPed ждёт описание прохожего объектом");
        return;
    }

    Resource& resource = resourceOf(isolate);
    const shared::PedId id = resource.core().createPed(*ped);

    if (id == shared::kInvalidPedId) {
        // Пустота, а не исключение: отказ здесь — обычный ход дела (исчерпан
        // предел кукол в сессии), и обрывать им скрипт незачем.
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(wrapPed(resource, context, id));
}

// --- Привязка сущностей -----------------------------------------------------
//
// Мост плоский, как и у картинок: род и номер отдельными доводами, остальное —
// описанием. Род словом, а не числом, по той же причине, по какой словом ходят
// рода в метаданных: в журнале ресурса «object» читается, а «3» — нет.

/// Род сущности по слову. None — слово не то.
[[nodiscard]] shared::EntityKind kindFromJs(std::string_view word) {
    if (word == "player") {
        return shared::EntityKind::Player;
    }
    if (word == "vehicle") {
        return shared::EntityKind::Vehicle;
    }
    if (word == "object") {
        return shared::EntityKind::Object;
    }
    if (word == "ped") {
        return shared::EntityKind::Ped;
    }

    return shared::EntityKind::None;
}

/// Род и номер сущности из пары доводов. Пусто — доводы не те.
[[nodiscard]] std::optional<EntityRef> entityFromJs(const v8::FunctionCallbackInfo<v8::Value>& info,
                                                    int first) {
    v8::Isolate* const isolate = info.GetIsolate();

    if (info.Length() <= first + 1) {
        return std::nullopt;
    }

    const shared::EntityKind kind = kindFromJs(fromJs(isolate, info[first]));
    const std::optional<std::int64_t> id =
        intFromJs(isolate->GetCurrentContext(), info[first + 1]);

    if (kind == shared::EntityKind::None || !id) {
        return std::nullopt;
    }

    return EntityRef{.kind = kind, .id = static_cast<std::uint32_t>(*id)};
}

void attachEntity(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<EntityRef> entity = entityFromJs(info, 0);
    const std::optional<Fields> fields =
        info.Length() >= 3 ? fieldsOf(context, info[2]) : std::nullopt;

    if (!entity || !fields) {
        fail(isolate, "attachEntity ждёт род, номер и описание привязки объектом");
        return;
    }

    AttachmentInfo attachment;
    attachment.target = EntityRef{
        .kind = kindFromJs(fields->text("targetKind")),
        .id = static_cast<std::uint32_t>(fields->number("target", 0.0)),
    };

    attachment.bone = static_cast<std::int32_t>(fields->number("bone", -1.0));
    attachment.boneName = fields->text("boneName");
    attachment.position = fields->point("position", {});
    attachment.rotation = fields->point("rotation", {});
    attachment.collision = fields->flag("collision");

    // Держать поворот намертво — по умолчанию: у alt:V довод назван наоборот, и
    // ресурс, не назвавший его, ждёт именно закреплённого поворота.
    attachment.fixedRotation = fields->flagOr("fixedRotation", true);

    if (attachment.target.kind == shared::EntityKind::None) {
        fail(isolate, "attachEntity ждёт род цели: player, vehicle или object");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().attachEntity(*entity, attachment));
}

void detachEntity(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    const std::optional<EntityRef> entity = entityFromJs(info, 0);
    if (!entity) {
        fail(isolate, "detachEntity ждёт род и номер сущности");
        return;
    }

    info.GetReturnValue().Set(resourceOf(isolate).core().detachEntity(*entity));
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
    blip.priority = fields->byte("priority", 0);
    blip.name = fields->text("name");

    // Признаки одним числом: слой их и собирает, и разбирает — здесь они уже
    // сложены. Разбирать их дважды, тут и там, значило бы завести второе место,
    // где номера битов могут разойтись с первым.
    blip.flags = static_cast<std::uint16_t>(fields->number("flags", 0));
    blip.flashInterval = static_cast<std::uint16_t>(fields->number("flashInterval", 0));
    blip.flashTimer = static_cast<std::uint16_t>(fields->number("flashTimer", 0));
    blip.number = fields->byte("number", 0);

    blip.hasSecondaryColour = fields->flag("hasSecondaryColour", false);
    blip.secondaryRed = fields->byte("secondaryRed", 0);
    blip.secondaryGreen = fields->byte("secondaryGreen", 0);
    blip.secondaryBlue = fields->byte("secondaryBlue", 0);

    blip.gxtName = fields->text("gxtName");

    // Общая ли метка. Умолчание — «да»: у alt:V довод `global` необязателен, и
    // не назвавший его режим ждёт обычную метку, видимую всем.
    blip.global = fields->flag("global", true);

    // Кому метка видна, если она не общая, — списком номеров, а не сущностей:
    // слой alt:V держит игроков объектами, а сюда доезжают их номера.
    for (const double target : fields->numbers("targets")) {
        blip.targets.push_back(static_cast<shared::PlayerId>(target));
    }

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

// --- Ресурсы ----------------------------------------------------------------

/// Кто ещё поднят: список объектов с именем и корнем.
///
/// Только это и отдаётся, и не по бедности. Всё прочее, что alt:V показывает у
/// чужого ресурса, живёт в его изоляте — а изолят у каждого свой, и заглянуть в
/// соседний нельзя ни отсюда, ни откуда бы то ни было. Разделение это не наша
/// прихоть, а условие: уронивший свою кучу не должен уносить чужие.
void listResources(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const auto everyone = resourceOf(isolate).roster();
    const v8::Local<v8::Array> list = v8::Array::New(isolate, static_cast<int>(everyone.size()));

    for (std::size_t index = 0; index < everyone.size(); ++index) {
        const v8::Local<v8::Object> each = v8::Object::New(isolate);

        (void)each->Set(context, toJs(isolate, "name"), toJs(isolate, everyone[index].first));
        (void)each->Set(context, toJs(isolate, "path"), toJs(isolate, everyone[index].second));

        (void)list->Set(context, static_cast<std::uint32_t>(index), each);
    }

    info.GetReturnValue().Set(list);
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

/// Устраивает взрыв.
///
/// Доводы названы так же, как у натива игры `ADD_EXPLOSION`, и в том же
/// порядке: тот, кто знает натив, напишет вызов верно с первого раза, а иного
/// образца взять неоткуда — серверного взрыва нет ни у alt:V, ни у RAGE MP.
///
/// Всё, кроме точки и рода, необязательно: чаще всего взрыв заводят двумя
/// доводами.
void addExplosion(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<shared::Vec3> where =
        info.Length() >= 1 ? vec3FromJs(context, info[0]) : std::nullopt;

    if (!where) {
        fail(isolate, "addExplosion ждёт точку и род взрыва");
        return;
    }

    script::ExplosionInfo explosion;
    explosion.position = *where;

    if (info.Length() >= 2) {
        if (const std::optional<std::int64_t> kind = intFromJs(context, info[1])) {
            explosion.kind = static_cast<std::int32_t>(*kind);
        }
    }

    // Дальше — необязательное, объектом. Объектом, а не шестью доводами подряд,
    // потому что подряд их никто не помнит: у натива их восемь, и путают в нём
    // как раз последние.
    if (info.Length() >= 3) {
        if (const std::optional<Fields> fields = fieldsOf(context, info[2])) {
            explosion.scale = static_cast<float>(fields->number("scale", 1.0));
            explosion.shake = static_cast<float>(fields->number("shake", 0.0));
            explosion.dimension = static_cast<std::int32_t>(
                fields->number("dimension", static_cast<double>(script::kDefaultDimension)));
            explosion.audible = fields->flagOr("audible", true);
            explosion.invisible = fields->flag("invisible");
        }
    }

    resourceOf(isolate).core().explode(explosion);
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

/// Чем сервер объявил себя при запуске.
///
/// Отдаётся обычным объектом, а не заготовкой класса: у alt:V это тоже простой
/// `IServerConfig`, и режимы кладут его целиком в свои настройки.
///
/// Пароля здесь нет и не будет — только признак того, что он есть. Режим,
/// положивший его в свой журнал или отправивший в своё окно, раздал бы его
/// игрокам, а спрашивать пароль у сервера ему незачем: проверяет его сервер.
/// Что этому игроку сейчас раздаётся.
///
/// Списком пар «род, номер», а не готовыми сущностями: оборачивать их здесь
/// значило бы заводить объект на каждую, а раздаётся их сотни. Обёртки и
/// расстояние доделывает слой — там же, где живут сами классы.
void playerStreamed(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<shared::PlayerId> id = selfPlayer(info);

    if (!id) {
        return;
    }

    const auto found = resourceOf(isolate).core().streamedTo(*id);
    const v8::Local<v8::Array> list = v8::Array::New(isolate, static_cast<int>(found.size()) * 2);

    for (std::size_t at = 0; at < found.size(); ++at) {
        (void)list->Set(context, static_cast<std::uint32_t>(at * 2),
                        v8::Integer::New(isolate, static_cast<std::int32_t>(found[at].first)));
        (void)list->Set(context, static_cast<std::uint32_t>(at * 2 + 1),
                        v8::Integer::New(isolate, static_cast<std::int32_t>(found[at].second)));
    }

    info.GetReturnValue().Set(list);
}

void serverConfig(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const script::ServerConfigInfo settings = resourceOf(isolate).core().config();
    const v8::Local<v8::Object> out = v8::Object::New(isolate);

    (void)out->Set(context, toJs(isolate, std::string{"name"}), toJs(isolate, settings.name));

    putNumber(context, out, "port", settings.port);
    putNumber(context, out, "players", static_cast<double>(settings.maxPlayers));
    putNumber(context, out, "tickRate", settings.tickRate);
    putNumber(context, out, "streamingDistance", settings.streamDistance);

    (void)out->Set(context, toJs(isolate, std::string{"passworded"}),
                   v8::Boolean::New(isolate, settings.passworded));
    (void)out->Set(context, toJs(isolate, std::string{"debug"}),
                   v8::Boolean::New(isolate, settings.verbose));

    const v8::Local<v8::Array> named =
        v8::Array::New(isolate, static_cast<int>(settings.resources.size()));

    for (std::size_t at = 0; at < settings.resources.size(); ++at) {
        (void)named->Set(context, static_cast<std::uint32_t>(at),
                         toJs(isolate, settings.resources[at]));
    }

    (void)out->Set(context, toJs(isolate, std::string{"resources"}), named);

    info.GetReturnValue().Set(out);
}

/// Просьба поднять, остановить или перезапустить ресурс.
///
/// Шаблоном на все три, потому что отличаются они одним числом: разбор довода,
/// отказ и ответ у них общие, а три почти одинаковых тела — это три места, где
/// легко разойтись.
template<script::ResourceAction Action>
void askResource(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();

    if (info.Length() < 1) {
        fail(isolate, "распоряжение о ресурсе ждёт его имя");
        return;
    }

    info.GetReturnValue().Set(
        resourceOf(isolate).core().askResource(fromJs(isolate, info[0]), Action));
}

/// Кости модели, разложенные в список объектов alt:V.
///
/// Имена полей — его же: `id`, `index`, `name`. Всякий раз заново, а не
/// запомненным списком: обращений к костям единицы, а живущий рядом с изолятом
/// список пришлось бы отпускать вместе с ним.
[[nodiscard]] v8::Local<v8::Array> bonesToJs(v8::Local<v8::Context> context,
                                             const std::vector<script::BoneInfo>& bones) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::Array> out = v8::Array::New(isolate, static_cast<int>(bones.size()));

    for (std::size_t at = 0; at < bones.size(); ++at) {
        const v8::Local<v8::Object> one = v8::Object::New(isolate);

        putNumber(context, one, "id", bones[at].id);
        putNumber(context, one, "index", bones[at].index);
        (void)one->Set(context, toJs(isolate, std::string{"name"}),
                       toJs(isolate, bones[at].name));

        (void)out->Set(context, static_cast<std::uint32_t>(at), one);
    }

    return out;
}

/// Общее для трёх вопросов о моделях: разбор довода и отказ.
///
/// Отказ здесь громкий и разный по поводу, и различать поводы обязательно.
/// «Справочника нет» означает «положите его рядом с сервером», а «модели нет» —
/// «такой модели не бывает». Ответь мы на оба одинаково — хозяин сервера искал
/// бы опечатку в имени модели там, где не хватает файла.
template<typename Info, const Info* (script::Core::*Ask)(std::uint32_t) const,
         typename Shape>
void modelInfo(const v8::FunctionCallbackInfo<v8::Value>& info, Shape shape) {
    v8::Isolate* const isolate = info.GetIsolate();
    script::Core& core = resourceOf(isolate).core();

    if (!core.knowsModels()) {
        fail(isolate, "справочника моделей у сервера нет: положите рядом gamedata.bin, "
                      "он собирается tools/gamedata");
        return;
    }

    // Хеш числом или имя строкой — так же, как у `player.model` и у татуировок:
    // режимы сплошь пишут `getVehicleModelInfoByHash(alt.hash('sultan'))`, но и
    // саму строку туда передают не реже.
    const std::optional<std::uint32_t> hash = hashAt(info, 0);

    if (!hash) {
        fail(isolate, "вопрос о модели ждёт её хеш или имя");
        return;
    }

    const Info* const known = (core.*Ask)(*hash);

    if (known == nullptr) {
        // Ноль, а не отказ: у alt:V «нет такой модели» — законный ответ, и
        // режимы проверяют его через `if (!info)`. Бросать здесь значило бы
        // требовать try/catch вокруг всякой проверки имени модели.
        info.GetReturnValue().SetNull();
        return;
    }

    info.GetReturnValue().Set(shape(isolate->GetCurrentContext(), *known));
}

[[nodiscard]] v8::Local<v8::Object> vehicleModelToJs(v8::Local<v8::Context> context,
                                                     const script::VehicleModelInfo& one) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::Object> out = v8::Object::New(isolate);

    putNumber(context, out, "modelHash", one.modelHash);
    (void)out->Set(context, toJs(isolate, std::string{"title"}), toJs(isolate, one.title));
    putNumber(context, out, "type", one.type);
    putNumber(context, out, "wheelsCount", one.wheelsCount);
    putFlag(context, out, "hasArmoredWindows", one.hasArmouredWindows);
    putNumber(context, out, "primaryColor", one.primaryColour);
    putNumber(context, out, "secondaryColor", one.secondaryColour);
    putNumber(context, out, "pearlColor", one.pearlColour);
    putNumber(context, out, "wheelsColor", one.wheelColour);
    putNumber(context, out, "interiorColor", one.interiorColour);
    putNumber(context, out, "dashboardColor", one.dashboardColour);
    putFlag(context, out, "hasAutoAttachTrailer", one.hasAutoAttachTrailer);
    putFlag(context, out, "canAttachCars", one.canAttachCars);
    putNumber(context, out, "handlingNameHash", one.handlingNameHash);
    (void)out->Set(context, toJs(isolate, std::string{"dlcName"}), toJs(isolate, one.dlc));

    // Наборы тюнинга и дополнения кузова отдаются числами, а слой alt:V делает
    // из них то, что объявлено: `availableModkits` списком признаков и
    // `hasExtra` вопросом. Разбор битов живёт там, а не здесь: здесь он был бы
    // вторым разбором того же, и второй разошёлся бы с первым.
    putNumber(context, out, "modKit", one.modKit);
    putNumber(context, out, "secondModKit", one.secondModKit);
    putNumber(context, out, "extras", one.extras);
    putNumber(context, out, "defaultExtras", one.defaultExtras);

    (void)out->Set(context, toJs(isolate, std::string{"bones"}), bonesToJs(context, one.bones));
    return out;
}

[[nodiscard]] v8::Local<v8::Object> pedModelToJs(v8::Local<v8::Context> context,
                                                 const script::PedModelInfo& one) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::Object> out = v8::Object::New(isolate);

    putNumber(context, out, "hash", one.hash);
    (void)out->Set(context, toJs(isolate, std::string{"name"}), toJs(isolate, one.name));
    (void)out->Set(context, toJs(isolate, std::string{"type"}), toJs(isolate, one.type));
    (void)out->Set(context, toJs(isolate, std::string{"dlcName"}), toJs(isolate, one.dlc));
    (void)out->Set(context, toJs(isolate, std::string{"defaultUnarmedWeapon"}),
                   toJs(isolate, one.defaultUnarmedWeapon));
    (void)out->Set(context, toJs(isolate, std::string{"movementClipSet"}),
                   toJs(isolate, one.movementClipSet));

    (void)out->Set(context, toJs(isolate, std::string{"bones"}), bonesToJs(context, one.bones));
    return out;
}

[[nodiscard]] v8::Local<v8::Object> weaponModelToJs(v8::Local<v8::Context> context,
                                                    const script::WeaponModelInfo& one) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::Object> out = v8::Object::New(isolate);

    putNumber(context, out, "hash", one.hash);
    (void)out->Set(context, toJs(isolate, std::string{"name"}), toJs(isolate, one.name));
    (void)out->Set(context, toJs(isolate, std::string{"modelName"}),
                   toJs(isolate, one.modelName));
    putNumber(context, out, "modelHash", one.modelHash);
    putNumber(context, out, "ammoTypeHash", one.ammoTypeHash);
    (void)out->Set(context, toJs(isolate, std::string{"ammoType"}), toJs(isolate, one.ammoType));
    (void)out->Set(context, toJs(isolate, std::string{"ammoModelName"}),
                   toJs(isolate, one.ammoModelName));
    putNumber(context, out, "ammoModelHash", one.ammoModelHash);
    putNumber(context, out, "defaultMaxAmmoMp", one.defaultMaxAmmo);
    putNumber(context, out, "skillAbove50MaxAmmoMp", one.skillAbove50MaxAmmo);
    putNumber(context, out, "maxSkillMaxAmmoMp", one.maxSkillMaxAmmo);
    putNumber(context, out, "bonusMaxAmmoMp", one.bonusMaxAmmo);
    (void)out->Set(context, toJs(isolate, std::string{"damageType"}),
                   toJs(isolate, one.damageType));

    return out;
}

void vehicleModelInfo(const v8::FunctionCallbackInfo<v8::Value>& info) {
    modelInfo<script::VehicleModelInfo, &script::Core::vehicleModel>(info, vehicleModelToJs);
}

void pedModelInfo(const v8::FunctionCallbackInfo<v8::Value>& info) {
    modelInfo<script::PedModelInfo, &script::Core::pedModel>(info, pedModelToJs);
}

void weaponModelInfo(const v8::FunctionCallbackInfo<v8::Value>& info) {
    modelInfo<script::WeaponModelInfo, &script::Core::weaponModel>(info, weaponModelToJs);
}

/// Сколько деталей есть у машины в этом месте тюнинга.
///
/// Спрашивается по номеру машины в сессии, а не по модели: так это объявлено у
/// alt:V (`vehicle.getModsCount`), и оттуда же берётся её модель.
void vehicleModsCount(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* const isolate = info.GetIsolate();
    const v8::Local<v8::Context> context = isolate->GetCurrentContext();

    const std::optional<shared::VehicleId> id = idOf<shared::VehicleId>(info.This());

    if (!id || info.Length() < 1) {
        fail(isolate, "getModsCount ждёт место тюнинга");
        return;
    }

    const std::optional<std::int64_t> slot = intFromJs(context, info[0]);

    if (!slot) {
        fail(isolate, "getModsCount ждёт место тюнинга числом");
        return;
    }

    const std::int32_t known =
        resourceOf(isolate).core().vehicleModsCount(*id, static_cast<std::uint8_t>(*slot));

    if (known < 0) {
        // Отказ вслух, а не ноль: ноль означает «в этом месте деталей нет», то
        // есть пустое меню тюнинга, и отдать его вместо «не знаю» значило бы
        // сказать про всякую машину, что тюнинговать её нечем.
        fail(isolate, "справочника моделей у сервера нет либо машины уже нет в сессии: "
                      "gamedata.bin собирается tools/gamedata");
        return;
    }

    info.GetReturnValue().Set(known);
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
    addGetter(isolate, shape, "ip", playerField<&PlayerInfo::ip>);
    addGetter(isolate, shape, "ping", playerField<&PlayerInfo::ping>);
    addGetter(isolate, shape, "position", playerField<&PlayerInfo::position>);
    addGetter(isolate, shape, "heading", playerField<&PlayerInfo::heading>);
    addGetter(isolate, shape, "health", playerField<&PlayerInfo::health>, setPlayerHealth);
    addGetter(isolate, shape, "armour", playerField<&PlayerInfo::armour>, setPlayerArmour);
    addGetter(isolate, shape, "maxArmour", playerField<&PlayerInfo::maxArmour>, setPlayerMaxArmour);
    addGetter(isolate, shape, "model", playerField<&PlayerInfo::model>, setPlayerModel);
    addGetter(isolate, shape, "seat", playerSeat);
    addGetter(isolate, shape, "velocity", playerField<&PlayerInfo::velocity>);
    addGetter(isolate, shape, "moveSpeed", playerSpeed<SpeedKind::Total>);
    addGetter(isolate, shape, "forwardSpeed", playerSpeed<SpeedKind::Forward>);
    addGetter(isolate, shape, "strafeSpeed", playerSpeed<SpeedKind::Strafe>);
    addGetter(isolate, shape, "admin", playerField<&PlayerInfo::admin>);

    // Кем игрок назвался при входе. Строками, как у alt:V, и по той же
    // причине: числа в JS теряют младшие разряды шестидесятичетырёхразрядного.
    //
    // Проверено это ничем и проверено быть не может: считает отпечаток чужая
    // машина. Режимы держат на нём свои запреты — так же, как на alt:V, — но
    // доказательством он не является ни там, ни здесь.
    addGetter(isolate, shape, "hwidHash", playerBigNumber<&PlayerInfo::hwidHash>);
    addGetter(isolate, shape, "socialID", playerBigNumber<&PlayerInfo::socialId>);
    addGetter(isolate, shape, "socialClubName", playerField<&PlayerInfo::socialName>);

    // Что у него в руках и куда он целится. Имена — alt:V, поля — те же, что
    // ехали в снимке с самого начала.
    addGetter(isolate, shape, "currentWeapon", playerField<&PlayerInfo::weapon>);
    addGetter(isolate, shape, "aimPos", playerField<&PlayerInfo::aimAt>);

    // Признаки состояния. Имена и их набор взяты у alt:V дословно, порядок —
    // его же; наши имена признаков (Melee, ExitingVehicle, Ragdoll) с ними не
    // совпадают, и перевод живёт ровно здесь.
    addGetter(isolate, shape, "isDead", playerFlag<shared::PlayerFlag::Dead>);
    addGetter(isolate, shape, "isAiming", playerFlag<shared::PlayerFlag::Aiming>);
    addGetter(isolate, shape, "isShooting", playerFlag<shared::PlayerFlag::Shooting>);
    addGetter(isolate, shape, "isInRagdoll", playerFlag<shared::PlayerFlag::Ragdoll>);
    addGetter(isolate, shape, "isJumping", playerFlag<shared::PlayerFlag::Jumping>);
    addGetter(isolate, shape, "isCrouching", playerFlag<shared::PlayerFlag::Crouching>);
    addGetter(isolate, shape, "isOnVehicle", playerFlag<shared::PlayerFlag::OnVehicle>);

    // Крадущийся и пригнувшийся у игры — одно и то же: своего приседания у
    // игрока в GTA V нет, а `GET_PED_STEALTH_MOVEMENT` отвечает и на то, и на
    // другое. У alt:V это два свойства, и оба отвечают одним признаком.
    addGetter(isolate, shape, "isStealthy", playerFlag<shared::PlayerFlag::Crouching>);
    addGetter(isolate, shape, "isParachuting", playerFlag<shared::PlayerFlag::Parachuting>);
    addGetter(isolate, shape, "isReloading", playerFlag<shared::PlayerFlag::Reloading>);
    addGetter(isolate, shape, "isInCover", playerFlag<shared::PlayerFlag::InCover>);
    addGetter(isolate, shape, "isInMelee", playerFlag<shared::PlayerFlag::Melee>);
    addGetter(isolate, shape, "isEnteringVehicle",
              playerFlag<shared::PlayerFlag::EnteringVehicle>);
    addGetter(isolate, shape, "isLeavingVehicle", playerFlag<shared::PlayerFlag::LeavingVehicle>);
    addGetter(isolate, shape, "isInWater", playerInWater);
    addGetter(isolate, shape, "isSpawned", playerSpawned);

    // Распоряжения о теле. Читаются и ставятся: помнит их сервер, накладывает
    // игра хозяина, и накладывает каждый кадр.
    addGetter(isolate, shape, "frozen", playerSwitch<shared::PlayerControlFlag::Frozen>,
              playerSetSwitch<&Core::setFrozen>);
    addGetter(isolate, shape, "invincible",
              playerSwitch<shared::PlayerControlFlag::Invincible>,
              playerSetSwitch<&Core::setInvincible>);
    addGetter(isolate, shape, "dimension", playerField<&PlayerInfo::dimension>,
              setPlayerDimension);
    addGetter(isolate, shape, "vehicle", playerVehicle);
    addGetter(isolate, shape, "valid", playerValid);

    addMethod(isolate, shape, "teleport", playerTeleport);
    addMethod(isolate, shape, "giveWeapon", playerGiveWeapon);
    addMethod(isolate, shape, "clearWeapons", playerClearWeapons);

    // Снаряжение: спросить и отобрать. Имена — alt:V; `removeAllWeapons` у него
    // делает то же, что у ядра `clearWeapons`, и потому идёт вторым именем, а не
    // вторым действием.
    addGetter(isolate, shape, "weapons", playerWeapons);
    addMethod(isolate, shape, "hasWeapon", playerHasWeapon);
    // Имя со звёздочкой на конце нарочно: свойство `streamedEntities`
    // объявляет слой, и он же зовёт это. Одно имя на оба означало бы, что
    // слой перекрывает ядро и зовёт сам себя.
    addMethod(isolate, shape, "streamedEntitiesRaw", playerStreamed);
    addMethod(isolate, shape, "getWeaponAmmo", playerWeaponAmmo);
    addMethod(isolate, shape, "removeWeapon", playerRemoveWeapon);
    addMethod(isolate, shape, "removeAllWeapons", playerClearWeapons);
    addMethod(isolate, shape, "playScenario", playerPlayScenario);
    addMethod(isolate, shape, "setWeaponAmmo", playerSetWeaponAmmo);
    addMethod(isolate, shape, "hasWeaponComponent", playerHasWeaponComponent);
    addGetter(isolate, shape, "currentWeaponComponents", playerHeldWeaponDetail<true>);
    addGetter(isolate, shape, "currentWeaponTintIndex", playerHeldWeaponDetail<false>);
    addMethod(isolate, shape, "addWeaponComponent", playerAddWeaponComponent);
    addMethod(isolate, shape, "removeWeaponComponent", playerRemoveWeaponComponent);
    addMethod(isolate, shape, "setWeaponTintIndex", playerSetWeaponTint);
    addMethod(isolate, shape, "setClothes", playerSetClothes);
    addMethod(isolate, shape, "setProp", playerSetProp);

    // Внешность лица. Имена — alt:V, вплоть до американского написания цвета:
    // ресурсы написаны под `setHeadOverlayColor`, и своё написание здесь
    // означало бы, что метод есть, а зовут его мимо.
    addMethod(isolate, shape, "getClothes", playerGetClothes);
    addMethod(isolate, shape, "getProp", playerGetProp);
    addMethod(isolate, shape, "getHeadBlendData", playerGetHeadBlend);
    addMethod(isolate, shape, "getHeadOverlay", playerGetHeadOverlay);
    addMethod(isolate, shape, "getHairColor",
              playerLookField<&shared::PlayerAppearance::hairColour>);
    addMethod(isolate, shape, "getHairHighlightColor",
              playerLookField<&shared::PlayerAppearance::hairHighlight>);
    addMethod(isolate, shape, "getEyeColor",
              playerLookField<&shared::PlayerAppearance::eyeColour>);

    addMethod(isolate, shape, "setHeadBlendData", playerSetHeadBlend);
    addMethod(isolate, shape, "setHeadOverlay", playerSetHeadOverlay);
    addMethod(isolate, shape, "setHeadOverlayColor", playerSetHeadOverlayColour);
    addMethod(isolate, shape, "setHairColor", playerSetHairColour);
    addMethod(isolate, shape, "setEyeColor", playerSetEyeColour);
    addMethod(isolate, shape, "setFaceFeature", playerSetFaceFeature);
    addMethod(isolate, shape, "addDecoration", playerAddDecoration);
    addMethod(isolate, shape, "removeDecoration", playerRemoveDecoration);
    addMethod(isolate, shape, "clearDecorations", playerClearDecorations);
    addMethod(isolate, shape, "getDecorations", playerDecorations);
    addMethod(isolate, shape, "getFaceFeatureScale", playerFaceFeature);
    addMethod(isolate, shape, "clearProp", playerClearProp);
    addMethod(isolate, shape, "clearClothes", playerClearClothes);
    addMethod(isolate, shape, "setHairHighlightColor", playerSetHairHighlight);
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
    addMethod(isolate, shape, "place", objectPlace);

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

    // Прочность, скорость, признаки и сидящие. Имена — alt:V; всё это ехало в
    // снимке машины с самого начала и не было видно скрипту.
    //
    // Прочность, двигатель и сирена у alt:V назначаются, а у нас — нет: машина
    // живёт в игре у ведущего, и «поставь ровно столько» протоколом не
    // переносится. Поэтому у них стоит отказ, а не пустое место: без него
    // присваивание из строгого модуля бросило бы TypeError посреди чужого
    // обработчика.
    addGetter(isolate, shape, "velocity", vehicleField<&VehicleInfo::velocity>);

    // Руль — тот же ввод водителя, что едет в снимке, и в тех же долях, что у
    // alt:V. Углом в градусах он не становится нигде, кроме самой игры.
    addGetter(isolate, shape, "steeringAngle", vehicleField<&VehicleInfo::steer>);
    addGetter(isolate, shape, "bodyHealth", vehicleField<&VehicleInfo::bodyHealth>,
              refuseAssignment<kBodyHealthWhat, kLeaderOwnsIt>);
    addGetter(isolate, shape, "engineHealth", vehicleField<&VehicleInfo::engineHealth>,
              refuseAssignment<kEngineHealthWhat, kLeaderOwnsIt>);
    addGetter(isolate, shape, "petrolTankHealth", vehicleField<&VehicleInfo::tankHealth>,
              refuseAssignment<kTankHealthWhat, kLeaderOwnsIt>);
    addGetter(isolate, shape, "engineOn", vehicleFlag<shared::VehicleFlag::EngineOn>,
              refuseAssignment<kEngineOnWhat, kLeaderOwnsIt>);
    addGetter(isolate, shape, "handbrakeActive", vehicleFlag<shared::VehicleFlag::Handbrake>);
    // `lightState` сюда не попал нарочно: у alt:V это число, а не признак, и
    // отдать под его именем логическое значило бы соврать в другую сторону —
    // ресурс, сравнивший его с числом, не сошёлся бы ни разу.
    addGetter(isolate, shape, "daylightOn", vehicleFlag<shared::VehicleFlag::LightsOn>);
    addGetter(isolate, shape, "nightlightOn", vehicleFlag<shared::VehicleFlag::HighBeams>);
    addGetter(isolate, shape, "sirenActive", vehicleFlag<shared::VehicleFlag::SirenOn>,
              refuseAssignment<kSirenWhat, kLeaderOwnsIt>);
    addGetter(isolate, shape, "hornActive", vehicleFlag<shared::VehicleFlag::HornOn>);
    addGetter(isolate, shape, "destroyed", vehicleFlag<shared::VehicleFlag::Destroyed>);
    addGetter(isolate, shape, "passengers", vehiclePassengers);
    addGetter(isolate, shape, "lockState", vehicleField<&VehicleInfo::lockState>,
              setVehicleLock);
    addMethod(isolate, shape, "isWindowOpened", vehicleWindowOpened);
    addMethod(isolate, shape, "setWindowOpened", vehicleSetWindowOpened);

    // Крыша — только на чтение: четыре её положения приходят из снимка
    // ведущего. Своего распоряжения о ней нет, и выдумывать его не стоит —
    // накладывает крышу тот же снимок, и второе наложение спорило бы с ним.
    addGetter(isolate, shape, "roofState", vehicleField<&VehicleInfo::roofState>);

    addMethod(isolate, shape, "getDoorState", vehicleDoorState);
    addMethod(isolate, shape, "setDoorState", vehicleSetDoorState);

    addGetter(isolate, shape, "appearance", vehicleAppearance);

    addMethod(isolate, shape, "destroy", vehicleDestroy);
    addMethod(isolate, shape, "teleport", vehicleTeleport);
    addMethod(isolate, shape, "repair", vehicleRepair);
    addMethod(isolate, shape, "setNetOwner", vehicleSetNetOwner);
    addMethod(isolate, shape, "resetNetOwner", vehicleResetNetOwner);
    addMethod(isolate, shape, "getModsCount", vehicleModsCount);
    addMethod(isolate, shape, "setAppearance", vehicleSetAppearance);

    return shape;
}

[[nodiscard]] v8::Local<v8::FunctionTemplate> buildPedShape(v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();
    const v8::Local<v8::FunctionTemplate> shape = entityTemplate(isolate, "Ped");

    addGetter(isolate, shape, "id", pedField<&PedInfo::id>);
    addGetter(isolate, shape, "model", pedField<&PedInfo::model>);
    addGetter(isolate, shape, "position", pedField<&PedInfo::position>);
    addGetter(isolate, shape, "rotation", pedField<&PedInfo::rotation>);
    addGetter(isolate, shape, "health", pedField<&PedInfo::health>,
              setPedField<&PedInfo::health>);
    addGetter(isolate, shape, "maxHealth", pedField<&PedInfo::maxHealth>,
              setPedField<&PedInfo::maxHealth>);
    addGetter(isolate, shape, "armour", pedField<&PedInfo::armour>,
              setPedField<&PedInfo::armour>);
    addGetter(isolate, shape, "weapon", pedField<&PedInfo::weapon>,
              setPedField<&PedInfo::weapon>);
    addGetter(isolate, shape, "dimension", pedField<&PedInfo::dimension>,
              setPedDimensionValue);
    addGetter(isolate, shape, "valid", pedValid);

    addMethod(isolate, shape, "destroy", pedDestroy);
    addMethod(isolate, shape, "update", pedUpdate);

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

v8::Local<v8::Value> wrapPed(Resource& resource, v8::Local<v8::Context> context,
                             shared::PedId id) {
    return instantiate(context, resource.pedShape(), static_cast<std::uint32_t>(id));
}

void installBindings(Resource& resource, v8::Local<v8::Context> context) {
    v8::Isolate* const isolate = context->GetIsolate();

    const v8::Local<v8::FunctionTemplate> playerShape = buildPlayerShape(context);
    const v8::Local<v8::FunctionTemplate> vehicleShape = buildVehicleShape(context);
    const v8::Local<v8::FunctionTemplate> objectShape = buildObjectShape(context);
    const v8::Local<v8::FunctionTemplate> pedShape = buildPedShape(context);

    resource.setPlayerShape(playerShape);
    resource.setVehicleShape(vehicleShape);
    resource.setObjectShape(objectShape);
    resource.setPedShape(pedShape);

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
    addFunction(context, oxymp, "addExplosion", addExplosion);
    addFunction(context, oxymp, "playAnimation", playAnimation);
    addFunction(context, oxymp, "playSpeech", playSpeech);
    addFunction(context, oxymp, "clearTasks", clearTasks);
    addFunction(context, oxymp, "peds", peds);
    addFunction(context, oxymp, "createPed", createPed);
    addFunction(context, oxymp, "attachEntity", attachEntity);
    addFunction(context, oxymp, "detachEntity", detachEntity);
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
    addFunction(context, oxymp, "serverConfig", serverConfig);
    addFunction(context, oxymp, "vehicleModelInfo", vehicleModelInfo);
    addFunction(context, oxymp, "pedModelInfo", pedModelInfo);
    addFunction(context, oxymp, "weaponModelInfo", weaponModelInfo);
    addFunction(context, oxymp, "startResource", askResource<script::ResourceAction::Start>);
    addFunction(context, oxymp, "stopResource", askResource<script::ResourceAction::Stop>);
    addFunction(context, oxymp, "restartResource",
                askResource<script::ResourceAction::Restart>);

    // Классы кладутся туда же: скрипту они нужны не для того, чтобы заводить
    // сущности, а для проверок вида `x instanceof oxymp.Player`.
    (void)oxymp->Set(context, toJs(isolate, "Player"),
                     playerShape->GetFunction(context).ToLocalChecked());
    (void)oxymp->Set(context, toJs(isolate, "Vehicle"),
                     vehicleShape->GetFunction(context).ToLocalChecked());
    (void)oxymp->Set(context, toJs(isolate, "Object"),
                     objectShape->GetFunction(context).ToLocalChecked());
    (void)oxymp->Set(context, toJs(isolate, "Ped"),
                     pedShape->GetFunction(context).ToLocalChecked());

    // Имя ресурса — чтобы он мог собрать путь к своим файлам и назвать себя в
    // событии клиенту.
    (void)oxymp->Set(context, toJs(isolate, "resourceName"), toJs(isolate, resource.name()));

    addFunction(context, oxymp, "resources", listResources);

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
