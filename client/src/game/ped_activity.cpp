#include "ped_activity.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Набор органов управления игрока. Ноль — тот, которым игрок и играет.
constexpr int kPlayerControls = 0;

/// Органы управления рукопашной в нумерации игры.
constexpr int kInputAttack = 24;
constexpr int kInputMeleeAttackLight = 140;
constexpr int kInputMeleeAttackHeavy = 141;
constexpr int kInputMeleeAttackAlternate = 142;

/// Хеш «безоружен». Каноническое значение joaat от WEAPON_UNARMED и заодно то,
/// что игра возвращает как выбранное оружие, когда в руках ничего нет.
constexpr std::uint32_t kUnarmed = 0xA2719263U;

/// Состояния парашюта в нумерации игры.
///
/// Минус единица — парашюта нет вовсе, ноль — сложен за спиной. Всё, что
/// больше, означает, что игрок уже в воздухе с ним, и различать эти состояния
/// подробнее нам незачем: получателю достаточно знать, что купол раскрыт.
constexpr int kParachuteDeploying = 1;

/// Ответ GET_PED_STEALTH_MOVEMENT: крадётся ли персонаж пригнувшись.
constexpr int kStealthOn = 1;

} // namespace

PedActivity::PedActivity(const NativeTable& table) noexcept
    : isShooting_(table.handlerFor(natives::kIsPedShooting)),
      isAiming_(table.handlerFor(natives::kIsPlayerFreeAiming)),
      isTargetting_(table.handlerFor(natives::kIsPlayerTargettingAnything)),
      isRagdoll_(table.handlerFor(natives::kIsPedRagdoll)),
      isJumping_(table.handlerFor(natives::kIsPedJumping)),
      stealthMovement_(table.handlerFor(natives::kGetPedStealthMovement)),
      isOnVehicle_(table.handlerFor(natives::kIsPedOnVehicle)),
      isClimbing_(table.handlerFor(natives::kIsPedClimbing)),
      isVaulting_(table.handlerFor(natives::kIsPedVaulting)),
      isSwimming_(table.handlerFor(natives::kIsPedSwimming)),
      isUnderwater_(table.handlerFor(natives::kIsPedSwimmingUnderWater)),
      isDiving_(table.handlerFor(natives::kIsPedDiving)),
      isFalling_(table.handlerFor(natives::kIsPedFalling)),
      parachuteState_(table.handlerFor(natives::kGetPedParachuteState)),
      isReloading_(table.handlerFor(natives::kIsPedReloading)),
      isInCover_(table.handlerFor(natives::kIsPedInCover)),
      isGettingUp_(table.handlerFor(natives::kIsPedGettingUp)),
      isDrivingBy_(table.handlerFor(natives::kIsPedDoingDriveby)),
      isInMelee_(table.handlerFor(natives::kIsPedInMeleeCombat)),
      gettingIntoVehicle_(table.handlerFor(natives::kIsPedGettingIntoAVehicle)),
      inAnyVehicle_(table.handlerFor(natives::kIsPedInAnyVehicle)),
      vehicleEntering_(table.handlerFor(natives::kGetVehiclePedIsTryingToEnter)),
      seatEntering_(table.handlerFor(natives::kGetSeatPedIsTryingToEnter)),
      getArmour_(table.handlerFor(natives::kGetPedArmour)),
      justPressed_(table.handlerFor(natives::kIsControlJustPressed)) {}

