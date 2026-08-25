#pragma once

#include "native_table.hpp"
#include "ped_animation.hpp"
#include "ped_appearance.hpp"
#include "vehicles.hpp"

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oxymp::client::game {

/// Чужой игрок, каким его нужно показать.
struct RemotePlayerView {
    shared::PlayerId id = shared::kInvalidPlayerId;
    std::string nickname;

    /// Состояние на текущее мгновение: положение и угол поворота уже посчитаны
    /// интерполяцией, остальное взято из последнего снимка.
    shared::PlayerState state;
};

/// Другие игроки сессии в игровом мире.
///
/// Каждому заводится персонаж — обычный, созданный нативами игры, а не сетевой.
/// Сетевых в одиночной игре нет и быть не может: сеть у GTA своя, к серверам
/// Rockstar, и мультиплеер поверх одиночной игры строится именно так — модели
/// расставляются и двигаются вручную по тому, что пришло с нашего сервера.
///
/// Главное решение здесь — как именно двигать персонажа, и оно менялось трижды.
///
/// Сперва движением ведала игра: персонажу выдавалась задача «дойди до точки»,
/// и он шёл туда сам. Точка бралась из интерполяции, то есть посчитанная на
/// текущее мгновение, — и тут же выбрасывалась, потому что вместо того, чтобы
/// оказаться в ней, персонаж начинал к ней идти. Он приходил с опозданием,
/// отставал, а когда отставание становилось невыносимым — телепортировался.
///
/// Тогда положение стали задавать напрямую каждый кадр. Отставание исчезло, но
/// вместе с ним исчезла и физика: персонажа переставляли поверх всего, что
/// делала игра, и он проходил сквозь препятствия, не сдвигал их и не отзывался
/// на удары.
///
/// Затем персонажа вернули в физический мир: он идёт сам, задачей, а расхождение
/// со снимком закрывается долей за кадр — и рывком только тогда, когда
/// расхождение стало таким, что плавно его не пройти.
///
/// Осталась последняя беда, из-за которой чужие стояли повёрнутыми не туда:
/// задача движения ведёт персонажа и заодно решает, куда он смотрит. Пока игрок
/// бежит вперёд, это одно и то же. Стоит ему прицелиться и пойти боком или
/// назад — и одно перестаёт быть другим. Поэтому целящийся ведётся другой
/// задачей: «иди туда, целясь вон туда», — она разводит направление движения и
/// направление взгляда, как их разводит сам игрок. А стоящему угол задаётся
/// прямо, без задачи вовсе.
///
/// Урон персонаж получает по-настоящему, но не умирает от него: сколько у него
/// здоровья, решает его хозяин. Попадание мы лишь замечаем и сообщаем о нём
/// серверу — применит его тот, в кого попали.
///
/// Вызывать можно только изнутри скриптового тика и только от потока с
/// обработчиком скрипта: создание персонажа игра записывает на счёт вызвавшего.
class RemotePlayers {
public:
    /// Куда сообщать о попадании по чужому игроку.
    using DamageSink = std::function<void(shared::PlayerId victim, std::uint16_t amount,
                                          std::uint32_t weapon)>;

    RemotePlayers(const NativeTable& table, const Vehicles& vehicles) noexcept;
    ~RemotePlayers();

    RemotePlayers(const RemotePlayers&) = delete;
    RemotePlayers& operator=(const RemotePlayers&) = delete;

    [[nodiscard]] bool ready() const noexcept;

    /// Куда отправлять замеченные попадания. Без этого они просто не считаются.
    void reportDamageTo(DamageSink sink) { onDamage_ = std::move(sink); }

    /// Приводит мир в соответствие со списком. Вызывать раз в кадр.
    ///
    /// localPed нужен, чтобы отличить наши попадания от чужих: сообщать серверу
    /// о попадании, которого мы не наносили, — значит удваивать урон.
    void sync(const std::vector<RemotePlayerView>& players, int localPed);

    /// Кому принадлежит персонаж. Пусто, если это не чужой игрок.
    [[nodiscard]] shared::PlayerId ownerOf(int ped) const;

    /// Каким персонажем показан игрок. Ноль — им ещё некем: модель могла не
    /// успеть загрузиться, а игрок — только что войти.
    ///
    /// Обратная сторона ownerOf, и обе нужны. Ресурс спрашивает то одно, то
    /// другое: обходя игроков сессии, он идёт от номера к персонажу, а разбирая
    /// попадание луча — от персонажа к номеру.
    [[nodiscard]] int handleFor(shared::PlayerId player) const;

    /// Где персонаж игрока стоит в мире прямо сейчас.
    ///
    /// Именно в мире, а не по последнему снимку, и разница здесь существенная.
    /// Персонаж не переставляется в точку снимка — он подводится к ней
    /// понемногу, оставаясь физическим телом. Всё, что рисуется над ним,
    /// обязано считаться от него самого: взятое от снимка, оно уезжает вперёд
    /// и дёргается на каждой поправке.
    ///
    /// Пусто, если персонажа ещё нет: модель могла не успеть загрузиться.
    [[nodiscard]] std::optional<shared::Vec3> positionOf(shared::PlayerId player) const;

    /// Показывает выстрел чужого игрока.
    ///
    /// Пулей игры, а не вспышкой поверх модели: пуля летит, попадает, оставляет
    /// след на стене и звучит — всё это игра делает сама, стоит назвать ей два
    /// конца и оружие. Тем же путём летят ракета и граната: для игры это такой
    /// же выстрел, только снаряд другой, и разница целиком в хеше оружия.
    ///
    /// Урона пуля не наносит, и это не осторожность, а устройство: попадания
    /// считает стрелявший у себя и присылает их отдельным сообщением. Пуля,
    /// бьющая по-настоящему, ударила бы второй раз по тому, кто уже посчитан.
    void fire(shared::PlayerId player, std::uint32_t weapon, const shared::Vec3& target);

    /// Запоминает, как собрано оружие у чужого игрока.
    ///
    /// Помнится, а не применяется сразу, и по той же причине, что и одежда:
    /// сообщение идёт надёжным каналом и обгоняет снимки, а куклы до первого
    /// снимка ещё нет. Да и надевать насадки можно только вместе с самим
    /// стволом — поставленные на оружие, которого у персонажа нет, игра
    /// проглатывает молча.
    void arm(shared::PlayerId player, const shared::PlayerWeapon& look);

    /// Одевает чужого игрока так, как он объявил.
    ///
    /// Помнится, а не только применяется, и это не запас: внешность приходит по
    /// надёжному каналу и обгоняет снимки, а персонажа до первого снимка ещё
    /// нет. Забыв её, мы одели бы человека только после следующей смены одежды —
    /// то есть, скорее всего, никогда.
    void dress(shared::PlayerId player, const shared::PlayerAppearance& appearance);

    /// Убирает всех. Нужно при выходе: оставленные персонажи переживут клиент.
    void clear();

    [[nodiscard]] std::size_t shown() const noexcept { return puppets_.size(); }

private:
    /// Персонаж чужого игрока и всё, что мы о нём помним.
    ///
    /// Одной записью, а не несколькими картами по ключу игрока: разложенные по
    /// отдельным картам, эти поля обязаны меняться согласованно, и любая
    /// забытая правка в одном месте оставляет запись о персонаже, которого уже
    /// нет.
    struct Puppet {
        /// Номер персонажа в игре.
        int ped = 0;

        /// Куда кукла сейчас смотрит и когда ей это велели.
        ///
        /// По ним видно, стоит ли выдавать задачу взгляда заново: каждый кадр
        /// её выдавать нельзя — голова начинает поворот сначала тридцать раз в
        /// секунду и замирает, не дойдя и до половины.
        shared::Vec3 lookAt;
        std::int32_t lookedAt = 0;

        /// Куда сейчас направлена задача движения. По ней видно, развернулся ли
        /// игрок настолько, чтобы задачу имело смысл выдать заново.
        shared::Vec3 aim;

        /// Когда задача выдавалась, по игровому времени. Ноль — не выдавалась.
        std::int32_t taskedAt = 0;

        /// Целился ли персонаж, когда задача выдавалась.
        ///
        /// Целящегося и просто идущего ведут разные задачи, и смена одного на
        /// другое обязана менять и задачу — иначе прицелившийся продолжит идти
        /// прежней, глядя себе под ноги.
        bool taskedAiming = false;

        /// Какое здоровье мы поставили в прошлый раз. По разнице с нынешним
        /// видно, попал ли кто-то по персонажу здесь, у нас.
        int appliedHealth = 0;

        /// Оружие, которое персонажу уже выдано. Выдавать заново каждый кадр
        /// нельзя: игра при этом перезаряжает его и сбрасывает позу.
        std::uint32_t weapon = 0;

        /// Сколько патронов ему уже поставлено. Догоняются они отдельно от
        /// выдачи оружия: выдача сбрасывает позу, а патроны меняются с каждым
        /// выстрелом.
        std::uint16_t ammo = 0;

        /// Когда кукла в последний раз стреляла присланным выстрелом.
        ///
        /// По ней и решается, стрелять ли ей вдобавок самой. Пока присланные
        /// выстрелы идут, своих не нужно: вышла бы двойная очередь, и половина
        /// её летела бы не туда, куда целился хозяин, а куда попадёт кукла со
        /// своей меткостью. Перестали идти — кукла стреляет сама, как и
        /// прежде: старый клиент выстрелов не шлёт вовсе, а новый мог их
        /// растерять по дороге.
        std::int32_t firedAt = 0;

        /// Признаки из прошлого снимка. Нужны, чтобы отличать переход от
        /// состояния: рэгдолл и посадку в машину заказывают один раз.
        std::uint32_t flags = 0;

        /// В какой машине и на каком месте персонаж сидит сейчас.
        shared::VehicleId vehicleId = shared::kInvalidVehicleId;
        std::int8_t seat = shared::kNoSeat;

        /// Когда персонажу выдана задача залезть в машину. Ноль — не выдана.
        ///
        /// По этому времени видно, что вход не удался: дверь могло заклинить о
        /// стену, а хозяин к этому времени уже едет. Тогда персонажа приходится
        /// сажать рывком — некрасиво, но лучше, чем оставить его снаружи.
        std::int32_t enteringSince = 0;

        /// Номер последнего показанного короткого движения.
        std::uint8_t actionSequence = 0;
    };

    /// Заводит персонажа для игрока. Ноль, если модель ещё не загрузилась.
    [[nodiscard]] int spawn(const RemotePlayerView& player);

    void remove(int ped) const;

    /// Ведёт персонажа на своих двоих: положение, поворот, походку.
    ///
    /// seconds — сколько длился кадр. Расхождение со снимком закрывается
    /// понемногу, и «понемногу» обязано считаться от времени, а не от кадров.
    ///
    /// busy означает «персонаж занят своим движением»: прыгает, лезет, сидит в
    /// укрытии. Тогда положение подводится по-прежнему, а походка и задача
    /// движения не трогаются вовсе — они отменили бы начатое.
    void walk(Puppet& puppet, const RemotePlayerView& player, float seconds, std::int32_t now,
              bool busy) const;

    /// Задаёт угол поворота персонажа.
    void turn(const Puppet& puppet, const RemotePlayerView& player, bool moving) const;

    /// Выдаёт задачу движения, если прежняя устарела.
    void steer(Puppet& puppet, const RemotePlayerView& player, float speed, std::int32_t now) const;

    /// Сажает персонажа в машину, высаживает обратно и показывает вход.
    void ride(Puppet& puppet, const RemotePlayerView& player, std::int32_t now) const;

    /// Ведёт оружие: выдачу, прицел и стрельбу стоящего.
    void aim(Puppet& puppet, const RemotePlayerView& player) const;

    /// Поворачивает кукле голову туда, куда смотрит её хозяин.
    ///
    /// Задача взгляда вторична: она уживается с ходьбой и не отменяет её — этим
    /// и отличается от задачи прицела, которая тело разворачивает. Отсюда и то,
    /// что она выдаётся всем, кто не целится: целящийся и так смотрит туда,
    /// куда целится.
    void look(Puppet& puppet, const RemotePlayerView& player, std::int32_t now) const;

    /// Показывает короткое движение, если оно новое.
    void act(Puppet& puppet, const RemotePlayerView& player);

    /// Согласует здоровье с тем, что сказал хозяин, и замечает наши попадания.
    void settleHealth(Puppet& puppet, const RemotePlayerView& player, int localPed) const;

    /// Хеш модели, которой показываются чужие игроки.
    [[nodiscard]] std::uint32_t model();

    const Vehicles& vehicles_;
    DamageSink onDamage_;

    /// Движения, которыми отыгрывается то, чем игрок занят.
    PedAnimation animation_;

    /// Одежда и лицо.
    PedAppearance appearance_;

    /// Как выглядит каждый игрок. Живёт дольше персонажа: тот появляется и
    /// исчезает по мере удаления, а внешность объявляется однажды.
    std::unordered_map<shared::PlayerId, shared::PlayerAppearance> looks_;

    /// Как собрано оружие у каждого. Рядом с одеждой и по тем же правилам:
    /// приходит надёжным каналом, живёт минутами и обгоняет снимки. Надевается
    /// вместе со стволом — см. aim.
    std::unordered_map<shared::PlayerId, shared::PlayerWeapon> guns_;

    NativeHandler hashKey_ = nullptr;
    NativeHandler requestModel_ = nullptr;
    NativeHandler hasModelLoaded_ = nullptr;
    NativeHandler createPed_ = nullptr;
    NativeHandler deletePed_ = nullptr;
    NativeHandler doesExist_ = nullptr;
    NativeHandler setCoords_ = nullptr;
    NativeHandler setHeading_ = nullptr;
    NativeHandler desiredHeading_ = nullptr;
    NativeHandler defaultVariation_ = nullptr;
    NativeHandler blockEvents_ = nullptr;
    NativeHandler asMissionEntity_ = nullptr;
    NativeHandler canBeTargetted_ = nullptr;
    NativeHandler diesWhenInjured_ = nullptr;
    NativeHandler invincible_ = nullptr;
    NativeHandler lodDistance_ = nullptr;
    NativeHandler getCoords_ = nullptr;
    NativeHandler taskGoTo_ = nullptr;
    NativeHandler taskGoToAiming_ = nullptr;
    NativeHandler moveBlend_ = nullptr;
    NativeHandler getHealth_ = nullptr;
    NativeHandler setHealth_ = nullptr;
    NativeHandler setArmour_ = nullptr;
    NativeHandler gameTimer_ = nullptr;
    NativeHandler clearTasks_ = nullptr;
    NativeHandler damagedBy_ = nullptr;
    NativeHandler clearDamage_ = nullptr;
    NativeHandler giveWeapon_ = nullptr;
    NativeHandler setWeapon_ = nullptr;
    NativeHandler setAmmo_ = nullptr;
    NativeHandler taskAim_ = nullptr;
    NativeHandler taskLookAt_ = nullptr;
    NativeHandler taskShoot_ = nullptr;
    NativeHandler setRagdoll_ = nullptr;
    NativeHandler canRagdoll_ = nullptr;
    NativeHandler setIntoVehicle_ = nullptr;
    NativeHandler enterVehicle_ = nullptr;
    NativeHandler leaveVehicle_ = nullptr;
    NativeHandler isInVehicle_ = nullptr;
    NativeHandler giveComponent_ = nullptr;
    NativeHandler setWeaponTint_ = nullptr;
    NativeHandler shootBullet_ = nullptr;
    NativeHandler throwProjectile_ = nullptr;
    NativeHandler weaponGroup_ = nullptr;
    NativeHandler shotTimer_ = nullptr;
    NativeHandler boneCoords_ = nullptr;

    std::uint32_t model_ = 0;

    /// Когда шёл прошлый кадр, по игровому времени. Нужно для его длительности.
    std::int32_t framedAt_ = 0;

    /// Кому какой персонаж принадлежит и что с ним происходит.
    std::unordered_map<shared::PlayerId, Puppet> puppets_;
};

} // namespace oxymp::client::game
