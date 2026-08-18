#include "remote_players.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include "../interpolation.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace oxymp::client::game {
namespace {

/// Модель, которой показываются чужие игроки.
///
/// Та же сетевая заготовка, что и у местного игрока: пока сервер не рассылает
/// внешность, все выглядят одинаково, и пусть уж одинаково выглядят как игроки,
/// а не как случайный прохожий.
constexpr const char* kRemoteModel = "mp_m_freemode_01";

/// Каноническое значение joaat от имени модели — сверка для GET_HASH_KEY.
constexpr std::uint32_t kRemoteModelHash = 0x705E61F2;

/// Тип персонажа для CREATE_PED. Четвёрка — обычный горожанин.
constexpr int kCivilianType = 4;

/// Дальность, с которой персонаж остаётся видимым, в метрах.
///
/// Задаётся явно и с запасом: игра сама выбирает её по назначению персонажа и
/// для случайного прохожего берёт небольшую, а чужой игрок должен быть виден
/// издалека — иначе он появляется прямо перед носом.
constexpr int kLodDistance = 500;

/// Медленнее этого движение считается стоянием на месте, в метрах в секунду.
///
/// Порог нужен: скорость из снимка почти никогда не бывает ровно нулевой, и без
/// него стоящий игрок вечно переминался бы с ноги на ногу.
constexpr float kStandingSpeed = 0.3F;

/// Скорости, которым отвечают походки игры, в метрах в секунду.
///
/// Игра задаёт походку не скоростью, а числом от нуля до трёх: ноль — стоять,
/// единица — шаг, двойка — бег, тройка — бег во весь дух. Между ними она
/// смешивает движения сама, поэтому достаточно знать, какой скорости отвечает
/// каждое из трёх, и попасть в промежуток.
constexpr float kWalkSpeed = 1.4F;
constexpr float kRunSpeed = 3.5F;
constexpr float kSprintSpeed = 7.0F;

/// Числа походок, отвечающие скоростям выше.
constexpr float kStandBlend = 0.0F;
constexpr float kWalkBlend = 1.0F;
constexpr float kRunBlend = 2.0F;
constexpr float kSprintBlend = 3.0F;

/// Как далеко впереди ставится цель задачи движения, в метрах.
///
/// Задача нужна не чтобы дойти, а чтобы шли ноги, поэтому цель ей назначается
/// заведомо недостижимая: дойдя, персонаж встал бы, и походка кончилась бы
/// вместе с задачей.
constexpr float kAimAhead = 30.0F;

/// Насколько должно развернуться направление движения, чтобы выдать задачу
/// заново, в метрах расхождения цели.
constexpr float kRetaskDistance = 5.0F;

/// Через сколько задача выдаётся заново, даже если направление не менялось.
constexpr int kTaskRefresh = 2000;

/// Сколько задача живёт, в миллисекундах. Заведомо больше срока обновления.
constexpr int kTaskTimeout = 10000;

/// Насколько персонажу разрешено проскользить к точке в конце пути.
constexpr float kNoSliding = 0.0F;

/// Насколько персонаж должен разойтись со снимком, чтобы его переставить рывком.
///
/// Три метра — это уже не «чуть отстал», а «стоит не там»: на таком расхождении
/// стрелять по нему бессмысленно, и лучше рывок, чем стойкая ложь.
constexpr float kSnapDistance = 3.0F;

/// Во сколько раз сокращать расхождение за секунду, пока оно невелико.
///
/// Персонаж не переставляется в точку снимка — он подводится к ней, оставаясь
/// физическим телом: упирается в стены, сдвигает предметы, принимает удары.
///
/// За секунду, а не за кадр, и это не придирка: доля за кадр съедает расхождение
/// вдвое быстрее при шестидесяти кадрах, чем при тридцати, и один и тот же игрок
/// у двух зрителей идёт по-разному. Такую же ошибку здесь уже ловили — на
/// повороте, который доходил до нужного угла тогда, когда хозяин смотрел уже в
/// другую сторону. Число подобрано так, чтобы при обычных шестидесяти кадрах
/// выходила прежняя четверть расхождения за кадр.
constexpr float kCorrectionRate = 17.0F;

/// Ниже этого расхождения не поправляем вовсе.
///
/// Иначе персонаж вечно подрагивает: снимки приходят с погрешностью в
/// сантиметры, и гоняться за ней — значит трясти его на месте.
constexpr float kIgnoredError = 0.15F;

/// Сколько держать тело обмякшим, в миллисекундах.
constexpr int kRagdollDuration = 4000;

/// Вид рэгдолла. Ноль — обычный, тело падает под собственным весом.
constexpr int kRagdollKind = 0;

/// Сколько патронов выдаётся вместе с оружием.
///
/// Стрелять по-настоящему персонаж не будет — попадания считает хозяин, — но
/// без патронов он не отыграет ни выстрела, а щёлкать пустым магазином будет.
constexpr int kFullAmmo = 250;

/// Сколько живёт задача прицела или выстрела, в миллисекундах.
///
/// Короткая намеренно: прицел и стрельба кончаются в тот же миг, что и у
/// хозяина, а не длятся сами по себе. Задача выдаётся каждый кадр заново, и
/// стоит ей перестать выдаваться — персонаж опускает оружие.
constexpr int kAimTaskDuration = 200;

/// Способ стрельбы. FIRING_PATTERN_FULL_AUTO — «жать на спуск, пока сказано».
constexpr std::uint32_t kFiringPatternFullAuto = 0xC6EE6B4CU;

/// Признак немедленного выхода из машины: без открывания двери и без анимации.
constexpr int kWarpOutOfVehicle = 16;

/// Признак обычного входа в машину: подойти, открыть дверь, сесть.
///
/// Ровно то, чего не хватало: раньше персонаж возникал на сиденье, и посадки со
/// стороны видно не было вовсе.
constexpr int kNormalEntry = 1;

/// С какой скоростью персонаж идёт к машине. Двойка — бегом.
constexpr float kRunToVehicle = 2.0F;

/// Сколько отводится задаче входа, в миллисекундах.
///
/// Не выдержала — сажаем рывком: дверь могло заклинить о стену, а хозяин к
/// этому времени уже едет, и оставить его персонажа снаружи хуже, чем показать
/// некрасивую посадку.
constexpr std::int32_t kEntryPatience = 3000;

/// Признаки задачи «иди туда, целясь вон туда»: не стрелять на ходу, не искать
/// дорогу вокруг препятствий и не сводить прицел мгновенно.
constexpr bool kNoShootingOnTheWay = false;
constexpr float kNoTargetRadius = 0.0F;
constexpr float kNoSlowdown = 0.0F;
constexpr bool kNoNavigation = false;
constexpr int kNoNavigationFlags = 0;
constexpr bool kNoInstantAim = false;

float length(const shared::Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

float distanceBetween(const shared::Vec3& from, const shared::Vec3& to) {
    return length(shared::Vec3{to.x - from.x, to.y - from.y, to.z - from.z});
}

/// Скорость, переведённая в число походки игры.
float blendForSpeed(float speed) {
    if (speed < kStandingSpeed) {
        return kStandBlend;
    }
    if (speed < kWalkSpeed) {
        return std::lerp(kStandBlend, kWalkBlend, speed / kWalkSpeed);
    }
    if (speed < kRunSpeed) {
        return std::lerp(kWalkBlend, kRunBlend, (speed - kWalkSpeed) / (kRunSpeed - kWalkSpeed));
    }
    if (speed < kSprintSpeed) {
        return std::lerp(kRunBlend, kSprintBlend, (speed - kRunSpeed) / (kSprintSpeed - kRunSpeed));
    }
    return kSprintBlend;
}

/// Целится ли игрок — то есть нужно ли разводить взгляд и направление движения.
bool aiming(const shared::PlayerState& state) {
    return shared::has(state.flags, shared::PlayerFlag::Aiming) ||
           shared::has(state.flags, shared::PlayerFlag::Shooting);
}

} // namespace

RemotePlayers::RemotePlayers(const NativeTable& table, const Vehicles& vehicles) noexcept
    : vehicles_(vehicles),
      animation_(table),
      appearance_(table),
      hashKey_(table.handlerFor(natives::kGetHashKey)),
      requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      createPed_(table.handlerFor(natives::kCreatePed)),
      deletePed_(table.handlerFor(natives::kDeletePed)),
      doesExist_(table.handlerFor(natives::kDoesEntityExist)),
      setCoords_(table.handlerFor(natives::kSetEntityCoordsNoOffset)),
      setHeading_(table.handlerFor(natives::kSetEntityHeading)),
      desiredHeading_(table.handlerFor(natives::kSetPedDesiredHeading)),
      defaultVariation_(table.handlerFor(natives::kSetPedDefaultComponentVariation)),
      blockEvents_(table.handlerFor(natives::kSetBlockingOfNonTemporaryEvents)),
      asMissionEntity_(table.handlerFor(natives::kSetEntityAsMissionEntity)),
      canBeTargetted_(table.handlerFor(natives::kSetPedCanBeTargetted)),
      diesWhenInjured_(table.handlerFor(natives::kSetPedDiesWhenInjured)),
      invincible_(table.handlerFor(natives::kSetEntityInvincible)),
      lodDistance_(table.handlerFor(natives::kSetEntityLodDist)),
      getCoords_(table.handlerFor(natives::kGetEntityCoords)),
      taskGoTo_(table.handlerFor(natives::kTaskGoStraightToCoord)),
      taskGoToAiming_(table.handlerFor(natives::kTaskGoToCoordWhileAimingAtCoord)),
      moveBlend_(table.handlerFor(natives::kSetPedDesiredMoveBlendRatio)),
      getHealth_(table.handlerFor(natives::kGetEntityHealth)),
      setHealth_(table.handlerFor(natives::kSetEntityHealth)),
      setArmour_(table.handlerFor(natives::kSetPedArmour)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)),
      clearTasks_(table.handlerFor(natives::kClearPedTasksImmediately)),
      damagedBy_(table.handlerFor(natives::kHasEntityBeenDamagedByEntity)),
      clearDamage_(table.handlerFor(natives::kClearEntityLastDamageEntity)),
      giveWeapon_(table.handlerFor(natives::kGiveWeaponToPed)),
      setWeapon_(table.handlerFor(natives::kSetCurrentPedWeapon)),
      setAmmo_(table.handlerFor(natives::kSetPedAmmo)),
      taskAim_(table.handlerFor(natives::kTaskAimGunAtCoord)),
      taskShoot_(table.handlerFor(natives::kTaskShootAtCoord)),
      setRagdoll_(table.handlerFor(natives::kSetPedToRagdoll)),
      canRagdoll_(table.handlerFor(natives::kSetPedCanRagdoll)),
      setIntoVehicle_(table.handlerFor(natives::kSetPedIntoVehicle)),
      enterVehicle_(table.handlerFor(natives::kTaskEnterVehicle)),
      leaveVehicle_(table.handlerFor(natives::kTaskLeaveVehicle)),
      isInVehicle_(table.handlerFor(natives::kIsPedInVehicle)) {}

