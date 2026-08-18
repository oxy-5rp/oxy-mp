#pragma once

#include <v8.h>

namespace oxymp::client::js {

class Resource;

/// Ставит в контекст всё, чем ресурс дотягивается до клиента.
///
/// Кладётся один объект — `globalThis.__oxympAlt.native`, — а привычный `alt`
/// собирается поверх него на JavaScript. Разделение то же, что и на сервере, и
/// по той же причине: вектор, складывающийся с вектором, не нуждается в C++, а
/// вызов натива не выразим на JavaScript вовсе. Каждому своё место.
void installBindings(Resource& resource, v8::Local<v8::Context> context);

} // namespace oxymp::client::js
