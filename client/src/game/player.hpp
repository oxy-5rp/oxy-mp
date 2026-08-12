#pragma once

#include "native_table.hpp"

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>

namespace oxymp::client::game {

/// Местный игрок: где он и куда его можно поставить.
///
/// Работает нативами, поэтому вызывать можно только изнутри скриптового тика.
class Player {
public:
    explicit Player(const NativeTable& table) noexcept;

    /// Все ли нативы нашлись.
    [[nodiscard]] bool ready() const noexcept;

    /// Номер местного игрока.
    ///
    /// Не то же, что персонаж: игрок переживает замену модели и смерть, а
    /// персонаж — нет. Нативы, распоряжающиеся розыском, управлением и
    /// внешностью, работают именно с игроком.
    [[nodiscard]] int id() const;

    /// Модель игрока. Ноль означает, что игрок ещё не создан: между загрузкой
    /// сессии и появлением персонажа проходит заметное время.
    [[nodiscard]] int ped() const;

    /// Играет ли игрок сейчас. Во время загрузки и кат-сцен — нет, и трогать
    /// его в этот момент бессмысленно: игра вернёт персонажа обратно.
    [[nodiscard]] bool playing() const;

    [[nodiscard]] shared::Vec3 coords(int ped) const;

    /// Куда персонаж повёрнут, в градусах.
    [[nodiscard]] float heading(int ped) const;

    /// Скорость персонажа в метрах в секунду, по осям мира.
    ///
    /// Уходит на сервер вместе с положением: по ней получатели заставляют его
    /// персонажа идти с той же скоростью, а не догадываются о ней по разнице
    /// между снимками.
    [[nodiscard]] shared::Vec3 velocity(int ped) const;

    /// Здоровье персонажа. Ноль означает смерть.
    [[nodiscard]] int health(int ped) const;

    /// Переносит игрока. Земля под точкой не проверяется: если её там нет,
    /// персонаж просто упадёт, а не застрянет в текстурах.
    void teleport(int ped, shared::Vec3 position) const;

    void setHeading(int ped, float degrees) const;

    /// Возвращает персонажу способность двигаться.
    ///
    /// Нужно потому, что достаётся он нам не чистым: сюжетная сцена, из которой
    /// мы его забираем, успевает и заморозить его на месте, и выдать ему свои
    /// задания. Само по себе это не проходит — замораживала игра намеренно, и
    /// отменять это тоже приходится намеренно.
    void release(int ped) const;

    /// Замораживает персонажа на месте и отпускает обратно.
    ///
    /// Нужно, пока вокруг него подгружается мир: под ногами ещё нет земли, и
    /// отпущенный он полетит сквозь неё.
    void freeze(int ped, bool frozen) const;

    /// Собирает снимок, который уходит на сервер.
    ///
    /// Одним вызовом, а не пятнадцатью по отдельности, и это не забота об
    /// удобстве: снимок обязан описывать одно и то же мгновение. Собранный по
    /// кусочкам из разных мест кадра, он рано или поздно скажет «стоит на месте»
    /// и «бежит» одновременно.
    ///
    /// Про машину здесь ничего нет: её описывает Vehicles, а сюда переносятся
    /// уже готовые поля.
    [[nodiscard]] shared::PlayerState snapshot(int player, int ped, bool dead) const;

    /// Наносит персонажу урон, о котором сообщил сервер.
    ///
    /// Урон считает тот, кто попал, а применяет тот, в кого попали: персонаж
    /// чужого игрока у стрелявшего — обычный болванчик, и происходящее с ним для
    /// хозяина не существует.
    void applyDamage(int ped, std::uint16_t amount) const;

private:
    /// Точка, куда направлено оружие.
    ///
    /// Оружие смотрит туда же, куда камера, поэтому точка берётся впереди
    /// персонажа по направлению взгляда. Настоящий луч прицела игра наружу не
    /// отдаёт, а для чужой стороны важно направление, а не попадание: попадания
    /// считает стрелявший у себя.
    [[nodiscard]] shared::Vec3 aimPoint(int ped) const;


    NativeHandler playerId_ = nullptr;
    NativeHandler playerPedId_ = nullptr;
    NativeHandler isPlaying_ = nullptr;
    NativeHandler getCoords_ = nullptr;
    NativeHandler setCoords_ = nullptr;
    NativeHandler setHeading_ = nullptr;
    NativeHandler getHeading_ = nullptr;
    NativeHandler getVelocity_ = nullptr;
    NativeHandler getHealth_ = nullptr;
    NativeHandler freeze_ = nullptr;
    NativeHandler setCollision_ = nullptr;
    NativeHandler clearTasks_ = nullptr;
    NativeHandler isShooting_ = nullptr;
    NativeHandler isAiming_ = nullptr;
    NativeHandler isRagdoll_ = nullptr;
    NativeHandler isJumping_ = nullptr;
    NativeHandler selectedWeapon_ = nullptr;
    NativeHandler forwardVector_ = nullptr;
    NativeHandler applyDamage_ = nullptr;
};

} // namespace oxymp::client::game
