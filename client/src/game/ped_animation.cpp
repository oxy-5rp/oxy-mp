#include "ped_animation.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <oxymp/shared/math/joaat.hpp>

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

/// Признаки проигрывания, числами самой игры (eAnimationFlags).
///
/// Шестнадцать — «только верхняя часть тела»: персонаж бьёт, не переставая при
/// этом идти. Тридцать два — «не отнимать управление телом»: без него движение
/// становится единственным, что персонаж делает, и наша задача ходьбы с ним
/// спорит.
///
/// Стояло здесь `8 | 1`, и оба числа были неверны. Восьмёрки среди признаков
/// игры нет вовсе, а всё, что меньше шестнадцати, означает «всем телом», — то
/// есть удар отыгрывался не поверх походки, а вместо неё. Единица же означает
/// не «оставить позу», как было написано рядом, а «повторять без конца»: удар,
/// начатый стоящим человеком, шёл по кругу до тех пор, пока его не отменяла
/// какая-нибудь другая задача. У стоящего отменять было нечему.
constexpr int kAnimUpperBody = 16;
constexpr int kAnimKeepControl = 32;
constexpr int kUpperBodyOverlay = kAnimUpperBody | kAnimKeepControl;

/// Скорость проигрывания. Единица — обычная.
constexpr float kNormalRate = 1.0F;

/// Сколько ждать загрузки набора, прежде чем счесть его несуществующим.
constexpr int kLoadPatience = 5000;

/// Крадётся ли персонаж, в нумерации игры.
constexpr bool kStealthOn = true;
constexpr bool kStealthOff = false;

/// То же самое, каким его возвращает GET_PED_STEALTH_MOVEMENT: числом, а не
/// признаком. Отдельной величиной, потому что смешивать в сравнении число с
/// признаком — предупреждение сборки, а не мелочь слога.
constexpr int kStealthAnswerOn = 1;

/// Состояния движения игры, названные её же именами.
///
/// Хеш считается здесь, а не берётся у GET_HASH_KEY, и это не экономия вызова:
/// имена состояний — часть самой игры, они не меняются между сборками, а joaat
/// у нас общий с сервером и сверен с игрой разряд в разряд.
constexpr std::uint32_t kIdleState = shared::joaat("motionstate_idle");
constexpr std::uint32_t kSwimmingState = shared::joaat("motionstate_swimming");
constexpr std::uint32_t kDivingState = shared::joaat("motionstate_diving_swim");
constexpr std::uint32_t kParachutingState = shared::joaat("motionstate_parachuting");

/// Как далеко кукла стреляет из окна машины, в метрах.
constexpr float kDriveByRange = 300.0F;

/// Меткость куклы. Ноль — не попадать вовсе.
///
/// Попадания считает стрелявший у себя и присылает их отдельным сообщением;
/// попадающая по-настоящему кукла била бы второй раз по тому, кто уже посчитан.
constexpr int kNoAccuracy = 0;

/// Способ стрельбы. FIRING_PATTERN_FULL_AUTO — «жать на спуск, пока сказано».
constexpr std::uint32_t kFiringPatternFullAuto = 0xC6EE6B4CU;

} // namespace

PedAnimation::PedAnimation(const NativeTable& table) noexcept
    : requestDict_(table.handlerFor(natives::kRequestAnimDict)),
      startScenario_(table.handlerFor(natives::kTaskStartScenarioInPlace)),
      hasDict_(table.handlerFor(natives::kHasAnimDictLoaded)),
      playAnim_(table.handlerFor(natives::kTaskPlayAnim)),
      stealthMovement_(table.handlerFor(natives::kSetPedStealthMovement)),
      getStealth_(table.handlerFor(natives::kGetPedStealthMovement)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)),
      clearTasks_(table.handlerFor(natives::kClearPedTasks)),
      taskJump_(table.handlerFor(natives::kTaskJump)),
      taskClimb_(table.handlerFor(natives::kTaskClimb)),
      taskReload_(table.handlerFor(natives::kTaskReloadWeapon)),
      taskCover_(table.handlerFor(natives::kTaskStayInCover)),
      taskDriveBy_(table.handlerFor(natives::kTaskDriveBy)),
      taskVehicleAim_(table.handlerFor(natives::kTaskVehicleAimAtCoord)),
      motionState_(table.handlerFor(natives::kForcePedMotionState)) {}

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
    const auto [entry, added] = requestedAt_.try_emplace(name, now);

    if (!added && now - entry->second >= kLoadPatience) {
        spdlog::warn("animation dictionary \"{}\" does not load: the animation will not show",
                     dictionary);
        complained_.insert(name);
    }

    return false;
}

