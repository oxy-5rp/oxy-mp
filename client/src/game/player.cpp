#include "player.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <algorithm>
#include <cmath>

namespace oxymp::client::game {
namespace {

/// Как далеко впереди персонажа ставится точка прицела, в метрах.
///
/// Настоящей дальности выстрела здесь не нужно: получателю она задаёт
/// направление, в котором персонаж держит оружие, и на сотне метров направление
/// то же, что и на пятистах.
constexpr float kAimDistance = 100.0F;

/// Порядок поворотов, которым игра описывает всё: сперва наклон, потом крен,
/// потом рыскание.
constexpr int kGameRotationOrder = 2;

/// Сколько замеченный удар держится в снимке, в миллисекундах.
///
/// Снимок собирается каждый кадр, а уходит на сервер раз в пятьдесят
/// миллисекунд. Удар, замеченный в одном кадре и забытый в следующем, попал бы в
/// отправленный снимок только по счастливой случайности. Двести миллисекунд —
/// это заведомо несколько отправок и заведомо меньше, чем промежуток между двумя
/// ударами человека.
constexpr std::int32_t kActionHold = 200;

} // namespace

Player::Player(const NativeTable& table) noexcept
    : playerId_(table.handlerFor(natives::kPlayerId)),
      playerPedId_(table.handlerFor(natives::kPlayerPedId)),
      isPlaying_(table.handlerFor(natives::kIsPlayerPlaying)),
      getCoords_(table.handlerFor(natives::kGetEntityCoords)),
      setCoords_(table.handlerFor(natives::kSetEntityCoords)),
      setHeading_(table.handlerFor(natives::kSetEntityHeading)),
      getHeading_(table.handlerFor(natives::kGetEntityHeading)),
      getVelocity_(table.handlerFor(natives::kGetEntityVelocity)),
      getHealth_(table.handlerFor(natives::kGetEntityHealth)),
      freeze_(table.handlerFor(natives::kFreezeEntityPosition)),
      setCollision_(table.handlerFor(natives::kSetEntityCollision)),
      clearTasks_(table.handlerFor(natives::kClearPedTasksImmediately)),
      selectedWeapon_(table.handlerFor(natives::kGetSelectedPedWeapon)),
      applyDamage_(table.handlerFor(natives::kApplyDamageToPed)),
      setHealth_(table.handlerFor(natives::kSetEntityHealth)),
      setArmour_(table.handlerFor(natives::kSetPedArmour)),
      camRotation_(table.handlerFor(natives::kGetGameplayCamRot)),
      giveWeapon_(table.handlerFor(natives::kGiveWeaponToPed)),
      giveComponent_(table.handlerFor(natives::kGiveWeaponComponentToPed)),
      setWeaponTint_(table.handlerFor(natives::kSetPedWeaponTintIndex)),
      removeAllWeapons_(table.handlerFor(natives::kRemoveAllPedWeapons)),
      getAmmo_(table.handlerFor(natives::kGetAmmoInPedWeapon)),
      activity_(table),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)) {}

bool Player::ready() const noexcept {
    return playerId_ != nullptr && playerPedId_ != nullptr && isPlaying_ != nullptr &&
           getCoords_ != nullptr && setCoords_ != nullptr && setHeading_ != nullptr &&
           getHeading_ != nullptr && freeze_ != nullptr && setCollision_ != nullptr &&
           clearTasks_ != nullptr;
}

void Player::release(int ped) const {
    if (!ready() || ped == 0) {
        return;
    }

    invokeNative<void>(freeze_, ped, false);
    invokeNative<void>(setCollision_, ped, true, true);
    invokeNative<void>(clearTasks_, ped);
}

int Player::id() const {
    if (!ready()) {
        return 0;
    }

    return invokeNative<int>(playerId_);
}

int Player::ped() const {
    if (!ready()) {
        return 0;
    }

    return invokeNative<int>(playerPedId_);
}

bool Player::playing() const {
    if (!ready()) {
        return false;
    }

    const int player = invokeNative<int>(playerId_);
    return invokeNative<bool>(isPlaying_, player);
}

shared::Vec3 Player::coords(int ped) const {
    if (!ready() || ped == 0) {
        return {};
    }

    // Тройка координат возвращается тремя ячейками подряд, а не одной
    // структурой: так устроен возврат векторов в скриптовом движке.
    NativeContext context;
    context.push(ped);
    context.push(true); // учитывать смещение модели

    getCoords_(context.address());

    return shared::Vec3{context.result<float>(0), context.result<float>(1),
                        context.result<float>(2)};
}

int Player::health(int ped) const {
    if (getHealth_ == nullptr || ped == 0) {
        return 0;
    }

    return invokeNative<int>(getHealth_, ped);
}

shared::Vec3 Player::velocity(int ped) const {
    if (getVelocity_ == nullptr || ped == 0) {
        return {};
    }

    NativeContext context;
    context.push(ped);
    getVelocity_(context.address());

    // Тройка чисел подряд, а не структура: так игра возвращает векторы.
    return shared::Vec3{context.result<float>(0), context.result<float>(1),
                        context.result<float>(2)};
}