RemotePlayers::~RemotePlayers() {
    // Персонажей здесь уже не убрать: разрушение приходится на выгрузку модуля,
    // а она случается вне скриптового тика, где нативы звать нельзя. Убирать их
    // положено вызовом clear до этого момента.
}

bool RemotePlayers::ready() const noexcept {
    return hashKey_ != nullptr && requestModel_ != nullptr && hasModelLoaded_ != nullptr &&
           createPed_ != nullptr && deletePed_ != nullptr && doesExist_ != nullptr &&
           setCoords_ != nullptr && setHeading_ != nullptr && defaultVariation_ != nullptr &&
           blockEvents_ != nullptr && asMissionEntity_ != nullptr && canBeTargetted_ != nullptr &&
           diesWhenInjured_ != nullptr && invincible_ != nullptr && lodDistance_ != nullptr &&
           getCoords_ != nullptr && taskGoTo_ != nullptr && moveBlend_ != nullptr &&
           getHealth_ != nullptr && setHealth_ != nullptr && gameTimer_ != nullptr &&
           clearTasks_ != nullptr;
}

std::uint32_t RemotePlayers::model() {
    if (model_ != 0) {
        return model_;
    }

    model_ = invokeNative<std::uint32_t>(hashKey_, kRemoteModel);

    if (model_ != kRemoteModelHash) {
        spdlog::error("GET_HASH_KEY вернул {:#010x} вместо {:#010x} — чужих игроков не показать",
                      model_, kRemoteModelHash);
        model_ = 0;
    }

    return model_;
}

