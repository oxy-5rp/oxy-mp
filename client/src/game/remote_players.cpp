#include "remote_players.hpp"

#include "puppet_duty.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <oxymp/client/interpolation.hpp>
#include <oxymp/shared/math/joaat.hpp>

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

/// Сколько ждать загрузки модели, прежде чем показать носителя заготовкой, в
/// миллисекундах.
///
/// Десять секунд — заведомо больше всякой честной загрузки и заведомо меньше
/// того, что человек примет за «его тут просто нет». Ждать дольше незачем:
/// модель, не приехавшая за десять секунд, не приедет вовсе.
constexpr std::int32_t kModelPatience = 10000;

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
///
/// Полтора метра на тридцати метрах впереди — это около трёх градусов. Прежде
/// стояло пять метров, то есть почти десять градусов, и поворот у чужого игрока
/// шёл ступенями по десять градусов: он бежал по ломаной там, где хозяин шёл по
/// дуге. Это и было главной оставшейся «рваностью» на своих двоих.
constexpr float kRetaskDistance = 1.5F;

/// Чаще этого задача заново не выдаётся, в миллисекундах.
///
/// Порог по углу без порога по времени опасен: на резком развороте три градуса
/// набегают за один кадр, и задача выдавалась бы каждый кадр — а выданная
/// каждый кадр, она не даёт себе начаться, и походка распадается на топтание.
///
/// Пятьдесят миллисекунд — двадцать выдач в секунду. Столько же случалось и
/// прежде на резком развороте при пятиметровом пороге, так что хуже уже
/// проверенного не будет.
constexpr int kRetaskInterval = 50;

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

/// Сколько тело поднимается само, прежде чем его поставят на ноги рывком, в
/// миллисекундах.
///
/// Две с половиной секунды — заведомо дольше любого подъёма, какие игра
/// отыгрывает, и заведомо короче четырёх секунд обмякания, заказанных кукле.
/// Смысл срока в этом и есть: подъём отдаётся игре, а вот лежание сверх него —
/// нет.
constexpr std::int32_t kRisePatience = 2500;

/// Как часто пытаться поднять куклу, которую игра считает мёртвой, а хозяин
/// живым, в миллисекундах.
///
/// Срок здесь потому, что подъём может не удаться, и удаваться он может не
/// начать никогда. Игра считает персонажа мёртвым не по нулю, а по своему
/// порогу — и если присланное здоровье ниже него, поднятая кукла ложится
/// обратно в тот же кадр. Без срока это оборачивалось вечной каруселью:
/// RESURRECT_PED, здоровье, зачистка задач — шестьдесят раз в секунду, по
/// пятьдесят шесть тысяч подъёмов за сессию (видно в журналах построчно).
///
/// Секунда выбрана так, чтобы удавшийся подъём не заметил задержки вовсе — он
/// случается с первой попытки, — а неудавшийся перестал быть нагрузкой.
constexpr std::int32_t kRaiseInterval = 1000;

/// Насколько обмякшему телу разрешено разойтись со снимком, в метрах.
///
/// Много больше обычного порога, и это не послабление, а единственный способ
/// не испортить падение. Обмякшее тело ведёт физика: оно катится по склону,
/// отскакивает от машины, обвисает на перилах — и у хозяина делает то же самое,
/// но не в точности так же. Подводить его к снимку понемногу нельзя вовсе, а
/// рывок нужен лишь тогда, когда тело осталось совсем не там, где хозяин.
constexpr float kRagdollSnapDistance = 8.0F;

/// Ближе какого угла доворот считается законченным, в градусах.
constexpr float kSettledAngle = 2.0F;

/// Дальше какого угла доворот не отыгрывается, а ставится рывком, в градусах.
///
/// Сорок пять градусов — это уже не «чуть не довернулся», а «стоит лицом не
/// туда»: доворачивать столько ногами дольше, чем человек успевает заметить
/// ложь.
constexpr float kSnapAngle = 45.0F;

/// Через сколько проверяется, идёт ли доворот вообще, в миллисекундах.
///
/// Страховка, а не срок на поворот. Желаемое направление персонаж отрабатывает
/// сам — поворотом на месте, ногами, — но занятый чем-нибудь своим не
/// отрабатывает вовсе, и без этой проверки он остался бы стоять лицом не туда
/// навсегда. Ровно эта беда здесь уже была, и вернуть её нельзя.
constexpr std::int32_t kTurnPatience = 400;

/// Какую долю расхождения доворот обязан съесть за этот срок.
///
/// Проверяется именно доля, а не время: долгий поворот — дело обычное, а вот
/// нетронутое расхождение означает, что поворота нет вовсе. Съел четверть —
/// значит идёт, и ему дают следующий срок; не съел — угол ставится рывком.
constexpr float kTurnProgress = 0.75F;

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

/// Признак обычного выхода: открыть дверь, вылезти, закрыть за собой.
constexpr int kNormalExit = 0;

/// Сколько отводится задаче выхода, в миллисекундах.
///
/// Тот же смысл, что и у срока посадки: не уложилась — высаживаем рывком.
/// Полторы секунды длится сам выход, три — с запасом на заклинившую дверь.
constexpr std::int32_t kExitPatience = 3000;

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

/// Как часто пересаживать куклу рывком, в миллисекундах.
///
/// Пересадка мгновенна: удалась — и следующий же кадр видит персонажа на месте.
/// А вот не удаться она может насовсем — например когда место названо неверно и
/// игра отказывается двигать туда того, кто уже сидит в этой машине. Без срока
/// это оборачивалось SET_PED_INTO_VEHICLE каждый кадр до конца сессии: в
/// журналах таких пересадок по тридцать тысяч.
///
/// Полсекунды: удавшаяся пересадка срока не замечает вовсе, а неудавшаяся
/// перестаёт быть нагрузкой.
constexpr std::int32_t kSeatInterval = 500;


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
/// Дальше какого расстояния точка взгляда перестаёт быть взглядом, в метрах.
///
/// Втрое дальше самой точки: та ставится в двадцати метрах, и запас нужен на
/// расхождение снимка с настоящим положением.
constexpr float kLookReach = 60.0F;

/// Как часто задача взгляда выдаётся заново, в миллисекундах.
constexpr std::int32_t kLookRefresh = 400;

/// Насколько должна сдвинуться точка взгляда, чтобы задачу стоило выдать
/// раньше срока, в метрах.
constexpr float kRelookDistance = 2.0F;

/// Сколько живёт задача взгляда. Чуть дольше промежутка между выдачами:
/// кончившаяся раньше следующей выдачи роняет голову прямо.
constexpr int kLookTaskDuration = 600;

/// Насколько сильно поворачивается голова и какова важность задачи.
///
/// Единица — «поворачивать головой и корпусом понемногу», двойка — «важнее
/// обычного»: без второго взгляд отменяется всякой мелочью, которую игра
/// придумывает персонажу сама.
constexpr int kLookFlags = 1;
constexpr int kLookPriority = 2;

/// Кость правой кисти в нумерации игры. Из неё торчит ствол.
constexpr int kRightHandBone = 28422;

/// На сколько точка вылета отодвигается вперёд по ходу выстрела, в метрах.
///
/// Кисть руки лежит вплотную к телу, а пуля, начавшаяся внутри собственной
/// коробки, гаснет о неё в тот же миг.
constexpr float kMuzzleAhead = 0.5F;

/// Урон от показанной пули. Ноль: попадания считает стрелявший у себя.
constexpr int kNoBulletDamage = 0;

/// Род оружия, которое бросают рукой: гранаты, коктейли, липучки, дымовые.
///
/// Хеш считается здесь, а не спрашивается у игры: имена родов — часть её же
/// описаний оружия, они не меняются между сборками, а joaat у нас общий с
/// сервером и сверен с игрой разряд в разряд.
constexpr std::uint32_t kThrownGroup = shared::joaat("group_thrown");

/// Сколько движению, названному сервером, даётся на то, чтобы начаться, в
/// миллисекундах.
///
/// Задача начинается в ближайшем обновлении персонажа, а не в кадре выдачи, и
/// всё это время игра честно отвечает, что такого движения она не играет. Треть
/// секунды — с запасом на низкую частоту кадров и заведомо меньше всякого
/// осмысленного движения.
constexpr std::int32_t kScriptedGrace = 300;