float Player::heading(int ped) const {
    if (!ready() || ped == 0) {
        return 0.0F;
    }

    return invokeNative<float>(getHeading_, ped);
}

void Player::teleport(int ped, shared::Vec3 position) const {
    if (!ready() || ped == 0) {
        return;
    }

    // Последние три признака: не выключать столкновения, не искать землю и
    // считать перенос мгновенным. Поиск земли здесь только мешает — он
    // притягивает персонажа к первой найденной поверхности, включая крыши.
    invokeNative<void>(setCoords_, ped, position.x, position.y, position.z, false, false, false,
                       true);
}

void Player::setHeading(int ped, float degrees) const {
    if (!ready() || ped == 0) {
        return;
    }

    invokeNative<void>(setHeading_, ped, degrees);
}

void Player::freeze(int ped, bool frozen) const {
    if (freeze_ == nullptr || ped == 0) {
        return;
    }

    invokeNative<void>(freeze_, ped, frozen);
}

shared::Vec3 Player::cameraPoint(int ped, float range) const {
    const shared::Vec3 position = coords(ped);

    // Точка отсчёта — голова, а не подошвы: луч, пущенный от земли, уводит шею
    // и ствол вверх тем сильнее, чем ближе цель.
    const shared::Vec3 head{position.x, position.y, position.z + shared::kLookHeight};

    if (camRotation_ == nullptr) {
        return head;
    }

    // Порядок поворотов — второй, тот же, которым игра описывает всё
    // остальное. Возвращается тройка: наклон, крен и рыскание в градусах.
    NativeContext context;
    context.push(kGameRotationOrder);
    camRotation_(context.address());

    const float pitch = context.result<float>(0) * shared::kRadians;
    const float yaw = context.result<float>(2) * shared::kRadians;

    // Направление из двух углов. Знак у горизонтали такой, а не иной, потому
    // что у GTA ось Y смотрит на север, а угол растёт против часовой: при
    // нулевом рыскании взгляд направлен на север.
    const float flat = std::cos(pitch);

    return shared::Vec3{head.x - std::sin(yaw) * flat * range,
                        head.y + std::cos(yaw) * flat * range,
                        head.z + std::sin(pitch) * range};
}

shared::Vec3 Player::aimPoint(int ped) const {
    // Тем же лучом, что и взгляд, — и это правка, а не наведение порядка.
    //
    // Прежде точка прицела бралась из направления тела
    // (GET_ENTITY_FORWARD_VECTOR) от подошв персонажа. У стоящего человека это
    // направление горизонтально всегда: целясь вверх или вниз, он поворачивает
    // камеру и верхнюю часть тела, а само тело остаётся стоять прямо. Значит
    // наклона в точке прицела не было вовсе — ни у кого и никогда.
    //
    // Видно это было дважды. Чужой игрок целился строго перед собой, куда бы ни
    // наводил его хозяин. И присланные выстрелы летели туда же: стреляющий с
    // балкона или по балкону слал пули в горизонт, и попасть у него не могло
    // получиться ни разу.
    return cameraPoint(ped, kAimDistance);
}

shared::Vec3 Player::lookPoint(int ped) const {
    return cameraPoint(ped, shared::kLookRange);
}

shared::PlayerState Player::snapshot(int player, int ped, bool dead) {
    shared::PlayerState state;

    if (ped == 0) {
        return state;
    }

    state.position = coords(ped);
    state.heading = heading(ped);
    state.velocity = velocity(ped);
    state.health = static_cast<std::uint16_t>(std::max(health(ped), 0));
    state.armour = activity_.armour(ped);

    // Смерть берётся не из здоровья, а у самой игры: шкала здоровья персонажа
    // не доходит до нуля в тот же миг, когда игра объявляет игрока мёртвым, и
    // порог по числу пришлось бы подбирать наугад. Поэтому она приходит сюда
    // доводом от того, кто разбирает смерть.
    state.flags = activity_.flags(player, ped, dead);

    if (selectedWeapon_ != nullptr) {
        state.weapon = invokeNative<std::uint32_t>(selectedWeapon_, ped);
        state.ammo = ammo(ped, state.weapon);
    }

    // Целящийся смотрит туда, куда целится, — и точка берётся у оружия.
    // Остальные смотрят туда, куда повёрнута камера, и это направление телу не
    // равно: человек идёт прямо и оглядывается по сторонам.
    state.aimAt = shared::has(state.flags, shared::PlayerFlag::Aiming) ||
                          shared::has(state.flags, shared::PlayerFlag::Shooting)
                      ? aimPoint(ped)
                      : lookPoint(ped);

    state.action = holdStrike(activity_.strike(ped, state.weapon));
    state.actionSequence = actionSequence_;

    return state;
}