int RemotePlayers::spawn(const RemotePlayerView& player) {
    const std::uint32_t wanted = model();
    if (wanted == 0) {
        return 0;
    }

    invokeNative<void>(requestModel_, wanted);
    if (!invokeNative<bool>(hasModelLoaded_, wanted)) {
        return 0;
    }

    const int ped =
        invokeNative<int>(createPed_, kCivilianType, wanted, player.state.position.x,
                          player.state.position.y, player.state.position.z, player.state.heading,
                          false, false);
    if (ped == 0) {
        return 0;
    }

    invokeNative<void>(defaultVariation_, ped, false);

    // Персонаж принадлежит нам, а не миру: иначе игра вправе убрать его как
    // лишнего ровно тогда, когда игрок отвернётся.
    invokeNative<void>(asMissionEntity_, ped, true, true);

    // Своей жизни у него нет: он не должен ни разбегаться от выстрелов, ни
    // ввязываться в драки, ни звать полицию. Всё, что он делает, приходит от
    // хозяина.
    invokeNative<void>(blockEvents_, ped, true);

    // По нему попадают по-настоящему. Умереть от этого он всё равно не может —
    // здоровье ему каждый кадр возвращает хозяин, — но само попадание мы
    // замечаем и отправляем серверу, а тот уже жертве.
    invokeNative<void>(canBeTargetted_, ped, true);
    invokeNative<void>(invincible_, ped, false);

    // Умирать сам он при этом не должен: смерть решает хозяин.
    invokeNative<void>(diesWhenInjured_, ped, false);

    invokeNative<void>(lodDistance_, ped, kLodDistance);

    if (canRagdoll_ != nullptr) {
        invokeNative<void>(canRagdoll_, ped, true);
    }

    // Одежда надевается сразу за созданием, а не следующим кадром: иначе игрок
    // на мгновение появится голым, и мгновение это заметно — персонаж выходит
    // из-за поворота уже одетым или не выходит вовсе.
    if (const auto known = looks_.find(player.id); known != looks_.end()) {
        appearance_.apply(ped, known->second);
    }

    spdlog::debug("игрок {} ({}) показан персонажем {}", player.id, player.nickname, ped);
    return ped;
}

