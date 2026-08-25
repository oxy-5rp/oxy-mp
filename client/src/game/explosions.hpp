#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

namespace oxymp::client::game {

/// Взрывы, которые устраивает сервер.
///
/// Устроены проще всего, что здесь есть, и это следует из того, чем взрыв
/// является. У метки, маркера и прохожего есть номер, потому что их правят и
/// убирают; у взрыва номера нет — он случается и кончается, и помнить о нём
/// нечего. Отсюда весь класс: один натив и один вызов.
///
/// Заводит взрыв сервер, а не тот, у кого рвануло, и это то же правило, что и у
/// погоды. Взрыв — это урон, звук и толчок всему вокруг; посчитанный каждым у
/// себя, он разошёлся бы у двоих зрителей и убил бы одного и того же человека
/// дважды или ни разу.
///
/// Вызывать можно только изнутри скриптового тика.
class Explosions {
public:
    explicit Explosions(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Устраивает взрыв там, где велел сервер.
    void play(const shared::Explosion& explosion) const;

private:
    NativeHandler addExplosion_ = nullptr;
};

} // namespace oxymp::client::game
