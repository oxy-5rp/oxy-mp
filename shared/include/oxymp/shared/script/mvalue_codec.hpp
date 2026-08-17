#pragma once

#include <oxymp/shared/protocol/serialization.hpp>
#include <oxymp/shared/script/mvalue.hpp>

namespace oxymp::shared {

/// Укладывает значение в байты.
void writeMValue(ByteWriter& out, const MValue& value);

/// Разбирает значение из байтов.
///
/// Неудачу не бросает и не возвращает отдельно: она поднимается флагом у
/// читателя, и спросить о ней нужно один раз, после разбора всего сообщения, —
/// через `in.ok()`. Так же устроен и весь остальной разбор протокола, и заводить
/// здесь второй порядок обращения значило бы разделить разбор одного сообщения
/// на два разных стиля.
///
/// Что считается неудачей помимо нехватки байтов: неизвестный номер типа,
/// вложенность глубже MValue::kMaxDepth, список или словарь длиннее
/// MValue::kMaxListLength, двоичные данные длиннее MValue::kMaxByteArrayLength.
/// Все четыре — про присланное клиентом, а не про свою ошибку: события приходят
/// от того, кто мог собрать пакет как угодно.
[[nodiscard]] MValue readMValue(ByteReader& in);

/// Укладывает набор доводов события: сначала их число, затем сами значения.
void writeMValueArgs(ByteWriter& out, const MValueArgs& args);

/// Разбирает набор доводов события.
[[nodiscard]] MValueArgs readMValueArgs(ByteReader& in);

} // namespace oxymp::shared