std::uint32_t PedActivity::flags(int player, int ped, bool dead) const {
    if (ped == 0) {
        return 0;
    }

    std::uint32_t flags = 0;

    // Каждый признак читается через свой натив, и каждый натив может не найтись:
    // хеши привязаны к сборке игры, и одна не найденная строчка не должна
    // ронять весь снимок. Поэтому проверка на пустоту здесь у каждого, а не
    // общее ready() — отсутствие признака означает, что этот признак не
    // передаётся, а не что синхронизации нет.
    const auto set = [&flags](shared::PlayerFlag flag, bool on) {
        if (on) {
            flags |= static_cast<std::uint32_t>(flag);
        }
    };

    const auto ask = [ped](NativeHandler handler) {
        return handler != nullptr && invokeNative<bool>(handler, ped);
    };

    set(shared::PlayerFlag::Dead, dead);

    // Прицел спрашивается двумя вопросами, а не одним. Свободный прицел —
    // это мышь и перекрестье; но у игры есть и второй способ, помощь в
    // наведении, и человек, играющий с ней, свободно не целится ни разу. Со
    // стороны это выглядело так, что часть игроков вообще не поднимает оружие,
    // — и объяснить, почему одни поднимают, а другие нет, было нечем.
    set(shared::PlayerFlag::Aiming,
        (isAiming_ != nullptr && invokeNative<bool>(isAiming_, player)) ||
            (isTargetting_ != nullptr && invokeNative<bool>(isTargetting_, player)));
    set(shared::PlayerFlag::Shooting, ask(isShooting_));
    set(shared::PlayerFlag::Ragdoll, ask(isRagdoll_));
    set(shared::PlayerFlag::Jumping, ask(isJumping_));
    set(shared::PlayerFlag::Climbing, ask(isClimbing_));
    set(shared::PlayerFlag::Vaulting, ask(isVaulting_));
    set(shared::PlayerFlag::Swimming, ask(isSwimming_));
    set(shared::PlayerFlag::Diving, ask(isUnderwater_) || ask(isDiving_));
    set(shared::PlayerFlag::Falling, ask(isFalling_));
    set(shared::PlayerFlag::Reloading, ask(isReloading_));
    set(shared::PlayerFlag::InCover, ask(isInCover_));
    set(shared::PlayerFlag::GettingUp, ask(isGettingUp_));
    set(shared::PlayerFlag::Melee, ask(isInMelee_));

    if (isDrivingBy_ != nullptr) {
        // Последние два довода: считать ли стрельбой прицеливание и стрельбу
        // из окна без оружия в руках. Оба да — снаружи это выглядит одинаково.
        set(shared::PlayerFlag::DriveBy, invokeNative<bool>(isDrivingBy_, ped, true, true));
    }

    if (stealthMovement_ != nullptr) {
        set(shared::PlayerFlag::Crouching,
            invokeNative<int>(stealthMovement_, ped) == kStealthOn);
    }

    if (isOnVehicle_ != nullptr) {
        // Стоит на машине снаружи, а не сидит внутри: игра различает эти два
        // ответа сама, и путать их нельзя — стоящему на крыше не полагается
        // места в салоне.
        set(shared::PlayerFlag::OnVehicle, invokeNative<bool>(isOnVehicle_, ped));
    }

    if (parachuteState_ != nullptr) {
        set(shared::PlayerFlag::Parachuting,
            invokeNative<int>(parachuteState_, ped) >= kParachuteDeploying);
    }

    // Выход из машины игра отдельным вопросом не отвечает — его приходится
    // выводить из трёх её ответов, и вывод здесь точный, а не приблизительный.
    //
    // IS_PED_IN_ANY_VEHICLE с признаком «считать и залезающего» стоит всё время,
    // пока персонаж имеет к машине отношение: от открытой двери при входе до
    // последнего кадра выхода. Без этого признака он стоит только тогда, когда
    // персонаж сидит по-настоящему. Значит «имеет отношение, но не сидит» — это
    // ровно вход или выход, а вход игра называет сама.
    //
    // Считать выход по пропаже InVehicle нельзя, и это здесь уже было: к тому
    // мгновению хозяин уже стоит на асфальте, и получателю остаётся выдернуть
    // куклу из машины рывком. Секунда с лишним, за которую человек открывает
    // дверь и вылезает, не была видна со стороны никогда.
    if (inAnyVehicle_ != nullptr && gettingIntoVehicle_ != nullptr) {
        const bool attached = invokeNative<bool>(inAnyVehicle_, ped, true);
        const bool seated = invokeNative<bool>(inAnyVehicle_, ped, false);
        const bool gettingIn = invokeNative<bool>(gettingIntoVehicle_, ped);

        set(shared::PlayerFlag::LeavingVehicle, attached && !seated && !gettingIn);
    }

    return flags;
}

std::uint16_t PedActivity::armour(int ped) const {
    if (getArmour_ == nullptr || ped == 0) {
        return 0;
    }

    const int value = invokeNative<int>(getArmour_, ped);
    return value > 0 ? static_cast<std::uint16_t>(value) : 0;
}

PedActivity::Entering PedActivity::entering(int ped) const {
    Entering result;

    if (ped == 0 || gettingIntoVehicle_ == nullptr || vehicleEntering_ == nullptr) {
        return result;
    }

    if (!invokeNative<bool>(gettingIntoVehicle_, ped)) {
        return result;
    }

    result.vehicle = invokeNative<int>(vehicleEntering_, ped);

    if (result.vehicle == 0) {
        return result;
    }

    result.seat = seatEntering_ != nullptr
                      ? static_cast<std::int8_t>(invokeNative<int>(seatEntering_, ped))
                      : shared::kDriverSeat;

    return result;
}

bool PedActivity::justPressed(int control) const {
    return justPressed_ != nullptr &&
           invokeNative<bool>(justPressed_, kPlayerControls, control);
}

bool PedActivity::fightsBarehanded(int ped, std::uint32_t weapon) const {
    if (weapon == kUnarmed || weapon == 0) {
        return true;
    }

    // С оружием в руках драка тоже случается — прикладом, ножом, битой. Отличить
    // такое оружие от огнестрельного по одному лишь хешу нельзя, а вот спросить
    // игру, дерётся ли персонаж сейчас, можно.
    return isInMelee_ != nullptr && invokeNative<bool>(isInMelee_, ped);
}

shared::PedAction PedActivity::strike(int ped, std::uint32_t weapon) const {
    if (ped == 0 || !fightsBarehanded(ped, weapon)) {
        return shared::PedAction::None;
    }

    if (justPressed(kInputMeleeAttackHeavy)) {
        return shared::PedAction::HeavyPunch;
    }

    if (justPressed(kInputMeleeAttackAlternate)) {
        return shared::PedAction::Kick;
    }

    // Обычная атака идёт последней и намеренно: на клавиатуре она делит клавишу
    // с быстрым ударом, и спроси мы о ней первой — тяжёлый удар превратился бы в
    // лёгкий.
    if (justPressed(kInputMeleeAttackLight) || justPressed(kInputAttack)) {
        return shared::PedAction::LightPunch;
    }

    return shared::PedAction::None;
}

} // namespace oxymp::client::game