void RemotePlayers::dress(shared::PlayerId player, const shared::PlayerAppearance& appearance) {
    looks_[player] = appearance;

    // Персонажа может ещё не быть: внешность идёт надёжным каналом и обгоняет
    // снимки, а создаётся персонаж по снимку. Тогда её наденут при создании.
    if (const auto puppet = puppets_.find(player); puppet != puppets_.end()) {
        appearance_.apply(puppet->second.ped, appearance);
    }
}

void RemotePlayers::remove(int ped) const {
    if (ped == 0) {
        return;
    }

    // Ссылка на номер, а не сам номер: натив обнуляет его у вызывающего, и
    // передать значение — значит оставить игре чужую копию.
    int handle = ped;

    NativeContext context;
    context.push(&handle);
    deletePed_(context.address());
}

void RemotePlayers::sync(const std::vector<RemotePlayerView>& players, int localPed) {
    if (!ready()) {
        return;
    }

    // Сперва ушедшие: их персонажи занимают место в мире и в списке игры, и
    // держать их лишний кадр незачем.
    for (auto it = puppets_.begin(); it != puppets_.end();) {
        const bool present = std::any_of(players.begin(), players.end(),
                                         [&it](const RemotePlayerView& player) {
                                             return player.id == it->first;
                                         });

        if (present) {
            ++it;
            continue;
        }

        spdlog::debug("игрок {} ушёл, персонаж {} убран", it->first, it->second.ped);
        remove(it->second.ped);
        it = puppets_.erase(it);
    }

    const auto now = invokeNative<std::int32_t>(gameTimer_);

    // Длительность кадра — один раз на всех: часы у игры одни, а персонажей
    // бывает три десятка. Первый кадр сравнивать не с чем, и отрицательная
    // длительность после переполнения счётчика — тоже не длительность: и то и
    // другое означает «времени не прошло».
    const float seconds =
        framedAt_ != 0 && now > framedAt_ ? static_cast<float>(now - framedAt_) / 1000.0F : 0.0F;

    framedAt_ = now;

    for (const RemotePlayerView& player : players) {
        const auto known = puppets_.find(player.id);

        if (known == puppets_.end()) {
            if (const int ped = spawn(player); ped != 0) {
                puppets_.emplace(player.id,
                                 Puppet{.ped = ped,
                                        .appliedHealth = player.state.health,
                                        .actionSequence = player.state.actionSequence});
            }
            continue;
        }

        Puppet& puppet = known->second;

        // Персонажа могло не стать помимо нас: игра вправе убрать то, что
        // считает лишним, а обращение к исчезнувшему — это вылет.
        if (!invokeNative<bool>(doesExist_, puppet.ped)) {
            spdlog::warn("персонаж игрока {} исчез, заводим заново", player.id);
            puppets_.erase(known);
            continue;
        }

        settleHealth(puppet, player, localPed);

        // Мёртвый никуда не идёт и никуда не смотрит: он лежит там, где упал.
        // Ставить ему положение значило бы возить труп по земле.
        if (!shared::has(player.state.flags, shared::PlayerFlag::Dead)) {
            ride(puppet, player, now);

            const bool riding = shared::has(player.state.flags, shared::PlayerFlag::InVehicle);
            const bool entering =
                shared::has(player.state.flags, shared::PlayerFlag::EnteringVehicle);

            // Залезающим распоряжается задача входа: она ведёт его к двери сама,
            // и вести его при этом ещё и снимками значит тянуть в две стороны.
            if (!riding && !entering) {
                walk(puppet, player, seconds, now);
                aim(puppet, player);
            }

            animation_.applyPosture(puppet.ped, player.state.flags, puppet.flags);
            act(puppet, player);
        }

        puppet.flags = player.state.flags;
    }
}

