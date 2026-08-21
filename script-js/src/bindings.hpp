#pragma once

#include <oxymp/shared/protocol/messages.hpp>

#include <v8.h>

namespace oxymp::script::js {

class Resource;

/// Ставит в контекст всё, чем пользуется скрипт: объект `oxymp` и классы
/// сущностей.
///
/// Глобальным объектом, а не модулем `require('oxymp')`, и это выбор в пользу
/// RAGE MP против alt:V. У alt:V ресурс начинается со строки импорта, и она же —
/// первое, обо что спотыкается новичок: забыл её, и `alt` не определён. Здесь
/// `oxymp` есть всегда, как `mp` в RAGE MP.
///
/// Обычный `require` при этом никуда не девается: ресурс волен раскладывать себя
/// по файлам и тянуть пакеты из node_modules — они ищутся рядом с ним.
void installBindings(Resource& resource, v8::Local<v8::Context> context);

/// Заворачивает игрока в объект JS.
///
/// Объект держит номер, а не запись: игрок волен выйти между двумя обращениями,
/// и указатель на него пережил бы его самого. Всякое обращение к свойству
/// разрешает номер заново, а к вышедшему — честно отвечает пустотой.
[[nodiscard]] v8::Local<v8::Value> wrapPlayer(Resource& resource,
                                               v8::Local<v8::Context> context,
                                               shared::PlayerId id);

/// То же для машины.
[[nodiscard]] v8::Local<v8::Value> wrapVehicle(Resource& resource,
                                                v8::Local<v8::Context> context,
                                                shared::VehicleId id);

/// То же для предмета.
[[nodiscard]] v8::Local<v8::Value> wrapObject(Resource& resource,
                                               v8::Local<v8::Context> context,
                                               shared::ObjectId id);

/// То же для прохожего.
[[nodiscard]] v8::Local<v8::Value> wrapPed(Resource& resource, v8::Local<v8::Context> context,
                                            shared::PedId id);

} // namespace oxymp::script::js