/// Довод IS_ENTITY_PLAYING_ANIM, которым его зовут скрипты самой игры.
constexpr int kPlayingAnimTaskFlag = 3;

/// Сколько кукла ждёт присланного выстрела, прежде чем начать стрелять сама.
///
/// Полсекунды — заметно больше промежутка между выстрелами любого автомата и
/// заметно меньше того, что человек примет за молчание. Меньше — и в очереди
/// нашлась бы щель, в которую кукла успела бы дать своих; больше — и стрельба
/// старого клиента, который выстрелов не шлёт вовсе, начиналась бы с задержкой.
constexpr std::int32_t kShotFallback = 500;

/// Скорость пули. Минус единица означает «как у этого оружия».
constexpr float kDefaultBulletSpeed = -1.0F;

bool aiming(const shared::PlayerState& state) {
    return shared::has(state.flags, shared::PlayerFlag::Aiming) ||
           shared::has(state.flags, shared::PlayerFlag::Shooting);
}

/// Кратчайший угол между двумя направлениями, в градусах. Всегда неотрицателен.
float angleBetween(float from, float to) {
    float difference = std::fmod(to - from, 360.0F);

    if (difference < -180.0F) {
        difference += 360.0F;
    } else if (difference > 180.0F) {
        difference -= 360.0F;
    }

    return std::abs(difference);
}

} // namespace

RemotePlayers::RemotePlayers(const NativeTable& table, const Vehicles& vehicles) noexcept
    : vehicles_(vehicles),
      animation_(table),
      appearance_(table),
      hashKey_(table.handlerFor(natives::kGetHashKey)),
      requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      modelInCdimage_(table.handlerFor(natives::kIsModelInCdimage)),
      releaseModel_(table.handlerFor(natives::kSetModelAsNoLongerNeeded)),
      selectedWeapon_(table.handlerFor(natives::kGetSelectedPedWeapon)),
      createPed_(table.handlerFor(natives::kCreatePed)),
      deletePed_(table.handlerFor(natives::kDeletePed)),
      doesExist_(table.handlerFor(natives::kDoesEntityExist)),
      setCoords_(table.handlerFor(natives::kSetEntityCoordsNoOffset)),
      setHeading_(table.handlerFor(natives::kSetEntityHeading)),
      getHeading_(table.handlerFor(natives::kGetEntityHeading)),
      desiredHeading_(table.handlerFor(natives::kSetPedDesiredHeading)),
      defaultVariation_(table.handlerFor(natives::kSetPedDefaultComponentVariation)),
      blockEvents_(table.handlerFor(natives::kSetBlockingOfNonTemporaryEvents)),
      asMissionEntity_(table.handlerFor(natives::kSetEntityAsMissionEntity)),
      canBeTargetted_(table.handlerFor(natives::kSetPedCanBeTargetted)),
      diesWhenInjured_(table.handlerFor(natives::kSetPedDiesWhenInjured)),
      resurrect_(table.handlerFor(natives::kResurrectPed)),
      isDead_(table.handlerFor(natives::kIsEntityDead)),
      invincible_(table.handlerFor(natives::kSetEntityInvincible)),
      lodDistance_(table.handlerFor(natives::kSetEntityLodDist)),
      getCoords_(table.handlerFor(natives::kGetEntityCoords)),
      taskGoTo_(table.handlerFor(natives::kTaskGoStraightToCoord)),
      taskGoToAiming_(table.handlerFor(natives::kTaskGoToCoordWhileAimingAtCoord)),
      moveBlend_(table.handlerFor(natives::kSetPedDesiredMoveBlendRatio)),
      getHealth_(table.handlerFor(natives::kGetEntityHealth)),
      setHealth_(table.handlerFor(natives::kSetEntityHealth)),
      setArmour_(table.handlerFor(natives::kSetPedArmour)),
      getArmour_(table.handlerFor(natives::kGetPedArmour)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)),
      clearTasks_(table.handlerFor(natives::kClearPedTasksImmediately)),
      damagedBy_(table.handlerFor(natives::kHasEntityBeenDamagedByEntity)),
      clearDamage_(table.handlerFor(natives::kClearEntityLastDamageEntity)),
      giveWeapon_(table.handlerFor(natives::kGiveWeaponToPed)),
      setWeapon_(table.handlerFor(natives::kSetCurrentPedWeapon)),
      setAmmo_(table.handlerFor(natives::kSetPedAmmo)),
      taskAim_(table.handlerFor(natives::kTaskAimGunAtCoord)),
      taskLookAt_(table.handlerFor(natives::kTaskLookAtCoord)),
      taskShoot_(table.handlerFor(natives::kTaskShootAtCoord)),
      setRagdoll_(table.handlerFor(natives::kSetPedToRagdoll)),
      canRagdoll_(table.handlerFor(natives::kSetPedCanRagdoll)),
      isRagdoll_(table.handlerFor(natives::kIsPedRagdoll)),
      setIntoVehicle_(table.handlerFor(natives::kSetPedIntoVehicle)),
      enterVehicle_(table.handlerFor(natives::kTaskEnterVehicle)),
      leaveVehicle_(table.handlerFor(natives::kTaskLeaveVehicle)),
      isInVehicle_(table.handlerFor(natives::kIsPedInVehicle)),
      pedInSeat_(table.handlerFor(natives::kGetPedInVehicleSeat)),
      playingAnim_(table.handlerFor(natives::kIsEntityPlayingAnim)),
      usingScenario_(table.handlerFor(natives::kIsPedUsingScenario)),
      giveComponent_(table.handlerFor(natives::kGiveWeaponComponentToPed)),
      setWeaponTint_(table.handlerFor(natives::kSetPedWeaponTintIndex)),
      shootBullet_(table.handlerFor(natives::kShootSingleBulletBetweenCoords)),
      throwProjectile_(table.handlerFor(natives::kTaskThrowProjectile)),
      weaponGroup_(table.handlerFor(natives::kGetWeapontypeGroup)),
      shotTimer_(table.handlerFor(natives::kGetGameTimer)),
      boneCoords_(table.handlerFor(natives::kGetPedBoneCoords)) {}

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
        spdlog::error("GET_HASH_KEY returned {:#010x} instead of {:#010x}: other players cannot be shown",
                      model_, kRemoteModelHash);
        model_ = 0;
    }

    return model_;
}

std::uint32_t RemotePlayers::wantedModel(shared::PlayerId player) {
    const auto known = looks_.find(player);

    if (known == looks_.end() || known->second.model == 0) {
        // Сервер о модели ничего не сказал — показываем заготовкой. Ноль здесь
        // законное значение, а не пропуск: у alt:V `player.model` нулём не
        // бывает, у нас же ноль означает «не назначали».
        return model();
    }

    // Модель, которой мы уже перестали ждать, — первой: этот ответ ничего не
    // стоит, а спрашивают его каждый кадр у каждого её носителя.
    //
    // Так бывает с моделью, которая у игры числится, а грузиться всё равно
    // отказывается: например, когда её файлы приехали от сервера битыми.
    if (unloadable_.contains(known->second.model)) {
        return model();
    }

    // Заказ несуществующей модели ждёт её вечно, и вечно ждущий игрок не
    // появился бы в мире ни разу — молча. Поэтому спрашиваем игру, есть ли у
    // неё такая вообще, и не найдя — показываем заготовкой: показать человека
    // не тем, кем он есть, куда лучше, чем не показать вовсе.
    if (modelInCdimage_ != nullptr &&
        !invokeNative<bool>(modelInCdimage_, known->second.model)) {
        return model();
    }

    return known->second.model;
}