void RemotePlayers::settleHealth(Puppet& puppet, const RemotePlayerView& player,
                                 int localPed) const {
    const bool wasDead = shared::has(puppet.flags, shared::PlayerFlag::Dead);
    const bool dead = shared::has(player.state.flags, shared::PlayerFlag::Dead);

    const int current = invokeNative<int>(getHealth_, puppet.ped);

    // Попадание замечается по разнице между тем, что мы поставили в прошлый
    // кадр, и тем, что осталось сейчас. Проверка «кто ударил» обязательна: без
    // неё мы бы отправляли серверу и урон от чужой машины, и падение с высоты —
    // всё то, что хозяин уже посчитал у себя.
    if (onDamage_ && localPed != 0 && damagedBy_ != nullptr && !dead) {
        if (invokeNative<bool>(damagedBy_, puppet.ped, localPed, true)) {
            const int lost = puppet.appliedHealth - current;

            if (lost > 0) {
                onDamage_(player.id, static_cast<std::uint16_t>(lost), player.state.weapon);
            }

            if (clearDamage_ != nullptr) {
                invokeNative<void>(clearDamage_, puppet.ped);
            }
        }
    }

    // Здоровье возвращается к тому, что сказал хозяин. Он единственный, кто
    // знает его наверняка: у него настоящий игрок, а у нас его изображение.
    const int wanted = dead ? 0 : static_cast<int>(player.state.health);
    if (current != wanted) {
        invokeNative<void>(setHealth_, puppet.ped, wanted);
    }
    puppet.appliedHealth = wanted;

    // Броня видна со стороны: она рисуется на полосе над головой и решает,
    // сколько выстрелов персонаж выдержит, прежде чем по нему начнёт попадать
    // по-настоящему.
    if (setArmour_ != nullptr) {
        invokeNative<void>(setArmour_, puppet.ped, static_cast<int>(player.state.armour));
    }

    if (dead == wasDead) {
        return;
    }

    // Задачи снимаются и при смерти, и при подъёме: мертвецу они не нужны, а
    // поднявшемуся достанутся чужие, оставшиеся от того, кем он был до смерти.
    invokeNative<void>(clearTasks_, puppet.ped);
    puppet.taskedAt = 0;

    spdlog::debug("игрок {} {}", player.id, dead ? "погиб" : "снова жив");
}

