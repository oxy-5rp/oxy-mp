#pragma once

#include "native_table.hpp"
#include "ped_activity.hpp"

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
    ///
    /// Не const, и это не оплошность: в снимке есть счётчик коротких движений, а
    /// он обязан меняться ровно тогда, когда меняется поле рядом с ним. Отдай мы
    /// его вызывающему — и рано или поздно нашлось бы место, где снимок собран,
    /// а счётчик остался прежним.
    [[nodiscard]] shared::PlayerState snapshot(int player, int ped, bool dead);

    /// Куда персонаж лезет прямо сейчас, если лезет.
    [[nodiscard]] PedActivity::Entering entering(int ped) const { return activity_.entering(ped); }

    /// Наносит персонажу урон, о котором сообщил сервер.
    ///
    /// Урон считает тот, кто попал, а применяет тот, в кого попали: персонаж
    /// чужого игрока у стрелявшего — обычный болванчик, и происходящее с ним для
    /// хозяина не существует.
    void applyDamage(int ped, std::uint16_t amount) const;

    /// Ставит здоровье и броню, назначенные сервером.
    ///
    /// Ставит, а не отнимает: здоровье принадлежит серверу, и клиент о нём
    /// больше не свидетельствует — он о нём узнаёт. Урон при этом остаётся
    /// отдельным сообщением, но меняет он только картинку: вспышку на экране да
    /// строку о том, кто попал.
    void applyHealth(int ped, std::uint16_t health, std::uint16_t armour) const;

    /// Выдаёт персонажу оружие, назначенное сервером.
    ///
    /// replace означает «взамен всего»: с ним персонаж сперва разоружается
    /// начисто. Без него выданное добавляется к тому, что уже есть.
    void applyLoadout(int ped, const std::vector<shared::WeaponSlot>& weapons, bool replace) const;

    /// Сколько патронов осталось в оружии, которое персонаж держит.
    [[nodiscard]] std::uint16_t ammo(int ped, std::uint32_t weapon) const;

private:
    /// Точка, куда направлено оружие.
    ///
    /// Оружие смотрит туда же, куда камера, поэтому точка берётся впереди
    /// персонажа по направлению взгляда. Настоящий луч прицела игра наружу не
    /// отдаёт, а для чужой стороны важно направление, а не попадание: попадания
    /// считает стрелявший у себя.
    [[nodiscard]] shared::Vec3 aimPoint(int ped) const;

    /// Держит замеченный удар в снимке достаточно долго, чтобы он ушёл по сети.
    [[nodiscard]] shared::PedAction holdStrike(shared::PedAction started);


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
    NativeHandler selectedWeapon_ = nullptr;
    NativeHandler forwardVector_ = nullptr;
    NativeHandler applyDamage_ = nullptr;
    NativeHandler setHealth_ = nullptr;
    NativeHandler setArmour_ = nullptr;
    NativeHandler giveWeapon_ = nullptr;
    NativeHandler giveComponent_ = nullptr;
    NativeHandler setWeaponTint_ = nullptr;
    NativeHandler removeAllWeapons_ = nullptr;
    NativeHandler getAmmo_ = nullptr;

    /// Чем персонаж занят помимо перемещения.
    PedActivity activity_;

    NativeHandler gameTimer_ = nullptr;

    /// Последнее замеченное короткое движение и когда оно замечено.
    ///
    /// Движение держится в снимке дольше, чем длится его начало, и без этого оно
    /// не дошло бы вовсе: снимок собирается каждый кадр, а уходит на сервер раз в
    /// полсотни миллисекунд. Удар, замеченный в одном кадре и забытый в
    /// следующем, попал бы в отправляемый снимок разве что случайно.
    ///
    /// Повторов получатель не боится: он отличает их по номеру.
    shared::PedAction action_ = shared::PedAction::None;
    std::int32_t actionAt_ = 0;

    /// Номер последнего объявленного короткого движения.
    ///
    /// Считает движения, а не кадры: получатель по нему отличает «всё ещё тот же
    /// удар» от «ударил снова». Переполнение безвредно — сравнивается только
    /// равенство соседних значений.
    std::uint8_t actionSequence_ = 0;
};

} // namespace oxymp::client::game
