#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

namespace oxymp::client::game {

/// Как персонаж одет и какое у него лицо.
///
/// Отсюда внешность своего игрока уходит на сервер, а чужая приходит и
/// надевается на куклу. Без этого все чужие игроки — один и тот же лысый
/// `mp_m_freemode_01`, в чём бы ни ходили их хозяева.
///
/// Читается не всё, что передаётся, и это не недоделка, а свойство игры. Одежду
/// и аксессуары она отдаёт обратно; **цвет волос, цвет глаз и слои лица —
/// нет**: у них есть только запись. Ровно так же обстоит дело и у alt:V, и
/// вывод из этого один — эти три величины ведёт тот, кто их задал, то есть
/// сервер, а не тот, кто на них смотрит. В протоколе они есть, и надеваются они
/// исправно; читаются же нулями.
///
/// Вызывать можно только изнутри скриптового тика.
class PedAppearance {
public:
    explicit PedAppearance(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Считывает, как выглядит персонаж.
    [[nodiscard]] shared::PlayerAppearance read(int ped) const;

    /// Одевает персонажа так, как сказано.
    ///
    /// Модель здесь не меняется: смена модели — это замена самого персонажа со
    /// всей его загрузкой, и живёт она отдельно (см. remote_players.cpp).
    /// Надевается всё остальное.
    void apply(int ped, const shared::PlayerAppearance& appearance) const;

private:
    NativeHandler getDrawable_ = nullptr;
    NativeHandler getTexture_ = nullptr;
    NativeHandler getPalette_ = nullptr;
    NativeHandler setComponent_ = nullptr;

    NativeHandler getProp_ = nullptr;
    NativeHandler getPropTexture_ = nullptr;
    NativeHandler setProp_ = nullptr;
    NativeHandler clearProp_ = nullptr;

    NativeHandler setHeadBlend_ = nullptr;
    NativeHandler setOverlay_ = nullptr;
    NativeHandler setOverlayColour_ = nullptr;
    NativeHandler setHairColour_ = nullptr;
    NativeHandler setEyeColour_ = nullptr;

    NativeHandler entityModel_ = nullptr;
};

} // namespace oxymp::client::game