void RemotePlayers::ride(Puppet& puppet, const RemotePlayerView& player, std::int32_t now) const {
    const bool riding = shared::has(player.state.flags, shared::PlayerFlag::InVehicle);
    const bool entering = shared::has(player.state.flags, shared::PlayerFlag::EnteringVehicle);

    if (!riding && !entering) {
        if (puppet.vehicleId == shared::kInvalidVehicleId) {
            return;
        }

        // Вышел. Высаживаем сразу, а не с открыванием двери: хозяин к этому
        // времени уже стоит на асфальте, и отыгрывать выход некогда.
        const int left = vehicles_.handleFor(puppet.vehicleId);
        puppet.vehicleId = shared::kInvalidVehicleId;
        puppet.seat = shared::kNoSeat;
        puppet.enteringSince = 0;

        if (left != 0 && leaveVehicle_ != nullptr) {
            invokeNative<void>(leaveVehicle_, puppet.ped, left, kWarpOutOfVehicle);
        } else {
            // Машины уже нет — её убрали вместе с тем, что о ней перестали
            // рассказывать. Персонажу остаётся только забыть свои задачи.
            invokeNative<void>(clearTasks_, puppet.ped);
        }
        return;
    }

    const int vehicle = vehicles_.handleFor(player.state.vehicleId);
    if (vehicle == 0) {
        // Машина ещё не создана: её модель могла не успеть загрузиться. Сажать
        // некуда, и это нормально — сядет в следующем кадре.
        return;
    }

    const int seat = static_cast<int>(player.state.seat);

    if (entering) {
        // Хозяин ещё лезет. Задача входа выдаётся один раз и дальше идёт сама:
        // персонаж подходит к двери, открывает её и садится — то самое, чего со
        // стороны не было видно вовсе, пока его пересаживали рывком.
        if (puppet.enteringSince != 0 || enterVehicle_ == nullptr) {
            return;
        }

        puppet.enteringSince = now;
        puppet.vehicleId = player.state.vehicleId;
        puppet.seat = player.state.seat;

        invokeNative<void>(enterVehicle_, puppet.ped, vehicle, kTaskTimeout, seat, kRunToVehicle,
                           kNormalEntry, 0);

        spdlog::debug("игрок {} лезет в машину {:#010x} на место {}", player.id,
                      player.state.vehicleId, seat);
        return;
    }

    // Хозяин уже сидит. Если наш персонаж тоже — вход удался, и трогать его
    // больше не нужно.
    const bool placed =
        puppet.vehicleId == player.state.vehicleId && puppet.seat == player.state.seat;

    // Спросить игру надёжнее, чем помнить самим: персонажа могло выбросить из
    // машины взрывом. Но если спросить нечем, память — единственное, что есть, и
    // считать по ней «не сидит» нельзя: мы сажали бы его рывком каждый кадр.
    const bool seated = isInVehicle_ != nullptr
                            ? invokeNative<bool>(isInVehicle_, puppet.ped, vehicle, false)
                            : placed;

    if (placed && seated) {
        puppet.enteringSince = 0;
        return;
    }

    // Вход был начат и ещё не кончился — дадим ему доиграть. Но не бесконечно:
    // дверь могло заклинить о стену, а хозяин уже едет.
    if (!seated && puppet.enteringSince != 0 && now - puppet.enteringSince < kEntryPatience) {
        return;
    }

    puppet.vehicleId = player.state.vehicleId;
    puppet.seat = player.state.seat;
    puppet.enteringSince = 0;

    if (setIntoVehicle_ != nullptr) {
        invokeNative<void>(setIntoVehicle_, puppet.ped, vehicle, seat);
    }

    spdlog::debug("игрок {} сел в машину {:#010x} на место {}", player.id, player.state.vehicleId,
                  seat);
}

void RemotePlayers::turn(const Puppet& puppet, const RemotePlayerView& player, bool moving) const {
    // Угол приходит уже посчитанным на это мгновение — тем же расчётом и на то
    // же время, что и положение. Доводить его здесь долей за кадр, как делалось
    // раньше, не нужно и вредно: доля за кадр означает, что скорость доворота
    // зависит от частоты кадров, а до нужного угла персонаж доходит уже тогда,
    // когда хозяин смотрит в другую сторону.
    if (!moving && setHeading_ != nullptr) {
        // Стоящему угол задаётся прямо: у него нет ни задачи, ни походки,
        // которые могли бы этот угол оспорить.
        invokeNative<void>(setHeading_, puppet.ped, player.state.heading);
        return;
    }

    if (desiredHeading_ != nullptr) {
        // Идущему — желаемое направление, а не мгновенное: игра доворачивает
        // персонажа сама, отыгрывая шаг ногами, вместо того чтобы вращать его
        // вокруг оси.
        invokeNative<void>(desiredHeading_, puppet.ped, player.state.heading);
    }
}

