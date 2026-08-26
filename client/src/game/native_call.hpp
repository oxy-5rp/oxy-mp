#pragma once

#include "native_context.hpp"
#include "native_table.hpp"

#include <type_traits>

namespace oxymp::client::game {

/// Вызывает натив по готовому обработчику.
///
/// Обработчик берётся у NativeTable и проверяется на пустоту вызывающим:
/// отсутствующий натив — это ошибка в хешах, и молча её проглатывать нельзя.
template <typename Result = void, typename... Args>
Result invokeNative(NativeHandler handler, Args... arguments) {
    NativeContext context;
    (context.push(arguments), ...);

    handler(context.address());

    if constexpr (!std::is_void_v<Result>) {
        return context.result<Result>();
    }
}

} // namespace oxymp::client::game
