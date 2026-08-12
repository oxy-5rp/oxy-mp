#pragma once

#include "native_table.hpp"
#include "vehicles.hpp"

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oxymp::client::game {

/// Чужой игрок, каким его нужно показать.
struct RemotePlayerView {
    shared::PlayerId id = shared::kInvalidPlayerId;
    std::string nickname;

    /// Состояние на текущее мгновение: положение уже посчитано интерполяцией,
    /// остальное взято из последнего снимка.
    shared::PlayerState state;
};

/// Другие игроки сессии в игровом мире.
///
/// Каждому заводится персонаж — обычный, созданный нативами игры, а не сетевой.
/// Сетевых в одиночной игре нет и быть не может: сеть у GTA своя, к серверам
/// Rockstar, и мультиплеер поверх одиночной игры строится именно так — модели
/// расставляются и двигаются вручную по тому, что пришло с нашего сервера.
///
/// Главное решение здесь — как именно двигать персонажа, и оно менялось дважды.
///
/// Сперва движением ведала игра: персонажу выдавалась задача «дойди до точки»,
/// и он шёл туда сам. Точка бралась из интерполяции, то есть посчитанная на
/// текущее мгновение, — и тут же выбрасывалась, потому что вместо того, чтобы
/// оказаться в ней, персонаж начинал к ней идти. Он приходил с опозданием,
/// отставал, а когда отставание становилось невыносимым — телепортировался.
///
/// Тогда положение стали задавать напрямую каждый кадр. Отставание исчезло, но
/// вместе с ним исчезла и физика: персонажа переставляли поверх всего, что
/// делала игра, и он проходил сквозь препятствия, не сдвигал их и не
/// отзывался на удары. Именно это и было видно со стороны как «нет коллизий» и
/// «дёргается».
///
/// Теперь так: персонаж остаётся физическим телом и идёт сам, задачей, а
/// расхождение со снимком закрывается долей за кадр — и рывком только тогда,
/// когда расхождение стало таким, что плавно его не пройти. Персонаж при этом
/// живой: по нему можно попасть, его можно сбить машиной, он упирается в стены.
///
/// Урон он получает по-настоящему, но не умирает от него: сколько у него
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
    ///
    /// Нужно пассажиру: сидя в чужой машине, он узнаёт её хозяина по персонажу
    /// за рулём — а перевести персонажа обратно в игрока можно только здесь.
    [[nodiscard]] shared::PlayerId ownerOf(int ped) const;

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

        /// Куда сейчас направлена задача движения. По ней видно, развернулся ли
        /// игрок настолько, чтобы задачу имело смысл выдать заново.
        shared::Vec3 aim;

        /// Когда задача выдавалась, по игровому времени. Ноль — не выдавалась.
        std::int32_t taskedAt = 0;

        /// Какое здоровье мы поставили в прошлый раз. По разнице с нынешним
        /// видно, попал ли кто-то по персонажу здесь, у нас.
        int appliedHealth = 0;

        /// Оружие, которое персонажу уже выдано. Выдавать заново каждый кадр
        /// нельзя: игра при этом перезаряжает его и сбрасывает позу.
        std::uint32_t weapon = 0;

        /// Признаки из прошлого снимка. Нужны, чтобы отличать переход от
        /// состояния: рэгдолл и посадку в машину заказывают один раз.
        std::uint32_t flags = 0;

        /// В какой машине и на каком месте персонаж сидит сейчас.
        shared::PlayerId vehicleOwner = shared::kInvalidPlayerId;
        std::int8_t seat = shared::kDriverSeat;
    };

    /// Заводит персонажа для игрока. Ноль, если модель ещё не загрузилась.
    [[nodiscard]] int spawn(const RemotePlayerView& player);

    void remove(int ped) const;

    /// Ведёт персонажа на своих двоих: положение, поворот, походку.
    void walk(Puppet& puppet, const RemotePlayerView& player) const;

    /// Сажает персонажа в машину и высаживает обратно.
    void ride(Puppet& puppet, const RemotePlayerView& player) const;

    /// Ведёт оружие: выдачу, прицел и стрельбу.
    void aim(Puppet& puppet, const RemotePlayerView& player) const;

    /// Согласует здоровье с тем, что сказал хозяин, и замечает наши попадания.
    void settleHealth(Puppet& puppet, const RemotePlayerView& player, int localPed) const;

    /// Хеш модели, которой показываются чужие игроки.
    [[nodiscard]] std::uint32_t model();

    const Vehicles& vehicles_;
    DamageSink onDamage_;

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
    NativeHandler getHeading_ = nullptr;
    NativeHandler taskGoTo_ = nullptr;
    NativeHandler moveBlend_ = nullptr;
    NativeHandler getHealth_ = nullptr;
    NativeHandler setHealth_ = nullptr;
    NativeHandler gameTimer_ = nullptr;
    NativeHandler clearTasks_ = nullptr;
    NativeHandler damagedBy_ = nullptr;
    NativeHandler clearDamage_ = nullptr;
    NativeHandler giveWeapon_ = nullptr;
    NativeHandler setWeapon_ = nullptr;
    NativeHandler taskAim_ = nullptr;
    NativeHandler taskShoot_ = nullptr;
    NativeHandler setRagdoll_ = nullptr;
    NativeHandler canRagdoll_ = nullptr;
    NativeHandler setIntoVehicle_ = nullptr;
    NativeHandler leaveVehicle_ = nullptr;

    std::uint32_t model_ = 0;

    /// Кому какой персонаж принадлежит и что с ним происходит.
    std::unordered_map<shared::PlayerId, Puppet> puppets_;
};

} // namespace oxymp::client::game