void RemotePlayers::steer(Puppet& puppet, const RemotePlayerView& player, float speed,
                          std::int32_t now) const {
    // Цель — далеко впереди по ходу движения. Дойти до неё персонаж не успеет,
    // и в этом весь смысл: задача не должна завершиться, иначе походка кончится
    // вместе с ней.
    const shared::Vec3 direction{player.state.velocity.x / speed, player.state.velocity.y / speed,
                                 player.state.velocity.z / speed};
    const shared::Vec3 target{player.state.position.x + direction.x * kAimAhead,
                              player.state.position.y + direction.y * kAimAhead,
                              player.state.position.z + direction.z * kAimAhead};

    const bool aims = aiming(player.state);

    const bool tasked = puppet.taskedAt != 0;
    const bool stale = tasked && now - puppet.taskedAt >= kTaskRefresh;
    const bool veered = tasked && distanceBetween(puppet.aim, target) >= kRetaskDistance;
    const bool switched = tasked && puppet.taskedAiming != aims;

    if (tasked && !stale && !veered && !switched) {
        return;
    }

    puppet.aim = target;
    puppet.taskedAt = now;
    puppet.taskedAiming = aims;

    // Целящегося ведёт другая задача, и в этом суть. Обычная задача движения
    // заодно решает, куда персонаж смотрит, — а целящийся ходит боком и назад,
    // глядя туда, куда наведено оружие. Развести направление движения и
    // направление взгляда умеет только эта.
    if (aims && taskGoToAiming_ != nullptr) {
        invokeNative<void>(taskGoToAiming_, puppet.ped, target.x, target.y, target.z,
                           player.state.aimAt.x, player.state.aimAt.y, player.state.aimAt.z,
                           blendForSpeed(speed), kNoShootingOnTheWay, kNoTargetRadius, kNoSlowdown,
                           kNoNavigation, kNoNavigationFlags, kNoInstantAim,
                           kFiringPatternFullAuto);
        return;
    }

    // Скорость задаче отдаётся та же, что и походке: разойдись они, персонаж
    // поехал бы по земле — ноги отыгрывали бы одну скорость, а перемещала бы
    // его другая.
    invokeNative<void>(taskGoTo_, puppet.ped, target.x, target.y, target.z, speed, kTaskTimeout,
                       player.state.heading, kNoSliding);
}

void RemotePlayers::walk(Puppet& puppet, const RemotePlayerView& player, float seconds,
                         std::int32_t now) const {
    NativeContext coords;
    coords.push(puppet.ped);
    coords.push(true);
    getCoords_(coords.address());

    const shared::Vec3 actual{coords.result<float>(0), coords.result<float>(1),
                              coords.result<float>(2)};

    const float error = distanceBetween(actual, player.state.position);

    if (error > kSnapDistance) {
        // Разошлись настолько, что плавно уже не свести. Признаки натива: не
        // сдвигать по осям, не искать землю, не расталкивать соседей.
        invokeNative<void>(setCoords_, puppet.ped, player.state.position.x,
                           player.state.position.y, player.state.position.z, false, false, false);
    } else if (error > kIgnoredError) {
        // Понемногу, и «понемногу» считается от времени, а не от кадров: доля за
        // кадр съедала бы расхождение вдвое быстрее при шестидесяти кадрах, чем
        // при тридцати, и один и тот же игрок у двух зрителей шёл бы по-разному.
        const float share = interpolation::catchUp(kCorrectionRate, seconds);

        invokeNative<void>(setCoords_, puppet.ped,
                           std::lerp(actual.x, player.state.position.x, share),
                           std::lerp(actual.y, player.state.position.y, share),
                           std::lerp(actual.z, player.state.position.z, share), false,
                           false, false);
    }

    // Рэгдолл заказывается на переходе, а не каждый кадр: заказанный повторно,
    // он поднимает тело и роняет заново, и вместо падения выходит судорога.
    const bool ragdoll = shared::has(player.state.flags, shared::PlayerFlag::Ragdoll);
    if (ragdoll && !shared::has(puppet.flags, shared::PlayerFlag::Ragdoll) &&
        setRagdoll_ != nullptr) {
        invokeNative<void>(setRagdoll_, puppet.ped, kRagdollDuration, kRagdollDuration,
                           kRagdollKind, true, true, false);
        puppet.taskedAt = 0;
    }

    // Обмякшее тело не ходит и не поворачивается — им распоряжается физика.
    if (ragdoll) {
        return;
    }

    const float speed = length(player.state.velocity);
    const bool moving = speed >= kStandingSpeed;

    turn(puppet, player, moving);

    invokeNative<void>(moveBlend_, puppet.ped, blendForSpeed(speed));

    if (!moving) {
        // Стоящему задача движения не нужна: без неё он просто стоит, а с ней
        // топтался бы на месте, пытаясь дойти до цели, которой мы его не
        // снабдили.
        if (puppet.taskedAt != 0) {
            invokeNative<void>(clearTasks_, puppet.ped);
            puppet.taskedAt = 0;
        }
        return;
    }

    steer(puppet, player, speed, now);
}