std::optional<shared::WeaponFired> Player::shot(const shared::PlayerState& state) {
    const std::uint32_t weapon = firedWeapon_;
    const std::uint16_t ammo = firedAmmo_;

    // Запоминается всегда, даже когда выстрела не было: иначе следующий кадр
    // сравнивал бы нынешние патроны с позапрошлыми.
    firedWeapon_ = state.weapon;
    firedAmmo_ = state.ammo;

    if (state.weapon == 0 || state.weapon != weapon) {
        // Оружие сменилось: разница в патронах сейчас означает не выстрел, а
        // другой магазин.
        return std::nullopt;
    }

    if (state.ammo >= ammo) {
        return std::nullopt;
    }

    if (!shared::has(state.flags, shared::PlayerFlag::Shooting)) {
        // Патроны убыли, а на спуск не жали: их отобрал сервер либо игрок
        // выбросил оружие.
        return std::nullopt;
    }

    shared::WeaponFired fired;
    fired.weapon = state.weapon;

    // Куда ушёл выстрел — та же точка, куда игрок целится: она уже посчитана
    // для снимка, и считать её второй раз значило бы завести второй источник
    // правды об одном и том же.
    fired.target = state.aimAt;

    return fired;
}

shared::PedAction Player::holdStrike(shared::PedAction started) {
    const auto now = gameTimer_ != nullptr ? invokeNative<std::int32_t>(gameTimer_) : 0;

    if (started != shared::PedAction::None) {
        action_ = started;
        actionAt_ = now;

        // Счётчик двигается вместе с самим движением, а не с кадром: получатель
        // отличает по нему новый удар от того же самого, а не считает снимки.
        ++actionSequence_;

        return action_;
    }

    // Держится ровно столько, чтобы попасть хотя бы в один отправленный снимок,
    // и не дольше: иначе следующий удар оказался бы неотличим от эха прошлого.
    if (action_ != shared::PedAction::None && now - actionAt_ >= kActionHold) {
        action_ = shared::PedAction::None;
    }

    return action_;
}

void Player::applyDamage(int ped, std::uint16_t amount) const {
    if (applyDamage_ == nullptr || ped == 0 || amount == 0) {
        return;
    }

    // Признаки: урон не от игрока-владельца и обычный, не заглушающий. Оружие не
    // называется — его хеш у нас есть, но применять урон именно от оружия значит
    // отдать игре решение о том, сколько снять, а решать это ей больше не по
    // чину: сколько снять, решил сервер, и он же уже снял.
    invokeNative<void>(applyDamage_, ped, static_cast<int>(amount), false, 0);
}

void Player::applyHealth(int ped, std::uint16_t health, std::uint16_t armour) const {
    if (ped == 0) {
        return;
    }

    if (setHealth_ != nullptr) {
        invokeNative<void>(setHealth_, ped, static_cast<int>(health));
    }
    if (setArmour_ != nullptr) {
        invokeNative<void>(setArmour_, ped, static_cast<int>(armour));
    }
}

void Player::applyLoadout(int ped, const std::vector<shared::WeaponSlot>& weapons,
                          bool replace) const {
    if (ped == 0 || giveWeapon_ == nullptr) {
        return;
    }

    if (replace && removeAllWeapons_ != nullptr) {
        invokeNative<void>(removeAllWeapons_, ped, true);
    }

    for (const shared::WeaponSlot& slot : weapons) {
        if (slot.weapon == 0) {
            continue;
        }

        // Последние признаки: не брать сразу в руки и не делать оружие
        // сюжетным. В руки берёт сам игрок — подменять ему оружие в разгар
        // перестрелки оттого, что сервер прислал список, было бы издевательством.
        invokeNative<void>(giveWeapon_, ped, slot.weapon, static_cast<int>(slot.ammo), false,
                           false);

        // Насадки — после самого ствола и только после него: поставленное на
        // оружие, которого у персонажа ещё нет, игра проглатывает молча.
        if (giveComponent_ != nullptr) {
            for (const std::uint32_t component : slot.components) {
                if (component != 0) {
                    invokeNative<void>(giveComponent_, ped, slot.weapon, component);
                }
            }
        }

        // Расцветка ставится всегда, включая заводскую: список полный, и ноль
        // здесь означает «покрасить как с завода», а не «оставить как было».
        if (setWeaponTint_ != nullptr) {
            invokeNative<void>(setWeaponTint_, ped, slot.weapon, static_cast<int>(slot.tint));
        }
    }
}

std::uint16_t Player::ammo(int ped, std::uint32_t weapon) const {
    if (getAmmo_ == nullptr || ped == 0 || weapon == 0) {
        return 0;
    }

    const int count = invokeNative<int>(getAmmo_, ped, weapon);

    // Отрицательного боезапаса не бывает, а бесконечный игра обозначает большим
    // числом: и то и другое приводится к пределу поля, чтобы не превратиться по
    // дороге в свою противоположность.
    return static_cast<std::uint16_t>(std::clamp(count, 0, 0xFFFF));
}

} // namespace oxymp::client::game
