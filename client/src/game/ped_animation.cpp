#include "ped_animation.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

#include <string>

namespace oxymp::client::game {
namespace {

/// Набор движений рукопашной, которым игра бьёт без оружия.
constexpr std::string_view kMeleeDictionary = "melee@unarmed@streamed_core";

/// Движения из этого набора.
///
/// Единственное место в клиенте, где движения названы по имени, и потому
/// единственное, где имя может оказаться неверным: набор либо есть в игре, либо
/// нет, а спросить у неё список наперёд нельзя. Неверное имя не роняет ничего —
/// движение просто не проиграется, — но и не остаётся незамеченным: о наборе,
/// который не загрузился, говорится в журнал.
constexpr std::string_view kLightPunch = "short_0_attack";
constexpr std::string_view kHeavyPunch = "heavy_punch_a";
constexpr std::string_view kKick = "ground_attack_on_spot";

/// Насколько плавно движение входит и выходит. Больше — резче.
constexpr float kBlendIn = 8.0F;
constexpr float kBlendOut = -8.0F;

/// Сколько движению отведено, в миллисекундах. Минус единица — до конца.
constexpr int kUntilDone = -1;

/// Признаки проигрывания.
///
/// Восьмёрка — «только верхняя часть тела, поверх остального»: персонаж бьёт, не
/// переставая при этом идти. Единица к ней — не возвращать тело в исходное
/// положение по окончании: возвращённое, оно дёрнулось бы обратно к тому месту,
/// где удар начался, а мы это место задаём сами каждый кадр.
constexpr int kUpperBodyOverlay = 8 | 1;

/// Скорость проигрывания. Единица — обычная.
constexpr float kNormalRate = 1.0F;

/// Сколько ждать загрузки набора, прежде чем счесть его несуществующим.
constexpr int kLoadPatience = 5000;

/// Крадётся ли персонаж, в нумерации игры.
constexpr bool kStealthOn = true;
constexpr bool kStealthOff = false;

} // namespace

PedAnimation::PedAnimation(const NativeTable& table) noexcept
    : requestDict_(table.handlerFor(natives::kRequestAnimDict)),
      hasDict_(table.handlerFor(natives::kHasAnimDictLoaded)),
      playAnim_(table.handlerFor(natives::kTaskPlayAnim)),
      stealthMovement_(table.handlerFor(natives::kSetPedStealthMovement)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)) {}

PedAnimation::Clip PedAnimation::clipFor(shared::PedAction action) {
    switch (action) {
    case shared::PedAction::LightPunch:
        return Clip{kMeleeDictionary, kLightPunch};
    case shared::PedAction::HeavyPunch:
        return Clip{kMeleeDictionary, kHeavyPunch};
    case shared::PedAction::Kick:
        return Clip{kMeleeDictionary, kKick};
    case shared::PedAction::None:
        break;
    }

    return Clip{};
}

bool PedAnimation::ready(std::string_view dictionary) {
    if (requestDict_ == nullptr || hasDict_ == nullptr || dictionary.empty()) {
        return false;
    }

    // Имя набора уходит в игру строкой на C, а string_view о завершающем нуле
    // ничего не обещает. Все имена здесь — литералы, и ноль у них есть, но
    // полагаться на это молча нельзя: стоит кому-то передать сюда обрезок чужой
    // строки, и игра прочитает память за её концом.
    const std::string name{dictionary};

    invokeNative<void>(requestDict_, name.c_str());

    if (invokeNative<bool>(hasDict_, name.c_str())) {
        return true;
    }

    if (gameTimer_ == nullptr || complained_.contains(dictionary)) {
        return false;
    }

    const auto now = invokeNative<std::int32_t>(gameTimer_);
    const auto [entry, added] = requestedAt_.try_emplace(dictionary, now);

    if (!added && now - entry->second >= kLoadPatience) {
        spdlog::warn("набор движений \"{}\" не загружается — чужие игроки не покажут удары",
                     dictionary);
        complained_.insert(dictionary);
    }

    return false;
}

void PedAnimation::applyPosture(int ped, std::uint32_t flags, std::uint32_t previous) const {
    if (ped == 0 || stealthMovement_ == nullptr) {
        return;
    }

    const bool crouching = shared::has(flags, shared::PlayerFlag::Crouching);

    if (crouching == shared::has(previous, shared::PlayerFlag::Crouching)) {
        return;
    }

    // Последний довод — имя набора движений для крадущейся походки. Пустая
    // строка означает «взять обычный», и другого нам не нужно: своей походки мы
    // персонажу не придумываем, а повторяем ту, которой идёт его хозяин.
    invokeNative<void>(stealthMovement_, ped, crouching ? kStealthOn : kStealthOff, "");
}

bool PedAnimation::play(int ped, shared::PedAction action) {
    if (ped == 0 || playAnim_ == nullptr || action == shared::PedAction::None) {
        return false;
    }

    const Clip clip = clipFor(action);

    if (!ready(clip.dictionary)) {
        return false;
    }

    const std::string dictionary{clip.dictionary};
    const std::string name{clip.name};

    // Последние три довода — вокруг каких осей персонажу разрешено смещаться за
    // движением. Все запрещены: смещение задаём мы, снимками, и позволить
    // движению возить персонажа значило бы бороться с ним каждый кадр.
    invokeNative<void>(playAnim_, ped, dictionary.c_str(), name.c_str(), kBlendIn, kBlendOut,
                       kUntilDone, kUpperBodyOverlay, kNormalRate, false, false, false);

    return true;
}

} // namespace oxymp::client::game