void RemotePlayers::aim(Puppet& puppet, const RemotePlayerView& player) const {
    if (giveWeapon_ == nullptr || setWeapon_ == nullptr) {
        return;
    }

    // Оружие выдаётся только при смене: выданное заново каждый кадр, оно
    // перезаряжается без конца и сбрасывает позу до того, как та начнётся.
    if (player.state.weapon != puppet.weapon) {
        puppet.weapon = player.state.weapon;

        if (puppet.weapon != 0) {
            // Боезапас — тот, что у хозяина, а не полный, как было раньше.
            // Разница видна в перестрелке: с полным магазином кукла продолжала
            // бы стрелять ровно тогда, когда хозяин перезаряжается, и обмен
            // выстрелами выглядел бы у двоих по-разному.
            invokeNative<void>(giveWeapon_, puppet.ped, puppet.weapon,
                               static_cast<int>(player.state.ammo), false, true);
            invokeNative<void>(setWeapon_, puppet.ped, puppet.weapon, true);

            puppet.ammo = player.state.ammo;
        }
    } else if (puppet.weapon != 0 && player.state.ammo != puppet.ammo && setAmmo_ != nullptr) {
        // Патроны догоняются отдельно и без выдачи оружия: выдача сбрасывает
        // позу, а патроны меняются каждым выстрелом.
        invokeNative<void>(setAmmo_, puppet.ped, puppet.weapon,
                           static_cast<int>(player.state.ammo));

        puppet.ammo = player.state.ammo;
    }

    if (!aiming(player.state)) {
        return;
    }

    // Идущего и целящегося ведёт задача движения — она же держит и прицел.
    // Выдавать ему вдобавок задачу прицела значило бы отменять первую.
    if (length(player.state.velocity) >= kStandingSpeed && taskGoToAiming_ != nullptr) {
        return;
    }

    const bool shooting = shared::has(player.state.flags, shared::PlayerFlag::Shooting);

    // Задача выдаётся коротким сроком и каждый кадр: и прицел, и стрельба — это
    // состояния, которые кончаются в тот же миг, что и у хозяина, а не длятся
    // сами по себе.
    if (shooting && taskShoot_ != nullptr) {
        invokeNative<void>(taskShoot_, puppet.ped, player.state.aimAt.x, player.state.aimAt.y,
                           player.state.aimAt.z, kAimTaskDuration, kFiringPatternFullAuto);
        return;
    }

    if (taskAim_ != nullptr) {
        invokeNative<void>(taskAim_, puppet.ped, player.state.aimAt.x, player.state.aimAt.y,
                           player.state.aimAt.z, kAimTaskDuration, false, false);
    }
}

void RemotePlayers::act(Puppet& puppet, const RemotePlayerView& player) {
    if (player.state.action == shared::PedAction::None ||
        player.state.actionSequence == puppet.actionSequence) {
        return;
    }

    // Номер запоминается только после удачи. Набор движений мог не успеть
    // загрузиться, и забыв про удар сейчас, мы не показали бы его никогда:
    // следующий снимок принесёт тот же номер, и он покажется уже повтором.
    if (animation_.play(puppet.ped, player.state.action)) {
        puppet.actionSequence = player.state.actionSequence;
    }
}

std::optional<shared::Vec3> RemotePlayers::positionOf(shared::PlayerId player) const {
    const auto known = puppets_.find(player);
    if (known == puppets_.end() || known->second.ped == 0 || getCoords_ == nullptr) {
        return std::nullopt;
    }

    NativeContext context;
    context.push(known->second.ped);
    context.push(true);
    getCoords_(context.address());

    return shared::Vec3{context.result<float>(0), context.result<float>(1),
                        context.result<float>(2)};
}

int RemotePlayers::handleFor(shared::PlayerId player) const {
    const auto known = puppets_.find(player);
    return known == puppets_.end() ? 0 : known->second.ped;
}

shared::PlayerId RemotePlayers::ownerOf(int ped) const {
    if (ped == 0) {
        return shared::kInvalidPlayerId;
    }

    const auto it = std::ranges::find_if(puppets_, [ped](const auto& entry) {
        return entry.second.ped == ped;
    });

    return it == puppets_.end() ? shared::kInvalidPlayerId : it->first;
}

void RemotePlayers::clear() {
    if (!ready()) {
        return;
    }

    for (const auto& [id, puppet] : puppets_) {
        remove(puppet.ped);
    }

    puppets_.clear();
}

} // namespace oxymp::client::game