void PedAnimation::applyPosture(int ped, std::uint32_t flags, std::uint32_t previous) const {
    if (ped == 0) {
        return;
    }

    // Присед — положение тела: длится, пока признак стоит, и сверяется с тем,
    // что у персонажа на самом деле, а не с прошлым снимком.
    //
    // Прежде он задавался ровно на переходе признака, и этого было мало.
    // Присед сбрасывает у персонажа всякая зачистка задач — а она случается и
    // при подъёме после смерти, и при остановке, и при посадке в машину, — и
    // сброшенный, он не возвращался до тех пор, пока хозяин не присядет заново.
    // То есть, скорее всего, не возвращался вовсе.
    if (stealthMovement_ != nullptr) {
        const bool crouching = shared::has(flags, shared::PlayerFlag::Crouching);

        const bool crouched = getStealth_ != nullptr
                                  ? invokeNative<int>(getStealth_, ped) == kStealthAnswerOn
                                  : shared::has(previous, shared::PlayerFlag::Crouching);

        if (crouching != crouched) {
            // Последний довод — имя набора движений для крадущейся походки.
            // Пустая строка означает «взять обычный», и другого нам не нужно:
            // своей походки мы персонажу не придумываем, а повторяем ту,
            // которой идёт его хозяин.
            invokeNative<void>(stealthMovement_, ped, crouching ? kStealthOn : kStealthOff, "");
        }
    }

    // Начался ли признак ровно в этом кадре.
    const auto begun = [flags, previous](shared::PlayerFlag flag) {
        return shared::has(flags, flag) && !shared::has(previous, flag);
    };

    // Состояние движения. Навязывается только то, чего игра не выведет сама:
    // парашют у куклы не раскроется никогда — парашюта у неё нет и не будет, —
    // а плавание и погружение она выводит из воды, но выводит не сразу, и
    // названное прямо оно начинается в тот же кадр, что и у хозяина.
    if (motionState_ != nullptr) {
        const auto force = [this, ped](std::uint32_t state) {
            // Три последних довода: не ждать окончания нынешнего движения, не
            // сбрасывать скорость и не перезапускать состояние, если оно уже то
            // самое. Так же зовут его и скрипты игры.
            invokeNative<bool>(motionState_, ped, state, false, false, false);
        };

        if (begun(shared::PlayerFlag::Parachuting)) {
            force(kParachutingState);
        } else if (begun(shared::PlayerFlag::Diving)) {
            force(kDivingState);
        } else if (begun(shared::PlayerFlag::Swimming)) {
            force(kSwimmingState);
        } else if (shared::has(previous, shared::PlayerFlag::Parachuting) &&
                   !shared::has(flags, shared::PlayerFlag::Parachuting)) {
            // Приземлился. Состояние приходится снимать явно: навязанное, оно
            // само не кончается, и персонаж остался бы висеть в позе парашюта
            // посреди тротуара.
            force(kIdleState);
        }
    }

    // Короткие движения — задачами и ровно на переходе. Заказанные каждый кадр,
    // они начинались бы заново до того, как успеют начаться.
    if (taskJump_ != nullptr && begun(shared::PlayerFlag::Jumping)) {
        invokeNative<void>(taskJump_, ped, true);
    }

    // Лазание и перемах — одна задача: игра сама решает, за что тут можно
    // ухватиться, а различаются они только высотой препятствия.
    if (taskClimb_ != nullptr &&
        (begun(shared::PlayerFlag::Climbing) || begun(shared::PlayerFlag::Vaulting))) {
        invokeNative<void>(taskClimb_, ped, true);
    }

    if (taskReload_ != nullptr && begun(shared::PlayerFlag::Reloading)) {
        invokeNative<void>(taskReload_, ped, true);
    }

    if (taskCover_ != nullptr && begun(shared::PlayerFlag::InCover)) {
        // Задача сама ищет ближайшее укрытие. Не найдёт — не случится ничего, и
        // это верное поведение: у нас персонаж стоит там же, где хозяин, а
        // значит укрытие рядом либо есть у обоих, либо нет ни у кого.
        invokeNative<void>(taskCover_, ped);
    }

    // Укрытие приходится снимать явно, и оно здесь единственное такое.
    //
    // Прыжок, лазание и перезарядка кончаются сами: отыграли своё движение — и
    // задачи не стало. Задача укрытия не кончается никогда, на то она и «сиди в
    // укрытии». Пока её не снять, персонаж остаётся прижатым к стене — а
    // хозяин, вышедший из укрытия и стоящий на месте, ничем её снять и не мог:
    // задачу ходьбы ему не выдают, снимать её некому. Со стороны это выглядело
    // так, что вставший из-за угла человек навсегда остаётся за углом.
    if (clearTasks_ != nullptr && !shared::has(flags, shared::PlayerFlag::InCover) &&
        shared::has(previous, shared::PlayerFlag::InCover)) {
        invokeNative<void>(clearTasks_, ped);
    }
}