RemotePlayers::Born RemotePlayers::spawn(const RemotePlayerView& player) {
    const std::uint32_t wanted = wantedModel(player.id);
    if (wanted == 0) {
        return {};
    }

    invokeNative<void>(requestModel_, wanted);

    if (!invokeNative<bool>(hasModelLoaded_, wanted)) {
        // Ждём — но не бесконечно. Сколько ждём, считается от первого заказа
        // этой модели, а не этого игрока: не грузится она, а не он.
        if (gameTimer_ != nullptr && wanted != model()) {
            const auto now = invokeNative<std::int32_t>(gameTimer_);
            const auto [asked, added] = modelAskedAt_.try_emplace(wanted, now);

            if (!added && now - asked->second >= kModelPatience) {
                spdlog::warn("ped model {:#010x} does not load: players wearing it are shown "
                             "with the freemode body instead",
                             wanted);

                unloadable_.insert(wanted);
            }
        }

        return {};
    }

    // Загрузилась — значит ждать её больше не нужно, и помнить о заказе тоже.
    modelAskedAt_.erase(wanted);

    const int ped =
        invokeNative<int>(createPed_, kCivilianType, wanted, player.state.position.x,
                          player.state.position.y, player.state.position.z, player.state.heading,
                          false, false);

    // Заказ снимается сразу за созданием: пока он стоит, игра держит модель в
    // памяти, даже если персонажа этой модели не осталось ни одного. С одной
    // заготовкой на всех это было безразлично, а моделей, назначенных сервером,
    // бывает столько же, сколько игроков.
    if (releaseModel_ != nullptr) {
        invokeNative<void>(releaseModel_, wanted);
    }

    if (ped == 0) {
        return {};
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
    //
    // Кроме выстрела в голову: тот убивает персонажа мимо всех расчётов урона, и
    // этот запрет его не держит. Отбирать у игры критические попадания
    // (SET_PED_SUFFERS_CRITICAL_HITS) нельзя — с ними уходит и смертельность
    // выстрела в голову, а её игрок замечает первой же перестрелкой. Пусть
    // кукла падает: попадание уйдёт серверу разницей здоровья, то есть целиком,
    // и жертва умрёт по-настоящему. А если выживет — броня, скрипт со своими
    // правилами, — куклу поднимут ниже, по общей проверке.
    invokeNative<void>(diesWhenInjured_, ped, false);

    invokeNative<void>(lodDistance_, ped, kLodDistance);

    // Падать по своей воле кукла не должна: см. allowRagdoll. Здесь запрет
    // ставится прямо, а не через него, — записи о кукле ещё нет, а игре сказать
    // нужно до того, как её кто-нибудь заденет.
    if (canRagdoll_ != nullptr) {
        invokeNative<void>(canRagdoll_, ped, false);
    }

    // Одежда надевается сразу за созданием, а не следующим кадром: иначе игрок
    // на мгновение появится голым, и мгновение это заметно — персонаж выходит
    // из-за поворота уже одетым или не выходит вовсе.
    if (const auto known = looks_.find(player.id); known != looks_.end()) {
        appearance_.apply(ped, known->second);
    }

    spdlog::debug("игрок {} ({}) показан персонажем {} модели {:#010x}", player.id,
                  player.nickname, ped, wanted);
    return Born{.ped = ped, .model = wanted};
}

void RemotePlayers::fire(shared::PlayerId player, std::uint32_t weapon,
                         const shared::Vec3& target) {
    if (weapon == 0) {
        return;
    }

    const auto puppet = puppets_.find(player);
    if (puppet == puppets_.end() || puppet->second.ped == 0) {
        // Куклы нет: игрок ещё не показан либо уже ушёл. Стрелять неоткуда, и
        // это обычное дело — выстрел идёт надёжным каналом и вполне может
        // обогнать первый снимок.
        return;
    }

    const int ped = puppet->second.ped;

    // Брошенное рукой летит не пулей, а по дуге, и заводится оно другим путём.
    // Пулей граната не полетит вовсе: у пули нет ни веса, ни времени полёта, а
    // у гранаты только они и есть.
    //
    // Бросает кукла сама, задачей: своего «создай снаряд вот здесь и с вот
    // такой скоростью» у игры нет — точнее, есть, но не в открытой базе имён, а
    // подставлять хеш по памяти значит уронить игру в мгновение вызова.
    //
    // Цена этого пути — задержка: кукла отыгрывает замах, и её граната
    // отправится в полёт позже хозяйской. Зато полетит правильно и взорвётся
    // там, где ей положено.
    const std::uint32_t group =
        weaponGroup_ != nullptr ? invokeNative<std::uint32_t>(weaponGroup_, weapon) : 0;

    if (group == kThrownGroup) {
        if (throwProjectile_ != nullptr) {
            invokeNative<void>(throwProjectile_, ped, target.x, target.y, target.z);

            if (shotTimer_ != nullptr) {
                puppet->second.firedAt = invokeNative<std::int32_t>(shotTimer_);
            }
        }
        return;
    }

    if (shootBullet_ == nullptr) {
        return;
    }

    // Откуда вылетает пуля — от руки той самой куклы, которая стоит у нас, а не
    // от точки, присланной хозяином. Хозяин прислал бы место, где его ствол был
    // шестьдесят миллисекунд назад, а показываем мы куклу с тем же отставанием:
    // пуля вылетала бы из воздуха рядом с ней.
    shared::Vec3 muzzle;

    if (boneCoords_ != nullptr) {
        NativeContext bone;
        bone.push(ped);
        bone.push(kRightHandBone);
        bone.push(0.0F);
        bone.push(0.0F);
        bone.push(0.0F);
        boneCoords_(bone.address());

        muzzle = shared::Vec3{bone.result<float>(0), bone.result<float>(1), bone.result<float>(2)};
    } else {
        NativeContext coords;
        coords.push(ped);
        coords.push(true);
        getCoords_(coords.address());

        muzzle = shared::Vec3{coords.result<float>(0), coords.result<float>(1),
                              coords.result<float>(2) + shared::kLookHeight};
    }

    // Точка вылета отодвигается вперёд по ходу выстрела. Кисть руки лежит
    // вплотную к телу, а пуля, начавшаяся внутри собственной коробки, гаснет о
    // неё в тот же миг: снаружи это выглядит как стреляющий без единого следа
    // на стенах.
    const float dx = target.x - muzzle.x;
    const float dy = target.y - muzzle.y;
    const float dz = target.z - muzzle.z;
    const float reach = std::sqrt(dx * dx + dy * dy + dz * dz);

    if (reach > kMuzzleAhead) {
        const float share = kMuzzleAhead / reach;

        muzzle.x += dx * share;
        muzzle.y += dy * share;
        muzzle.z += dz * share;
    }

    invokeNative<void>(shootBullet_, muzzle.x, muzzle.y, muzzle.z, target.x, target.y, target.z,
                       kNoBulletDamage, true, weapon, ped, true, false, kDefaultBulletSpeed);

    // Отмечаем, что за эту куклу выстрелили мы. Пока такие выстрелы идут, сама
    // она стрелять не будет — см. Puppet::firedAt.
    if (shotTimer_ != nullptr) {
        puppet->second.firedAt = invokeNative<std::int32_t>(shotTimer_);
    }
}

void RemotePlayers::arm(shared::PlayerId player, const shared::PlayerWeapon& look) {
    guns_[player] = look;

    // Надеть прямо сейчас нельзя: насадки ставятся на ствол, который уже в
    // руках, а он выдаётся кукле по снимку. Забудем об этом — и глушитель
    // появился бы только после следующей смены оружия.
    //
    // Поэтому сбрасываем память о выданном оружии: следующий же кадр выдаст его
    // заново, теперь уже с насадками.
    if (const auto puppet = puppets_.find(player); puppet != puppets_.end()) {
        puppet->second.weapon = 0;
    }
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
            if (const Born born = spawn(player); born.ped != 0) {
                puppets_.emplace(player.id,
                                 Puppet{.ped = born.ped,
                                        .model = born.model,
                                        .appliedHealth = player.state.health,
                                        .appliedArmour = player.state.armour,
                                        .actionSequence = player.state.actionSequence});
            }
            continue;
        }

        Puppet& puppet = known->second;

        // Персонажа могло не стать помимо нас: игра вправе убрать то, что
        // считает лишним, а обращение к исчезнувшему — это вылет.
        if (!invokeNative<bool>(doesExist_, puppet.ped)) {
            spdlog::warn("the ped of player {} vanished, creating it again", player.id);
            puppets_.erase(known);
            continue;
        }

        // Сервер назначил игроку другую модель. Сменить её у готового
        // персонажа нельзя — модель это и есть тело, — поэтому персонаж
        // заводится заново, а одежда наденется на него при создании.
        //
        // Заведётся он следующим кадром, а не этим: модель может оказаться
        // ещё не загруженной, и городить здесь второй заход незачем — список
        // придёт снова через кадр.
        //
        // Объявленное сравнивается раньше разрешённого нарочно: разрешение
        // спрашивает у игры, есть ли у неё такая модель, а это вызов натива на
        // каждого показанного человека в каждом кадре. Совпало объявленное с
        // тем, из чего кукла сделана, — спрашивать не о чем.
        const auto dressed = looks_.find(player.id);
        const std::uint32_t announced =
            dressed != looks_.end() ? dressed->second.model : 0;

        // Разрешённая модель спрашивается только при расхождении: сама проверка
        // ходит к игре, а разошлось объявленное с телом — случай редкий.
        const std::uint32_t wanted =
            announced != 0 && announced != puppet.model ? wantedModel(player.id) : puppet.model;

        if (wanted != puppet.model) {
            spdlog::debug("player {} changed model from {:#010x} to {:#010x}, the ped is remade",
                          player.id, puppet.model, wanted);

            remove(puppet.ped);
            puppets_.erase(known);
            continue;
        }

        // Падать кукле разрешено только тогда, когда об этом сказал хозяин, —
        // и решается это раньше всего прочего в кадре.
        //
        // Смерть здесь наравне с рэгдоллом, и это не запас: убитая кукла с
        // запретом на падение осталась бы стоять столбом на месте собственной
        // смерти. Разрешение выдаётся до settleHealth намеренно — именно она
        // обнуляет здоровье, то есть убивает.
        allowRagdoll(puppet, shared::has(player.state.flags, shared::PlayerFlag::Ragdoll) ||
                                 shared::has(player.state.flags, shared::PlayerFlag::Dead));

        settleHealth(puppet, player, localPed, now);

        // Мёртвый никуда не идёт и никуда не смотрит: он лежит там, где упал.
        // Ставить ему положение значило бы возить труп по земле.
        if (!shared::has(player.state.flags, shared::PlayerFlag::Dead)) {
            ride(puppet, player, now);

            // Кто в этом кадре распоряжается телом, решает отдельный разбор
            // без единого натива — его и проверить можно списком случаев (см.
            // puppet_duty.hpp). Здесь остаётся только исполнить решённое.
            //
            // Два ответа из шести приходится добывать до него, и оба у игры:
            // обмякло ли тело на самом деле и идёт ли движение сервера. Ни то
            // ни другое из снимка хозяина не выводится.
            const bool limp = settleRagdoll(puppet, player, now);

            const PuppetDuty duty = dutyFor(PuppetSituation{
                .flags = player.state.flags,
                .verticalSpeed = player.state.velocity.z,
                .limp = limp,
                .scripted = !limp && scripted(puppet, now),
                .leaving = puppet.leavingSince != 0,
                .carried = shared::has(player.state.flags, shared::PlayerFlag::InVehicle) &&
                           vehicles_.handleFor(player.state.vehicleId) != 0,
            });

            // Поза задаётся раньше движения, и порядок здесь важен. Прыжок,
            // лазание и уход в укрытие — это задачи, а задача ходьбы, выданная
            // следом, их отменяет.
            if (duty.posture) {
                animation_.applyPosture(puppet.ped, player.state.flags, puppet.flags);

                // Оружие вкладывается в руки там же, где разрешена поза, и по
                // той же причине: это состояние, а не задача, и спорить ему не с
                // чем. Обмякшему телу и занятому движением сервера — не
                // вкладывается: выдача сбрасывает позу.
                equip(puppet, player);
            }

            // Задачу ходьбы с падающего снимают на переходе и только на нём:
            // оставленная, она заставляет персонажа перебирать ногами в воздухе.
            if (duty.falling && !puppet.falling) {
                animation_.clearTasks(puppet.ped);
                puppet.taskedAt = 0;
            }

            puppet.falling = duty.falling;

            switch (duty.body) {
            case PuppetBody::Physics:
                // Телом распоряжается физика. Подводим его к снимку только
                // тогда, когда оно уехало совсем далеко, — понемногу нельзя.
                drift(puppet, player);
                break;

            case PuppetBody::Boarding:
                // Ни положением, ни задачей: персонажа ведёт своя задача входа
                // или выхода, а сидящего — машина.
                break;

            case PuppetBody::Scripted:
                // Положение подводим — тело обязано оказаться там, где хозяин, —
                // а задач не даём вовсе: всякая отменила бы движение сервера.
                walk(puppet, player, seconds, now, true);
                break;

            case PuppetBody::Riding:
                if (aiming(player.state)) {
                    // Сидящий в машине показывает единственное, что может
                    // показать: куда он целится из окна.
                    animation_.applyDriveBy(puppet.ped, player.state.aimAt,
                                            shared::has(player.state.flags,
                                                        shared::PlayerFlag::Shooting) &&
                                                ownFire(puppet));
                } else {
                    // А не целящийся — то же, что и пеший: куда смотрит. Без
                    // этого водитель и пассажиры едут, уставившись строго перед
                    // собой, как манекены.
                    look(puppet, player, now);
                }
                break;

            case PuppetBody::Ours:
                walk(puppet, player, seconds, now, !duty.tasks);

                if (duty.tasks) {
                    aim(puppet, player);
                    look(puppet, player, now);
                }
                break;
            }

            // Удар обмякшему телу не показать: движение поверх рэгдолла игра
            // проглатывает молча, а номер движения мы бы при этом запомнили как
            // показанный. Поверх движения, которое велел сервер, удар тоже не
            // показывают — но по другой причине: он бы его отменил.
            if (duty.action) {
                act(puppet, player);
            }
        }

        puppet.flags = player.state.flags;
    }
}

