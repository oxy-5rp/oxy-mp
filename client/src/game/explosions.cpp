#include "explosions.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {

Explosions::Explosions(const NativeTable& table) noexcept
    : addExplosion_(table.handlerFor(natives::kAddExplosion)) {}

bool Explosions::ready() const noexcept {
    return addExplosion_ != nullptr;
}

void Explosions::play(const shared::Explosion& explosion) const {
    if (!ready()) {
        return;
    }

    // Род взрыва приходит по сети числом, а натив принимает его не глядя: игре,
    // которой велели устроить взрыв номер девять тысяч, деваться некуда — она
    // пойдёт за описанием по своей таблице и промахнётся мимо неё. Проверка
    // здесь, а не у отправителя, и намеренно: пакет мог испортиться по дороге, а
    // отправитель мог оказаться не тем сервером, за который себя выдаёт.
    if (explosion.kind < 0 || explosion.kind > shared::kMaxExplosionKind) {
        spdlog::debug("взрыв неизвестного рода {} отброшен", explosion.kind);
        return;
    }

    invokeNative<void>(addExplosion_, explosion.position.x, explosion.position.y,
                       explosion.position.z, explosion.kind, explosion.scale, explosion.audible,
                       explosion.invisible, explosion.shake);
}

} // namespace oxymp::client::game