void PedAnimation::applyDriveBy(int ped, const shared::Vec3& target, bool firing) const {
    if (ped == 0) {
        return;
    }

    // Только целится. Стреляет за неё присланный выстрел — тот самый, что
    // прилетает отдельным сообщением и уходит пулей из её же руки. Дай мы ей
    // вдобавок стрелять самой, очередь вышла бы двойной, и половина её летела бы
    // не туда, куда целился хозяин, а куда попадёт кукла со своей меткостью.
    //
    // Ровно это правило уже есть у пешей стрельбы, и здесь его недоставало.
    if (!firing && taskVehicleAim_ != nullptr) {
        invokeNative<void>(taskVehicleAim_, ped, target.x, target.y, target.z);
        return;
    }

    if (taskDriveBy_ == nullptr) {
        return;
    }

    // Ни цели-персонажа, ни цели-машины: стреляем по точке. Кто там на самом
    // деле, решает не наша сторона — попадания считает стрелявший у себя.
    //
    // Меткость нулевая, и это не оплошность: кукла стреляет ради вида, а урон
    // приходит отдельным сообщением. Попадающая по-настоящему, она била бы
    // второй раз по тому, кто уже посчитан.
    invokeNative<void>(taskDriveBy_, ped, 0, 0, target.x, target.y, target.z, kDriveByRange,
                       kNoAccuracy, true, kFiringPatternFullAuto);
}

bool PedAnimation::playNamed(int ped, const shared::PlayerAnimation& animation) {
    if (ped == 0) {
        return false;
    }

    // Сценарий — раньше движения и вместо него: у alt:V это разные вызовы, и
    // одновременно их не просят. Набор движений ему не нужен вовсе — сценарий
    // игра держит у себя целиком, — и потому ждать загрузки здесь нечего.
    if (!animation.scenario.empty()) {
        if (startScenario_ == nullptr) {
            return false;
        }

        // Доводы: задержка перед началом и играть ли вступление. Задержки нет,
        // вступление есть — персонаж должен опуститься на скамейку на глазах, а
        // не оказаться на ней сидящим.
        invokeNative<void>(startScenario_, ped, animation.scenario.c_str(), 0, true);

        return true;
    }

    if (playAnim_ == nullptr || animation.dictionary.empty() || animation.name.empty()) {
        return false;
    }

    if (!ready(animation.dictionary)) {
        return false;
    }

    // Признаки проигрывания и запреты по осям — от ресурса, а не наши. У alt:V
    // это доводы `player.playAnimation`, и подставить вместо них свои значило бы
    // решать за режим, как выглядит его отыгрыш.
    invokeNative<void>(playAnim_, ped, animation.dictionary.c_str(), animation.name.c_str(),
                       animation.blendIn, animation.blendOut, animation.duration,
                       animation.flags, animation.playbackRate,
                       shared::has(animation.locks, shared::AnimationLock::X),
                       shared::has(animation.locks, shared::AnimationLock::Y),
                       shared::has(animation.locks, shared::AnimationLock::Z));

    return true;
}

void PedAnimation::clearTasks(int ped) const {
    if (ped == 0 || clearTasks_ == nullptr) {
        return;
    }

    invokeNative<void>(clearTasks_, ped);
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
