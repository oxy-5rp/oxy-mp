#include "player.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <algorithm>

namespace oxymp::client::game {
namespace {

/// Как далеко впереди персонажа ставится точка прицела, в метрах.
///
/// Настоящей дальности выстрела здесь не нужно: получателю она задаёт
/// направление, в котором персонаж держит оружие, и на сотне метров направление
/// то же, что и на пятистах.
constexpr float kAimDistance = 100.0F;

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
      isShooting_(table.handlerFor(natives::kIsPedShooting)),
      isAiming_(table.handlerFor(natives::kIsPlayerFreeAiming)),
      isRagdoll_(table.handlerFor(natives::kIsPedRagdoll)),
      isJumping_(table.handlerFor(natives::kIsPedJumping)),
      selectedWeapon_(table.handlerFor(natives::kGetSelectedPedWeapon)),
      forwardVector_(table.handlerFor(natives::kGetEntityForwardVector)),
      applyDamage_(table.handlerFor(natives::kApplyDamageToPed)) {}

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

shared::Vec3 Player::aimPoint(int ped) const {
    const shared::Vec3 position = coords(ped);

    if (forwardVector_ == nullptr) {
        return position;
    }

    NativeContext context;
    context.push(ped);
    forwardVector_(context.address());

    const shared::Vec3 forward{context.result<float>(0), context.result<float>(1),
                               context.result<float>(2)};

    return shared::Vec3{position.x + forward.x * kAimDistance,
                        position.y + forward.y * kAimDistance,
                        position.z + forward.z * kAimDistance};
}

shared::PlayerState Player::snapshot(int player, int ped, bool dead) const {
    shared::PlayerState state;

    if (ped == 0) {
        return state;
    }

    state.position = coords(ped);
    state.heading = heading(ped);
    state.velocity = velocity(ped);
    state.health = static_cast<std::uint16_t>(std::max(health(ped), 0));

    std::uint32_t flags = 0;

    // Смерть берётся не из здоровья, а у самой игры: шкала здоровья персонажа
    // не доходит до нуля в тот же миг, когда игра объявляет игрока мёртвым, и
    // порог по числу пришлось бы подбирать наугад.
    if (dead) {
        flags |= static_cast<std::uint32_t>(shared::PlayerFlag::Dead);
    }

    const bool aiming = isAiming_ != nullptr && invokeNative<bool>(isAiming_, player);
    const bool shooting = isShooting_ != nullptr && invokeNative<bool>(isShooting_, ped);

    if (aiming) {
        flags |= static_cast<std::uint32_t>(shared::PlayerFlag::Aiming);
    }
    if (shooting) {
        flags |= static_cast<std::uint32_t>(shared::PlayerFlag::Shooting);
    }
    if (isRagdoll_ != nullptr && invokeNative<bool>(isRagdoll_, ped)) {
        flags |= static_cast<std::uint32_t>(shared::PlayerFlag::Ragdoll);
    }
    if (isJumping_ != nullptr && invokeNative<bool>(isJumping_, ped)) {
        flags |= static_cast<std::uint32_t>(shared::PlayerFlag::Jumping);
    }

    state.flags = flags;

    if (selectedWeapon_ != nullptr) {
        state.weapon = invokeNative<std::uint32_t>(selectedWeapon_, ped);
    }

    // Точка прицела считается только когда она нужна: это лишний натив на кадр,
    // а стоящему без оружия она ничего не описывает.
    if (aiming || shooting) {
        state.aimAt = aimPoint(ped);
    }

    return state;
}

void Player::applyDamage(int ped, std::uint16_t amount) const {
    if (applyDamage_ == nullptr || ped == 0 || amount == 0) {
        return;
    }

    // Признаки: урон не от игрока-владельца и обычный, не заглушающий. Оружие не
    // называется — его хеш у нас есть, но применять урон именно от оружия значит
    // отдать игре решение о том, сколько снять, а решение это уже принято тем,
    // кто попал.
    invokeNative<void>(applyDamage_, ped, static_cast<int>(amount), false, 0);
}

} // namespace oxymp::client::game