void RemotePlayers::settleHealth(Puppet& puppet, const RemotePlayerView& player, int localPed,
                                 std::int32_t now) const {
    const bool wasDead = shared::has(puppet.flags, shared::PlayerFlag::Dead);
    const bool dead = shared::has(player.state.flags, shared::PlayerFlag::Dead);

    const int current = invokeNative<int>(getHealth_, puppet.ped);

    // Броня читается до того, как её вернут на место, — иначе читать было бы
    // нечего.
    const int currentArmour =
        getArmour_ != nullptr ? invokeNative<int>(getArmour_, puppet.ped) : puppet.appliedArmour;

    // Попадание замечается по разнице между тем, что мы поставили в прошлый
    // кадр, и тем, что осталось сейчас. Проверка «кто ударил» обязательна: без
    // неё мы бы отправляли серверу и урон от чужой машины, и падение с высоты —
    // всё то, что хозяин уже посчитал у себя.
    if (onDamage_ && localPed != 0 && damagedBy_ != nullptr && !dead) {
        if (invokeNative<bool>(damagedBy_, puppet.ped, localPed, true)) {
            // Считается вместе с бронёй, и это была не мелочь, а дыра в самой
            // перестрелке.
            //
            // Пуля уходит сперва в броню и только потом в здоровье. Броню же мы
            // возвращаем кукле каждый кадр — она принадлежит хозяину, как и
            // здоровье, — и потому за кадр она успевает поглотить попадание и
            // восстановиться. По разнице одного здоровья такое попадание не
            // видно вовсе: бронированный игрок не терял ничего, сколько в него
            // ни стреляй.
            //
            // Серверу уходит полный урон, а не остаток: он делит его на броню и
            // здоровье сам и по тем же самым числам, что и игра здесь.
            const int lostArmour = std::max(puppet.appliedArmour - currentArmour, 0);
            const int lost = std::max(puppet.appliedHealth - current, 0) + lostArmour;

            if (lost > 0) {
                // Оружие называется наше, а не жертвы, и это было настоящей
                // ошибкой: сюда уходил ствол того, в кого попали. Сервер
                // пересказывает это число дважды — жертве, чтобы показать, из
                // чего по ней попали, и скрипту в событии смерти как оружие
                // убийства, — и оба раза оно называло не то. Безоружная жертва
                // делала всякое убийство ударом кулака.
                //
                // Спрашивается ствол в руках, а не разбирается причина урона:
                // причину игра наружу не отдаёт, и сбитый машиной запишется на
                // то, что у нас в руках. Это неточно, но это ближе правды, чем
                // чужой ствол, и заметно только в редком случае.
                const std::uint32_t weapon =
                    selectedWeapon_ != nullptr
                        ? invokeNative<std::uint32_t>(selectedWeapon_, localPed)
                        : player.state.weapon;

                onDamage_(player.id, static_cast<std::uint16_t>(lost), weapon);
            }

            if (clearDamage_ != nullptr) {
                invokeNative<void>(clearDamage_, puppet.ped);
            }
        }
    }

    // Здоровье возвращается к тому, что сказал хозяин. Он единственный, кто
    // знает его наверняка: у него настоящий игрок, а у нас его изображение.
    const int wanted = dead ? 0 : static_cast<int>(player.state.health);

    // Но поднять поставленным здоровьем куклу, которую игра всё же успела
    // записать в мёртвые, нельзя — для этого есть отдельный натив. Сюда мы
    // приходим за тем, чего не предусмотрели: выстрел в голову, от которого
    // жертва уцелела, взрыв рядом, огонь, падение с высоты, добивание в ближнем
    // бою. Перечислять такие пути поимённо бессмысленно, а проверка одна и
    // стоит один вызов.
    //
    // Спрашивается здесь именно поставленное здоровье, а не признак смерти.
    // Разойтись они могут: сервер вправе обнулить здоровье распоряжением
    // скрипта, и до ответного снимка хозяина признак ещё не поднят. Поднимай мы
    // куклу по признаку — она вставала бы и падала замертво каждый кадр, потому
    // что следом ей ставят тот самый ноль.
    const bool downHere = wanted > 0 && resurrect_ != nullptr && isDead_ != nullptr &&
                          invokeNative<bool>(isDead_, puppet.ped);

    if (!downHere) {
        // Стоит на ногах — значит и память о попытках поднять больше не нужна:
        // следующая смерть начнёт счёт заново.
        puppet.raisedAt = 0;
        puppet.raiseFailureTold = false;
    }

    // Подъём — по сроку, а не каждый кадр, и это правка по журналу.
    //
    // Он может не удаться вовсе: мёртвым игра считает персонажа не по нулю
    // здоровья, а по своему порогу, и кукла, которой мы ставим здоровье ниже
    // порога, ложится обратно в том же кадре. Без срока получалась вечная
    // карусель — подъём, здоровье, зачистка задач, шестьдесят раз в секунду.
    // В одной сессии таких подъёмов насчиталось пятьдесят шесть тысяч.
    const bool raiseDue = puppet.raisedAt == 0 || now - puppet.raisedAt >= kRaiseInterval;

    if (downHere && raiseDue) {
        // Не удался прошлый — говорим об этом один раз и называем число, из-за
        // которого он не удаётся: чаще всего дело именно в нём.
        if (puppet.raisedAt != 0 && !puppet.raiseFailureTold) {
            puppet.raiseFailureTold = true;

            spdlog::warn("player {} stays dead in the game at health {} while the owner reports "
                         "him alive: the game treats a ped at this health as dead",
                         player.id, wanted);
        }

        puppet.raisedAt = now;

        invokeNative<void>(resurrect_, puppet.ped);

        // Признаки после подъёма выставляются заново: часть их игра снимает
        // вместе со смертью, и поднятая кукла снова ввязывалась бы в чужие
        // события и умирала бы от следующей же царапины.
        invokeNative<void>(blockEvents_, puppet.ped, true);
        invokeNative<void>(diesWhenInjured_, puppet.ped, false);

        // И запрет падать — тоже, а память о нём объявляется неверной.
        //
        // Разрешение падать мы помним, а не спрашиваем: спросить у игры «а можно
        // ли ему падать» нельзя. Подъём же сбрасывает признаки персонажа мимо
        // нашей памяти — и та осталась бы говорить «запрещено» о теле, которому
        // уже можно. Со стороны это выглядело бы так, что после первой же смерти
        // человек снова начинает крутиться от выстрела и ползать от толчка.
        if (canRagdoll_ != nullptr) {
            invokeNative<void>(canRagdoll_, puppet.ped, false);
        }

        puppet.ragdollAllowed = false;

        invokeNative<void>(clearTasks_, puppet.ped);
        puppet.taskedAt = 0;

        // И память об оружии — тоже, иначе поднятая кукла останется с пустыми
        // руками навсегда.
        //
        // Смерть отбирает у персонажа всё, что было в руках, а выдаём мы оружие
        // только при смене: сверяется присланный хеш с тем, что мы выдали в
        // прошлый раз, — и после подъёма они совпадают, хотя в руках уже ничего
        // нет. Забыв выданное, мы заставляем следующий же кадр выдать его
        // заново.
        puppet.weapon = 0;
        puppet.ammo = 0;

        spdlog::debug("player {} was dead here but alive at the owner, resurrected", player.id);
    }

    if (current != wanted) {
        invokeNative<void>(setHealth_, puppet.ped, wanted);
    }
    puppet.appliedHealth = wanted;

    // Броня видна со стороны: она рисуется на полосе над головой и решает,
    // сколько выстрелов персонаж выдержит, прежде чем по нему начнёт попадать
    // по-настоящему.
    if (setArmour_ != nullptr) {
        invokeNative<void>(setArmour_, puppet.ped, static_cast<int>(player.state.armour));
        puppet.appliedArmour = static_cast<int>(player.state.armour);
    } else {
        // Броню поставить нечем — значит и терять её кукле неоткуда, и считать
        // по ней урон нельзя. Запоминаем то, что у неё есть на самом деле: иначе
        // разница с воображаемой бронёй ушла бы серверу как попадание, которого
        // не было.
        puppet.appliedArmour = currentArmour;
    }

    if (dead == wasDead) {
        return;
    }

    // Задачи снимаются и при смерти, и при подъёме: мертвецу они не нужны, а
    // поднявшемуся достанутся чужие, оставшиеся от того, кем он был до смерти.
    invokeNative<void>(clearTasks_, puppet.ped);
    puppet.taskedAt = 0;

    // И память об оружии — по той же причине, что и выше: смерть отбирает у
    // персонажа всё, что было в руках. Здесь это на случай, когда кукла умерла и
    // поднялась мимо resurrect — например, когда хозяин объявил смерть и жизнь
    // между двумя нашими кадрами.
    puppet.weapon = 0;
    puppet.ammo = 0;

    spdlog::debug("игрок {} {}", player.id, dead ? "погиб" : "снова жив");
}

