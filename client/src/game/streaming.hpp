#pragma once

#include "native_table.hpp"

#include <oxymp/shared/math/vec3.hpp>

namespace oxymp::client::game {

/// Подгрузка мира вокруг игрока.
///
/// Заведено ради переносов. Игра подгружает мир по мере того, как игрок туда
/// едет, и это верно ровно до тех пор, пока он туда едет. Перенесённый мгновенно
/// оказывается там, где ещё ничего нет: земля не нарисована, столкновений нет, и
/// он проваливается сквозь неё.
///
/// Лечится это двумя вещами, и обе нужны.
///
/// Первая — просьба держать столкновения подгруженными вокруг персонажа. Она
/// повторяется каждый кадр и относится ко всему, что вокруг: игра иначе вправе
/// выгрузить то, чего игрок сейчас не касается.
///
/// Вторая — подгрузка сферой вокруг точки переноса. Она делается один раз, до
/// того как игрок там окажется, и о её готовности игра сообщает сама. Пока она
/// идёт, персонажа держат замороженным: падать сквозь незагруженную землю он
/// начнёт в первый же кадр, а не тогда, когда мы соберёмся.
///
/// Вызывать можно только изнутри скриптового тика.
class Streaming {
public:
    explicit Streaming(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Держит столкновения вокруг персонажа. Вызывать каждый кадр.
    void keepCollisionAround(int ped) const;

    /// Начинает подгрузку мира вокруг точки.
    void beginLoad(shared::Vec3 point);

    /// Идёт ли подгрузка прямо сейчас.
    [[nodiscard]] bool loading() const noexcept { return loading_; }

    /// Доводит подгрузку до конца. Вызывать каждый кадр, пока loading().
    ///
    /// Возвращает true, когда подгрузка закончилась — по готовности или по
    /// истечении срока. Срок обязателен: сфера, которую игре нечем заполнить,
    /// не догрузится никогда, а игрок всё это время стоит замороженным.
    bool advance();

private:
    NativeHandler requestCollision_ = nullptr;
    NativeHandler loadCollisionFlag_ = nullptr;
    NativeHandler startScene_ = nullptr;
    NativeHandler sceneLoaded_ = nullptr;
    NativeHandler stopScene_ = nullptr;
    NativeHandler gameTimer_ = nullptr;

    bool loading_ = false;

    /// Когда началась подгрузка, по игровому времени.
    std::int32_t startedAt_ = 0;
};

} // namespace oxymp::client::game
