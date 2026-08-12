#pragma once

#include "hud.hpp"
#include "native_table.hpp"
#include "remote_players.hpp"

#include <oxymp/shared/math/vec3.hpp>

#include <vector>

namespace oxymp::client::game {

/// Подписи над головами чужих игроков.
///
/// Имя и номер рисуются не поверх экрана, а в мире: игра умеет переносить начало
/// координат рисования в точку пространства, и всё, что нарисовано после,
/// ложится туда, где эта точка видна. Так подпись держится над головой сама, без
/// пересчёта мировых координат в экранные и без забот о том, что персонаж за
/// спиной.
///
/// Вызывать можно только изнутри скриптового тика.
class Nameplates {
public:
    Nameplates(const NativeTable& table, const Hud& hud) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Рисует подписи. Вызывать раз в кадр.
    ///
    /// viewer — где стоит местный игрок: дальние подписи не рисуются вовсе, а
    /// ближние тем мельче, чем дальше их хозяин.
    void draw(const std::vector<RemotePlayerView>& players, shared::Vec3 viewer) const;

private:
    const Hud& hud_;

    NativeHandler setOrigin_ = nullptr;
    NativeHandler clearOrigin_ = nullptr;
};

} // namespace oxymp::client::game