void RemotePlayers::ride(Puppet& puppet, const RemotePlayerView& player, std::int32_t now) const {
    const bool riding = shared::has(player.state.flags, shared::PlayerFlag::InVehicle);
    const bool entering = shared::has(player.state.flags, shared::PlayerFlag::EnteringVehicle);
    const bool leaving = shared::has(player.state.flags, shared::PlayerFlag::LeavingVehicle);

    if (!riding && !entering) {
        if (puppet.vehicleId == shared::kInvalidVehicleId) {
            puppet.leavingSince = 0;
            return;
        }

        const int left = vehicles_.handleFor(puppet.vehicleId);

        // Хозяин вылезает прямо сейчас — и кукла вылезает вместе с ним, тем же
        // движением: открывает дверь, выбирается, закрывает за собой.
        //
        // Прежде этого признака не было вовсе, и о выходе мы узнавали лишь по
        // тому, что хозяин перестал числиться сидящим. К этому мгновению он уже
        // стоял на асфальте, и кукле оставалось выпасть наружу рывком —
        // полутора секунд выхода со стороны не было видно никогда.
        //
        // Отыграть выход можно только тому, кто внутри. Проверка не
        // придирчивость: кукла могла не успеть сесть вовсе, и тогда задача
        // выхода не сделает ничего — а мы бы полторы секунды ждали её конца,
        // не ведя персонажа ни положением, ни походкой. Со стороны это
        // выглядело бы замиранием на ровном месте.
        const bool inside = left != 0 && leaveVehicle_ != nullptr &&
                            (isInVehicle_ == nullptr ||
                             invokeNative<bool>(isInVehicle_, puppet.ped, left, true));

        // Сроку задача подчиняется так же, как и посадка: не уложилась —
        // высаживаем рывком. Дверь могло заклинить о стену, а хозяин к этому
        // времени уже идёт своей дорогой.
        const bool overdue =
            puppet.leavingSince != 0 && now - puppet.leavingSince >= kExitPatience;

        if (leaving && inside && !overdue) {
            if (puppet.leavingSince == 0) {
                puppet.leavingSince = now;
                invokeNative<void>(leaveVehicle_, puppet.ped, left, kNormalExit);

                spdlog::debug("player {} is climbing out of vehicle {:#010x}", player.id,
                              puppet.vehicleId);
            }

            // Задача идёт. Место в машине за куклой пока числится: пока она не
            // вылезла, она в ней сидит.
            return;
        }

        // Вылез. Дальше — только страховка: если задача выхода не успела или её
        // не было вовсе (старый клиент выхода не объявляет), высаживаем рывком.
        puppet.vehicleId = shared::kInvalidVehicleId;
        puppet.seat = shared::kNoSeat;
        puppet.enteringSince = 0;
        puppet.leavingSince = 0;

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
        //
        // Выдаётся заново в двух случаях, и оба живые. Первый: хозяин передумал
        // и полез в другую машину или на другое место — задача же ведёт куклу к
        // прежней двери и сама об этом не узнает. Второй: задача идёт дольше
        // отведённого ей срока — дверь могло заклинить о стену, а до неё ещё и
        // дойти надо.
        const bool sameTarget = puppet.vehicleId == player.state.vehicleId &&
                                puppet.seat == player.state.seat;

        const bool overdue =
            puppet.enteringSince != 0 && now - puppet.enteringSince >= kEntryPatience;

        if (puppet.enteringSince != 0 && sameTarget && !overdue) {
            return;
        }

        if (enterVehicle_ == nullptr) {
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

    // Хозяин уже сидит. Сидит ли наш персонаж — и, главное, на том ли месте.
    //
    // Спрашивается именно место, а не «в машине ли он вообще», и это то самое,
    // из-за чего чужой игрок оказывался пассажиром в собственной машине.
    // Сверялась прежде одна только память о нашей же просьбе: попросили
    // водительское — считаем, что он за рулём. А задача входа вправе усадить
    // персонажа не туда, куда её просили: место могло оказаться занято, дверь
    // — заблокирована, игра могла счесть, что с другой стороны ближе. Усаженный
    // не на своё место оставался там навсегда, потому что спросить об этом было
    // некому.
    const bool placed =
        puppet.vehicleId == player.state.vehicleId && puppet.seat == player.state.seat;

    // Если спросить нечем, память — единственное, что есть, и считать по ней
    // «не сидит» нельзя: мы сажали бы его рывком каждый кадр.
    const bool seated =
        pedInSeat_ != nullptr
            ? invokeNative<int>(pedInSeat_, vehicle, seat) == puppet.ped
            : (isInVehicle_ != nullptr
                   ? placed && invokeNative<bool>(isInVehicle_, puppet.ped, vehicle, false)
                   : placed);

    if (seated) {
        puppet.vehicleId = player.state.vehicleId;
        puppet.seat = player.state.seat;
        puppet.enteringSince = 0;
        puppet.leavingSince = 0;

        // Сидит на своём месте — значит и память о неудачных пересадках больше
        // не нужна: следующая начнёт счёт заново.
        puppet.seatedAt = 0;
        puppet.seatFailureTold = false;
        return;
    }

    // Вход был начат и ещё не кончился — дадим ему доиграть. Но не бесконечно:
    // дверь могло заклинить о стену, а хозяин уже едет.
    if (puppet.enteringSince != 0 && now - puppet.enteringSince < kEntryPatience) {
        return;
    }

    // Где он оказался вместо своего места — в журнал: занятое чужим место
    // означает, что двое считают себя на нём одновременно, и разбираться с этим
    // придётся не здесь.
    if (pedInSeat_ != nullptr && isInVehicle_ != nullptr &&
        invokeNative<bool>(isInVehicle_, puppet.ped, vehicle, false)) {
        spdlog::debug("player {} sat in the wrong seat of vehicle {:#010x}, moving to seat {}",
                      player.id, player.state.vehicleId, seat);
    }

    // Пересадка — по сроку, а не каждый кадр. Удалась — срока никто не заметит,
    // не удалась — она хотя бы перестанет быть каруселью на всю сессию.
    const bool sameSeat =
        puppet.seatedVehicle == player.state.vehicleId && puppet.seatedIndex == player.state.seat;

    if (sameSeat && puppet.seatedAt != 0 && now - puppet.seatedAt < kSeatInterval) {
        return;
    }

    if (sameSeat && puppet.seatedAt != 0 && !puppet.seatFailureTold) {
        puppet.seatFailureTold = true;

        spdlog::warn("player {} does not stay in seat {} of vehicle {:#010x}: the game keeps him "
                     "elsewhere",
                     player.id, seat, player.state.vehicleId);
    }

    puppet.vehicleId = player.state.vehicleId;
    puppet.seat = player.state.seat;
    puppet.enteringSince = 0;
    puppet.leavingSince = 0;

    puppet.seatedVehicle = player.state.vehicleId;
    puppet.seatedIndex = player.state.seat;
    puppet.seatedAt = now;

    if (!sameSeat) {
        puppet.seatFailureTold = false;
    }

    if (setIntoVehicle_ != nullptr) {
        invokeNative<void>(setIntoVehicle_, puppet.ped, vehicle, seat);
    }

    spdlog::debug("игрок {} сел в машину {:#010x} на место {}", player.id, player.state.vehicleId,
                  seat);
}

void RemotePlayers::turn(Puppet& puppet, const RemotePlayerView& player, bool moving,
                         std::int32_t now) const {
    // Угол приходит уже посчитанным на это мгновение — тем же расчётом и на то
    // же время, что и положение. Доводить его здесь долей за кадр, как делалось
    // раньше, не нужно и вредно: доля за кадр означает, что скорость доворота
    // зависит от частоты кадров, а до нужного угла персонаж доходит уже тогда,
    // когда хозяин смотрит в другую сторону.
    if (desiredHeading_ == nullptr) {
        if (setHeading_ != nullptr) {
            invokeNative<void>(setHeading_, puppet.ped, player.state.heading);
        }
        return;
    }

    // Желаемое направление, а не мгновенное: игра доворачивает персонажа сама,
    // отыгрывая поворот ногами, вместо того чтобы вращать его вокруг оси.
    //
    // Идущему так было и раньше. Стоящему угол ставился прямо — и стоящий
    // человек разворачивался, как башня: тело мгновенно оказывалось лицом
    // туда, куда хозяин только начал поворачиваться. Поворота на месте, который
    // игра прекрасно умеет, со стороны не было видно ни разу.
    invokeNative<void>(desiredHeading_, puppet.ped, player.state.heading);

    if (moving || getHeading_ == nullptr || setHeading_ == nullptr) {
        // Идущего доворачивает походка, и подгонять его рывком незачем:
        // направление движения и без того задаёт задача.
        puppet.turningSince = 0;
        return;
    }

    // Но полагаться на один лишь доворот нельзя, и это здесь уже стоило беды:
    // персонаж, занятый чем-нибудь своим, желаемое направление не отрабатывает
    // вовсе и остаётся стоять лицом не туда. Поэтому за доворотом следим, и
    // если он не идёт — ставим угол рывком.
    const float off = angleBetween(invokeNative<float>(getHeading_, puppet.ped),
                                   player.state.heading);

    if (off <= kSettledAngle) {
        puppet.turningSince = 0;
        return;
    }

    if (off >= kSnapAngle) {
        // Так далеко ногами не доворачивают: пока персонаж отыгрывал бы полный
        // разворот, хозяин успел бы обернуться дважды.
        invokeNative<void>(setHeading_, puppet.ped, player.state.heading);
        puppet.turningSince = 0;
        return;
    }

    if (puppet.turningSince == 0) {
        puppet.turningSince = now;
        puppet.turningGap = off;
        return;
    }

    if (now - puppet.turningSince < kTurnPatience) {
        return;
    }

    if (off <= puppet.turningGap * kTurnProgress) {
        // Доворот идёт — пусть доворачивает. Срок отсчитывается заново от того
        // расхождения, что осталось.
        puppet.turningSince = now;
        puppet.turningGap = off;
        return;
    }

    // Расхождение стоит на месте: желаемое направление персонаж не отрабатывает.
    invokeNative<void>(setHeading_, puppet.ped, player.state.heading);
    puppet.turningSince = 0;
}

bool RemotePlayers::animate(shared::PlayerId player, const shared::PlayerAnimation& animation) {
    const auto known = puppets_.find(player);
    if (known == puppets_.end() || known->second.ped == 0) {
        // Куклы ещё нет: игрок далеко либо его модель грузится. Распоряжение
        // подождёт — его повторят в следующем кадре.
        return false;
    }

    if (!animation_.playNamed(known->second.ped, animation)) {
        return false;
    }

    // Запоминаем не ради памяти, а ради задач: пока это движение идёт, кукле
    // нельзя выдавать ни походку, ни прицел — они его отменят.
    // Сценарий запоминается там же, где движение, и в том же поле имени: занят
    // персонаж одинаково — задачей, которую выдали не мы. Отличает их пустой
    // набор: у сценария его нет вовсе.
    known->second.scriptedDictionary = animation.dictionary;
    known->second.scriptedName =
        animation.scenario.empty() ? animation.name : animation.scenario;
    known->second.scriptedScenario = !animation.scenario.empty();
    known->second.scriptedAt = gameTimer_ != nullptr
                                   ? invokeNative<std::int32_t>(gameTimer_)
                                   : 0;

    return true;
}

bool RemotePlayers::stopAnimating(shared::PlayerId player) {
    const auto known = puppets_.find(player);
    if (known == puppets_.end() || known->second.ped == 0) {
        return false;
    }

    animation_.clearTasks(known->second.ped);

    known->second.scriptedDictionary.clear();
    known->second.scriptedName.clear();
    known->second.scriptedScenario = false;
    known->second.scriptedAt = 0;
    known->second.taskedAt = 0;

    return true;
}

bool RemotePlayers::scripted(Puppet& puppet, std::int32_t now) const {
    if (puppet.scriptedDictionary.empty() && !puppet.scriptedScenario) {
        return false;
    }

    // Отсрочка. Задача начинается не в тот же миг, когда её выдали, и вопрос,
    // заданный сразу, ответит «не играет». Поверив ему, мы снесли бы движение
    // задачей ходьбы в том же кадре, в котором завели.
    if (puppet.scriptedAt != 0 && now - puppet.scriptedAt < kScriptedGrace) {
        return true;
    }

    // Идёт ли оно ещё, спрашивается у игры, а не считается по времени. У
    // сценария вопрос свой: набора движений у него нет, и `IS_ENTITY_PLAYING_ANIM`
    // о нём не знает ничего.
    if (puppet.scriptedScenario) {
        if (usingScenario_ != nullptr &&
            invokeNative<bool>(usingScenario_, puppet.ped, puppet.scriptedName.c_str())) {
            return true;
        }
    } else if (playingAnim_ != nullptr &&
               invokeNative<bool>(playingAnim_, puppet.ped, puppet.scriptedDictionary.c_str(),
                                  puppet.scriptedName.c_str(), kPlayingAnimTaskFlag)) {
        return true;
    }

    // Кончилось — своим ходом либо чужой задачей. Забываем: держать за куклой
    // движение, которого нет, значит не вести её вовсе.
    puppet.scriptedDictionary.clear();
    puppet.scriptedName.clear();
    puppet.scriptedScenario = false;
    puppet.scriptedAt = 0;

    return false;
}

bool RemotePlayers::ownFire(const Puppet& puppet) const {
    // Своя стрельба — только пока не идут присланные выстрелы. Иначе очередь
    // выходит двойной, и половина её летит не туда, куда целился хозяин, а
    // куда попадёт кукла со своей меткостью.
    //
    // Спрашивают об этом двое — пеший прицел и стрельба из окна, — и ответ у них
    // обязан быть один. Пока правило жило внутри одного из них, второй о нём не
    // знал вовсе и палил вдобавок к присланному.
    return shotTimer_ == nullptr ||
           invokeNative<std::int32_t>(shotTimer_) - puppet.firedAt >= kShotFallback;
}

void RemotePlayers::allowRagdoll(Puppet& puppet, bool allowed) const {
    if (canRagdoll_ == nullptr || puppet.ragdollAllowed == allowed) {
        return;
    }

    invokeNative<void>(canRagdoll_, puppet.ped, allowed);
    puppet.ragdollAllowed = allowed;
}

bool RemotePlayers::settleRagdoll(Puppet& puppet, const RemotePlayerView& player,
                                  std::int32_t now) const {
    const bool wanted = shared::has(player.state.flags, shared::PlayerFlag::Ragdoll);
    const bool rising = shared::has(player.state.flags, shared::PlayerFlag::GettingUp);

    // Что с телом на самом деле. Спросить игру обязательно: помнить одну лишь
    // свою просьбу мало — уронить куклу может и сама игра, столкновением или
    // выстрелом, а хозяин об этом не говорил ничего.
    const bool limp = isRagdoll_ != nullptr ? invokeNative<bool>(isRagdoll_, puppet.ped)
                                            : puppet.ragdolling;

    if (wanted && !puppet.ragdolling) {
        // Рэгдолл заказывается на переходе, а не каждый кадр: заказанный
        // повторно, он поднимает тело и роняет заново, и вместо падения выходит
        // судорога.
        //
        // Разрешение к этому мгновению уже выдано — его выдаёт sync по тому же
        // признаку, и по запрещённому телу SET_PED_TO_RAGDOLL не сделал бы
        // ничего, причём молча.
        if (setRagdoll_ != nullptr) {
            invokeNative<void>(setRagdoll_, puppet.ped, kRagdollDuration, kRagdollDuration,
                               kRagdollKind, true, true, false);
        }

        puppet.ragdolling = true;
        puppet.taskedAt = 0;
        puppet.turningSince = 0;
        puppet.risingSince = 0;
        return true;
    }

    if (wanted) {
        return true;
    }

    // Хозяин поднялся. Разрешение к этому мгновению уже снято — его снимает sync
    // по тому же признаку.
    puppet.ragdolling = false;

    if (!limp) {
        puppet.risingSince = 0;
        return false;
    }

    // Тело ещё обмякшее, а хозяин уже нет. Дальше важно, что именно у хозяина
    // происходит.
    //
    // Он поднимается — значит и наше тело поднимается тем же путём, само:
    // вставать после рэгдолла игра умеет и делает это куда убедительнее рывка.
    // Отнять у неё этот подъём значило бы менять падение с последующим вставанием
    // на падение с последующим вскакиванием.
    //
    // Но ждать сколько угодно нельзя: кукле заказано четыре секунды обмякания, а
    // хозяин мог подняться через одну.
    if (rising) {
        if (puppet.risingSince == 0) {
            puppet.risingSince = now;
        }

        if (now - puppet.risingSince < kRisePatience) {
            return true;
        }
    }

    // Либо хозяин вовсе не падал — уронила игра, а такое случается и с запретом
    // (взрыв рядом, машина в упор), — либо подъём затянулся дольше всякого
    // разумного. Оставлять как есть нельзя: у хозяина ничего не происходит, он
    // бежит дальше, а его изображение ползёт по асфальту.
    //
    // Поднимается тело немедленной зачисткой задач: своего «встань» у игры нет,
    // а эта её как раз и возвращает из рэгдолла в стойку.
    invokeNative<void>(clearTasks_, puppet.ped);
    puppet.taskedAt = 0;
    puppet.turningSince = 0;
    puppet.risingSince = 0;

    // Один кадр телом всё ещё распоряжается физика: подводить его в этом кадре
    // значило бы дёрнуть падающего.
    return true;
}

void RemotePlayers::drift(const Puppet& puppet, const RemotePlayerView& player) const {
    NativeContext coords;
    coords.push(puppet.ped);
    coords.push(true);
    getCoords_(coords.address());

    const shared::Vec3 actual{coords.result<float>(0), coords.result<float>(1),
                              coords.result<float>(2)};

    if (distanceBetween(actual, player.state.position) <= kRagdollSnapDistance) {
        return;
    }

    invokeNative<void>(setCoords_, puppet.ped, player.state.position.x, player.state.position.y,
                       player.state.position.z, false, false, false);
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

    // Смена задачи с обычной на целящуюся и обратно — не повод медлить: пока
    // задача не сменилась, персонаж идёт не тем боком. Всё остальное ждёт
    // своего срока: выданная каждый кадр, задача не даёт себе начаться.
    if (tasked && !switched && now - puppet.taskedAt < kRetaskInterval) {
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

    // Скорость задаче отдаётся та же, что и походке, — и это то самое место,
    // где они расходились.
    //
    // Задача ждёт не метров в секунду, а номера походки: единица — шаг, двойка
    // — бег, тройка — во весь дух. Отдавались же ей метры в секунду. Бегущий со
    // своими тремя с половиной получал задачу «во весь дух» и походку «бег»:
    // ноги отыгрывали одно, а перемещало его другое, и персонаж ехал по земле,
    // не попадая шагом в собственное движение. Целящегося это не касалось
    // никогда — его задача с самого начала получала номер походки, — и именно
    // поэтому со стороны выходило, что криво идут все, кроме целящихся.
    invokeNative<void>(taskGoTo_, puppet.ped, target.x, target.y, target.z, blendForSpeed(speed),
                       kTaskTimeout, player.state.heading, kNoSliding);
}

void RemotePlayers::walk(Puppet& puppet, const RemotePlayerView& player, float seconds,
                         std::int32_t now, bool busy) const {
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

    const float speed = length(player.state.velocity);
    const bool moving = speed >= kStandingSpeed;

    if (busy) {
        // Персонаж занят своим движением: прыгает, лезет, сидит в укрытии. Ни
        // поворачивать его, ни задавать ему походку в это время нельзя —
        // движение ведёт его само, и всякое наше распоряжение его прервёт.
        // Положение выше подведено, и этого довольно.
        puppet.taskedAt = 0;
        return;
    }

    turn(puppet, player, moving, now);

    invokeNative<void>(moveBlend_, puppet.ped, blendForSpeed(speed));

    if (!moving) {
        // Стоящему задача движения не нужна: без неё он просто стоит, а с ней
        // топтался бы на месте, пытаясь дойти до цели, которой мы его не
        // снабдили.
        //
        // Но снимать её нельзя, если персонаж в это самое мгновение начал
        // перезаряжаться, и это была настоящая поломка. Перезарядка — тоже
        // задача, и выданная выше, в applyPosture, она заняла место задачи
        // ходьбы. Зачистка здесь сносила её в том же кадре, в котором она
        // началась, — то есть перезарядки со стороны не было видно ни разу у
        // того, кто остановился, чтобы перезарядиться. А останавливаются ради
        // этого почти все.
        if (puppet.taskedAt != 0) {
            if (!shared::has(player.state.flags, shared::PlayerFlag::Reloading)) {
                // Не немедленной зачисткой, а обычной: немедленная обрывает
                // движение там, где оно есть, и остановившийся человек застывал
                // на полушаге. Обычная даёт игре доиграть шаг и перейти в
                // стойку.
                animation_.clearTasks(puppet.ped);
            }

            // Задача в любом случае больше не наша: либо мы её сняли, либо её
            // заняла перезарядка.
            puppet.taskedAt = 0;
        }
        return;
    }

    steer(puppet, player, speed, now);
}

void RemotePlayers::equip(Puppet& puppet, const RemotePlayerView& player) const {
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

            // Насадки и расцветка — сразу за стволом и только за ним:
            // поставленные на оружие, которого у персонажа ещё нет, игра
            // проглатывает молча. Без них у всех вокруг оружие всегда выглядело
            // заводским, как бы они его ни собрали.
            if (const auto gun = guns_.find(player.id);
                gun != guns_.end() && gun->second.weapon == puppet.weapon) {
                if (giveComponent_ != nullptr) {
                    for (const std::uint32_t component : gun->second.components) {
                        if (component != 0) {
                            invokeNative<void>(giveComponent_, puppet.ped, puppet.weapon,
                                               component);
                        }
                    }
                }

                if (setWeaponTint_ != nullptr) {
                    invokeNative<void>(setWeaponTint_, puppet.ped, puppet.weapon,
                                       static_cast<int>(gun->second.tint));
                }
            }

            puppet.ammo = player.state.ammo;
        }
    } else if (puppet.weapon != 0 && player.state.ammo != puppet.ammo && setAmmo_ != nullptr) {
        // Патроны догоняются отдельно и без выдачи оружия: выдача сбрасывает
        // позу, а патроны меняются каждым выстрелом.
        invokeNative<void>(setAmmo_, puppet.ped, puppet.weapon,
                           static_cast<int>(player.state.ammo));

        puppet.ammo = player.state.ammo;
    }
}

void RemotePlayers::aim(Puppet& puppet, const RemotePlayerView& player) const {
    if (taskAim_ == nullptr && taskShoot_ == nullptr) {
        return;
    }

    if (!aiming(player.state)) {
        return;
    }

    // Перезаряжающийся оружие не наводит — он его опускает и меняет магазин. И
    // дело здесь не в правдоподобии: перезарядка выдана задачей, а задача
    // прицела, выданная каждый кадр поверх неё, сносила бы её без конца.
    //
    // Признак прицела при этом стоит: человек держит правую кнопку всю
    // перезарядку и целится сразу, как она кончится. Поэтому спрашиваем не
    // «целится ли он», а «занят ли он сейчас другим».
    if (shared::has(player.state.flags, shared::PlayerFlag::Reloading)) {
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
    if (shooting && ownFire(puppet) && taskShoot_ != nullptr) {
        invokeNative<void>(taskShoot_, puppet.ped, player.state.aimAt.x, player.state.aimAt.y,
                           player.state.aimAt.z, kAimTaskDuration, kFiringPatternFullAuto);
        return;
    }

    if (taskAim_ != nullptr) {
        invokeNative<void>(taskAim_, puppet.ped, player.state.aimAt.x, player.state.aimAt.y,
                           player.state.aimAt.z, kAimTaskDuration, false, false);
    }
}

void RemotePlayers::look(Puppet& puppet, const RemotePlayerView& player,
                         std::int32_t now) const {
    if (taskLookAt_ == nullptr || puppet.ped == 0) {
        return;
    }

    // Целящийся смотрит туда, куда целится, и голову ему поворачивает задача
    // прицела. Вторая задача взгляда с ней бы спорила.
    if (aiming(player.state)) {
        return;
    }

    // Обмякшее тело головы не поворачивает: ею распоряжается физика.
    if (shared::has(player.state.flags, shared::PlayerFlag::Ragdoll)) {
        return;
    }

    // Точка взгляда обязана быть рядом с самим игроком, иначе это не взгляд.
    //
    // Проверка не от испорченного пакета, а от честного нуля: клиент, который
    // точку не заполняет вовсе, шлёт её нулевой — и кукла уставилась бы в
    // начало координат, то есть в море под Лос-Сантосом. Выглядело бы это как
    // «все смотрят в одну сторону», и объяснить это было бы нечем.
    if (distanceBetween(player.state.position, player.state.aimAt) > kLookReach) {
        return;
    }

    // Задача выдаётся заново не каждый кадр, а по сроку и по повороту. Каждый
    // кадр — значит начинать поворот головы заново тридцать раз в секунду:
    // голова замирает, не дойдя и до половины.
    const bool stale = now - puppet.lookedAt >= kLookRefresh;
    const bool moved = distanceBetween(puppet.lookAt, player.state.aimAt) >= kRelookDistance;

    if (puppet.lookedAt != 0 && !stale && !moved) {
        return;
    }

    puppet.lookAt = player.state.aimAt;
    puppet.lookedAt = now;

    // Срок задачи чуть длиннее промежутка между выдачами: кончившаяся раньше
    // следующей выдачи роняет голову прямо, и получается подёргивание.
    //
    // Последние два довода — насколько сильно поворачивается голова и какова
    // важность задачи. Единица и двойка означают «поворачивать и головой, и
    // корпусом понемногу» и «важнее обычного»: без второго взгляд отменяется
    // всякой мелочью, которую игра придумает персонажу сама.
    invokeNative<void>(taskLookAt_, puppet.ped, player.state.aimAt.x, player.state.aimAt.y,
                       player.state.aimAt.z, kLookTaskDuration, kLookFlags, kLookPriority);
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

    // Внешность и сборка оружия — тоже забываются, и это не опрятность.
    // Ключ у них — номер игрока, а номера выдаёт сервер: на следующем сервере
    // под тем же номером будет другой человек, и до своего объявления
    // внешности он вышел бы в мир одетым в чужое и с чужим телом.
    looks_.clear();
    guns_.clear();

    // А вот о моделях, которые не грузятся, забывать незачем: это свойство
    // самой игры, а не сессии, и следующий сервер его не изменит.
}

} // namespace oxymp::client::game
