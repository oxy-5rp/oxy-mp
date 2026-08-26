#include "vehicle_snapshot.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <oxymp/client/interpolation.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace oxymp::client::game {
namespace {

/// Состояния крыши кабриолета в нумерации игры: поднята, опускается, опущена,
/// поднимается.
constexpr int kRoofLowering = 1;
constexpr int kRoofDown = 2;

/// Состояния шасси в нумерации игры: выпущено, убирается, выпускается, убрано.
/// Числа те же, что и у крыши, но натив другой, и общего имени у них нет.
constexpr int kGearRetracting = 1;
constexpr int kGearRetracted = 3;

/// Что заказывают шасси: выпустить или убрать. Промежуточные два игра принимает
/// тоже, но заказывать их незачем — она сама через них проходит.
constexpr int kGearDeploy = 0;
constexpr int kGearRetract = 1;

/// На сколько заводится гудок за один раз.
///
/// Заводится он заново каждый кадр, пока признак стоит, — своего «перестань» у
/// игры нет. Сто миллисекунд: меньше — и гудок рвался бы между кадрами на
/// просевшей частоте, больше — и он тянулся бы заметно дольше чужого пальца на
/// клавише.
constexpr int kHornBurst = 100;

/// Лад гудка. Ноль — тот, что у машины свой; так его зовут и скрипты игры.
constexpr int kDefaultHornMode = 0;

/// Порядок углов поворота. Двойка — тот же, что и у камеры, и тот, в котором
/// игра отдаёт и принимает поворот машины без пересчёта.
constexpr int kRotationOrder = 2;

/// Набор органов управления игрока.
constexpr int kPlayerControls = 0;

/// Органы управления машиной в нумерации игры.
constexpr int kInputSteer = 59;
constexpr int kInputAccelerate = 71;
constexpr int kInputBrake = 72;
constexpr int kInputHandbrake = 76;

/// Насколько машина должна разойтись со снимком, чтобы её переставить рывком.
///
/// Обычно расхождение закрывается плавно, но после потери связи или въезда в
/// туннель машина оказывается за сотню метров, и доводить её туда плавно — это
/// показать, как она едет сквозь дома.
constexpr float kSnapDistance = 15.0F;

/// На каком расстоянии игра соглашается сцепить прицеп с тягачом, в метрах.
///
/// С запасом: у хозяина они уже сцеплены, а у нас обе машины стоят там, куда их
/// привели снимки, и между ними бывает метр расхождения.
constexpr float kTrailerReach = 15.0F;

/// Смещение крюка эвакуатора. Ноль: машина вешается туда, куда её вешает игра.
constexpr float kNoHookOffset = 0.0F;

/// Во сколько раз сокращать расхождение за секунду, когда оно невелико.
///
/// За секунду, а не за кадр, и это не придирка: доля за кадр съедает расхождение
/// вдвое быстрее при шестидесяти кадрах, чем при тридцати, и одна и та же машина
/// у двух зрителей едет по-разному. Число подобрано так, чтобы при обычных
/// шестидесяти кадрах выходила прежняя треть расхождения за кадр.
constexpr float kCorrectionRate = 26.0F;

/// Целая прочность в нумерации игры.
constexpr float kFullHealth = 1000.0F;

/// С какого угла дверь считается открытой.
///
/// Не ноль: закрытая дверь возвращает не строго нулевой угол, и сравнение с
/// нулём объявляло бы открытыми все двери подряд.
constexpr float kDoorOpenAngle = 0.05F;

/// Состояния фар для SET_VEHICLE_LIGHTS.
constexpr int kLightsForcedOff = 1;
constexpr int kLightsForcedOn = 2;

/// Места тюнинга, которые не выбираются из списка, а включаются переключателем.
///
/// Восемнадцать — турбина, двадцать — дым из-под колёс, двадцать два — ксенон.
/// Спрашивать о них тем же нативом, что и об остальных, нельзя: он отвечает про
/// выбор из списка, которого у них нет.
/// Стороны неона в том порядке, в каком их нумерует игра.
constexpr std::array<shared::NeonSide, 4> kNeonSides{
    shared::NeonSide::Left,
    shared::NeonSide::Right,
    shared::NeonSide::Front,
    shared::NeonSide::Back,
};

/// Какими числами игра называет дополнения кузова.
///
/// С первого по четырнадцатое: нулевого у неё нет, а больше четырнадцати не
/// бывает ни у одной модели.
constexpr int kFirstExtra = 1;
constexpr int kLastExtra = 14;

/// Место дисков, у которого спрашивают про не заводские покрышки.
constexpr int kFrontWheelSlot = 23;

constexpr int kToggleModSlots[] = {18, 20, 22};

/// Прочность из числа игры в число протокола.
///
/// Прочность двигателя уходит в минус, когда он разбит вдребезги, а по сети
/// едет беззнаковой: отрицательное и ноль означают одно и то же — сломан.
std::uint16_t packHealth(float value) {
    return static_cast<std::uint16_t>(std::clamp(value, 0.0F, kFullHealth));
}

float length(const shared::Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

shared::Vec3 readVector(NativeHandler handler, int entity) {
    NativeContext context;
    context.push(entity);
    handler(context.address());

    return shared::Vec3{context.result<float>(0), context.result<float>(1),
                        context.result<float>(2)};
}

} // namespace

VehicleSnapshot::VehicleSnapshot(const NativeTable& table) noexcept
    : getCoords_(table.handlerFor(natives::kGetEntityCoords)),
      setCoords_(table.handlerFor(natives::kSetEntityCoordsNoOffset)),
      getRotation_(table.handlerFor(natives::kGetEntityRotation)),
      setRotation_(table.handlerFor(natives::kSetEntityRotation)),
      getVelocity_(table.handlerFor(natives::kGetEntityVelocity)),
      setVelocity_(table.handlerFor(natives::kSetEntityVelocity)),
      getAngularVelocity_(table.handlerFor(natives::kGetEntityRotationVelocity)),
      getModel_(table.handlerFor(natives::kGetEntityModel)),
      controlNormal_(table.handlerFor(natives::kGetControlNormal)),
      controlPressed_(table.handlerFor(natives::kIsControlPressed)),
      getBodyHealth_(table.handlerFor(natives::kGetVehicleBodyHealth)),
      setBodyHealth_(table.handlerFor(natives::kSetVehicleBodyHealth)),
      getEngineHealth_(table.handlerFor(natives::kGetVehicleEngineHealth)),
      setEngineHealth_(table.handlerFor(natives::kSetVehicleEngineHealth)),
      getTankHealth_(table.handlerFor(natives::kGetVehiclePetrolTankHealth)),
      setTankHealth_(table.handlerFor(natives::kSetVehiclePetrolTankHealth)),
      engineRunning_(table.handlerFor(natives::kGetIsVehicleEngineRunning)),
      setEngineOn_(table.handlerFor(natives::kSetVehicleEngineOn)),
      lightsState_(table.handlerFor(natives::kGetVehicleLightsState)),
      setLights_(table.handlerFor(natives::kSetVehicleLights)),
      setFullBeam_(table.handlerFor(natives::kSetVehicleFullbeam)),
      sirenOn_(table.handlerFor(natives::kIsVehicleSirenOn)),
      setSiren_(table.handlerFor(natives::kSetVehicleSiren)),
      trailerOf_(table.handlerFor(natives::kGetVehicleTrailerVehicle)),
      hookedOf_(table.handlerFor(natives::kGetEntityAttachedToTowTruck)),
      attachHook_(table.handlerFor(natives::kAttachVehicleToTowTruck)),
      detachHook_(table.handlerFor(natives::kDetachVehicleFromTowTruck)),
      hookAttached_(table.handlerFor(natives::kIsVehicleAttachedToTowTruck)),
      attachTrailer_(table.handlerFor(natives::kAttachVehicleToTrailer)),
      detachTrailer_(table.handlerFor(natives::kDetachVehicleFromTrailer)),
      trailerAttached_(table.handlerFor(natives::kIsVehicleAttachedToTrailer)),
      wrecked_(table.handlerFor(natives::kIsEntityDead)),
      explode_(table.handlerFor(natives::kExplodeVehicle)),
      invincible_(table.handlerFor(natives::kSetEntityInvincible)),
      hornActive_(table.handlerFor(natives::kIsHornActive)),
      startHorn_(table.handlerFor(natives::kStartVehicleHorn)),
      roofState_(table.handlerFor(natives::kGetConvertibleRoofState)),
      raiseRoof_(table.handlerFor(natives::kRaiseConvertibleRoof)),
      lowerRoof_(table.handlerFor(natives::kLowerConvertibleRoof)),
      convertible_(table.handlerFor(natives::kIsVehicleAConvertible)),
      fix_(table.handlerFor(natives::kSetVehicleFixed)),
      fixDeformation_(table.handlerFor(natives::kSetVehicleDeformationFixed)),
      landingGear_(table.handlerFor(natives::kGetLandingGearState)),
      setLandingGear_(table.handlerFor(natives::kControlLandingGear)),
      hasLandingGear_(table.handlerFor(natives::kVehicleHasLandingGear)),
      setSteerBias_(table.handlerFor(natives::kSetVehicleSteerBias)),
      setHandbrake_(table.handlerFor(natives::kSetVehicleHandbrake)),
      setBrakeLights_(table.handlerFor(natives::kSetVehicleBrakeLights)),
      doorAngle_(table.handlerFor(natives::kGetVehicleDoorAngleRatio)),
      openDoor_(table.handlerFor(natives::kSetVehicleDoorOpen)),
      shutDoor_(table.handlerFor(natives::kSetVehicleDoorShut)),
      doorDamaged_(table.handlerFor(natives::kIsVehicleDoorDamaged)),
      breakDoor_(table.handlerFor(natives::kSetVehicleDoorBroken)),
      windowIntact_(table.handlerFor(natives::kIsVehicleWindowIntact)),
      smashWindow_(table.handlerFor(natives::kSmashVehicleWindow)),
      tyreBurst_(table.handlerFor(natives::kIsVehicleTyreBurst)),
      burstTyre_(table.handlerFor(natives::kSetVehicleTyreBurst)),
      fixTyre_(table.handlerFor(natives::kSetVehicleTyreFixed)),
      getColours_(table.handlerFor(natives::kGetVehicleColours)),
      setColours_(table.handlerFor(natives::kSetVehicleColours)),
      setCustomPrimary_(table.handlerFor(natives::kSetVehicleCustomPrimaryColour)),
      setCustomSecondary_(table.handlerFor(natives::kSetVehicleCustomSecondaryColour)),
      clearCustomPrimary_(table.handlerFor(natives::kClearVehicleCustomPrimaryColour)),
      clearCustomSecondary_(table.handlerFor(natives::kClearVehicleCustomSecondaryColour)),
      getExtraColours_(table.handlerFor(natives::kGetVehicleExtraColours)),
      setExtraColours_(table.handlerFor(natives::kSetVehicleExtraColours)),
      getPlate_(table.handlerFor(natives::kGetVehicleNumberPlateText)),
      setPlate_(table.handlerFor(natives::kSetVehicleNumberPlateText)),
      getPlateStyle_(table.handlerFor(natives::kGetVehicleNumberPlateTextIndex)),
      setPlateStyle_(table.handlerFor(natives::kSetVehicleNumberPlateTextIndex)),
      getLivery_(table.handlerFor(natives::kGetVehicleLivery)),
      setLivery_(table.handlerFor(natives::kSetVehicleLivery)),
      getDirt_(table.handlerFor(natives::kGetVehicleDirtLevel)),
      getNeonColour_(table.handlerFor(natives::kGetVehicleNeonLightsColour)),
      setNeonColour_(table.handlerFor(natives::kSetVehicleNeonLightsColour)),
      neonOn_(table.handlerFor(natives::kIsVehicleNeonLightEnabled)),
      setNeonOn_(table.handlerFor(natives::kSetVehicleNeonLightEnabled)),
      getTyreSmoke_(table.handlerFor(natives::kGetVehicleTyreSmokeColor)),
      setTyreSmoke_(table.handlerFor(natives::kSetVehicleTyreSmokeColor)),
      extraOn_(table.handlerFor(natives::kIsVehicleExtraTurnedOn)),
      setExtra_(table.handlerFor(natives::kSetVehicleExtra)),
      extraExists_(table.handlerFor(natives::kDoesExtraExist)),
      getModVariation_(table.handlerFor(natives::kGetVehicleModVariation)),
      setDirt_(table.handlerFor(natives::kSetVehicleDirtLevel)),
      getMod_(table.handlerFor(natives::kGetVehicleMod)),
      setMod_(table.handlerFor(natives::kSetVehicleMod)),
      setModKit_(table.handlerFor(natives::kSetVehicleModKit)),
      toggleMod_(table.handlerFor(natives::kToggleVehicleMod)),
      toggleModOn_(table.handlerFor(natives::kIsToggleModOn)),
      getWheelType_(table.handlerFor(natives::kGetVehicleWheelType)),
      setWheelType_(table.handlerFor(natives::kSetVehicleWheelType)),
      getWindowTint_(table.handlerFor(natives::kGetVehicleWindowTint)),
      setWindowTint_(table.handlerFor(natives::kSetVehicleWindowTint)) {}

bool VehicleSnapshot::ready() const noexcept {
    return getCoords_ != nullptr && setCoords_ != nullptr && getRotation_ != nullptr &&
           setRotation_ != nullptr && getVelocity_ != nullptr && setVelocity_ != nullptr &&
           getModel_ != nullptr;
}

shared::VehicleState VehicleSnapshot::read(int vehicle, bool driving) const {
    shared::VehicleState state;

    if (!ready() || vehicle == 0) {
        return state;
    }

    state.model = invokeNative<std::uint32_t>(getModel_, vehicle);

    {
        NativeContext context;
        context.push(vehicle);
        context.push(true); // учитывать смещение модели
        getCoords_(context.address());

        state.position = shared::Vec3{context.result<float>(0), context.result<float>(1),
                                      context.result<float>(2)};
    }

    {
        NativeContext context;
        context.push(vehicle);
        context.push(kRotationOrder);
        getRotation_(context.address());

        state.rotation = shared::Vec3{context.result<float>(0), context.result<float>(1),
                                      context.result<float>(2)};
    }

    state.velocity = readVector(getVelocity_, vehicle);

    if (getAngularVelocity_ != nullptr) {
        state.angularVelocity = readVector(getAngularVelocity_, vehicle);
    }

    if (getBodyHealth_ != nullptr) {
        state.bodyHealth = packHealth(invokeNative<float>(getBodyHealth_, vehicle));
    }
    if (getEngineHealth_ != nullptr) {
        state.engineHealth = packHealth(invokeNative<float>(getEngineHealth_, vehicle));
    }
    if (getTankHealth_ != nullptr) {
        state.tankHealth = packHealth(invokeNative<float>(getTankHealth_, vehicle));
    }

    // Руль, газ и тормоз — не свойства машины, а ввод того, кто за рулём. Именно
    // поэтому они и читаются отсюда: у самой машины игра о них не спрашивает, а
    // FiveM достаёт их из её памяти по смещениям, которые живут до ближайшего
    // обновления игры.
    if (driving && controlNormal_ != nullptr) {
        state.steer = invokeNative<float>(controlNormal_, kPlayerControls, kInputSteer);
        state.throttle = invokeNative<float>(controlNormal_, kPlayerControls, kInputAccelerate);
        state.brake = invokeNative<float>(controlNormal_, kPlayerControls, kInputBrake);
    }

    std::uint16_t flags = 0;

    const auto set = [&flags](shared::VehicleFlag flag, bool on) {
        if (on) {
            flags |= static_cast<std::uint16_t>(flag);
        }
    };

    if (engineRunning_ != nullptr) {
        set(shared::VehicleFlag::EngineOn, invokeNative<bool>(engineRunning_, vehicle));
    }
    if (sirenOn_ != nullptr) {
        set(shared::VehicleFlag::SirenOn, invokeNative<bool>(sirenOn_, vehicle));
    }
    if (driving && controlPressed_ != nullptr) {
        set(shared::VehicleFlag::Handbrake,
            invokeNative<bool>(controlPressed_, kPlayerControls, kInputHandbrake));
    }

    if (wrecked_ != nullptr) {
        set(shared::VehicleFlag::Destroyed, invokeNative<bool>(wrecked_, vehicle));
    }

    if (hornActive_ != nullptr) {
        set(shared::VehicleFlag::HornOn, invokeNative<bool>(hornActive_, vehicle));
    }

    if (roofState_ != nullptr) {
        // Игра держит четыре состояния, а по сети едет одно: куда крыша едет.
        // Опущенная и опускающаяся — обе «открыта», поднятая и поднимающаяся —
        // обе «закрыта». Саму дорогу получатель отыграет тем же движением.
        const int roof = invokeNative<int>(roofState_, vehicle);

        set(shared::VehicleFlag::RoofOpen, roof == kRoofLowering || roof == kRoofDown);
    }

    if (landingGear_ != nullptr &&
        (hasLandingGear_ == nullptr || invokeNative<bool>(hasLandingGear_, vehicle))) {
        // Спрашивать состояние у машины без шасси нельзя: натив читает его из
        // части, которой у неё нет, и отвечает мусором. Отсюда проверка перед
        // вопросом — у крыши ту же роль играет IS_VEHICLE_A_CONVERTIBLE.
        const int gear = invokeNative<int>(landingGear_, vehicle);

        set(shared::VehicleFlag::LandingGearUp,
            gear == kGearRetracting || gear == kGearRetracted);
    }

    if (lightsState_ != nullptr) {
        // Оба ответа приходят ссылками: натив возвращает не одно число, а два
        // признака, и места под них нужно отдать ему свои.
        int lights = 0;
        int highBeams = 0;

        NativeContext context;
        context.push(vehicle);
        context.push(&lights);
        context.push(&highBeams);
        lightsState_(context.address());

        set(shared::VehicleFlag::LightsOn, lights != 0);
        set(shared::VehicleFlag::HighBeams, highBeams != 0);
    }

    state.flags = flags;

    for (int door = 0; door < shared::kVehicleDoorCount; ++door) {
        const auto bit = static_cast<std::uint8_t>(1U << door);

        if (doorAngle_ != nullptr &&
            invokeNative<float>(doorAngle_, vehicle, door) > kDoorOpenAngle) {
            state.doorsOpen |= bit;
        }
        if (doorDamaged_ != nullptr && invokeNative<bool>(doorDamaged_, vehicle, door)) {
            state.doorsBroken |= bit;
        }
    }

    for (int window = 0; window < shared::kVehicleWindowCount; ++window) {
        if (windowIntact_ != nullptr && !invokeNative<bool>(windowIntact_, vehicle, window)) {
            state.windowsBroken |= static_cast<std::uint8_t>(1U << window);
        }
    }

    for (int wheel = 0; wheel < shared::kVehicleWheelCount; ++wheel) {
        // Последний довод — считать ли пробитым только разорванное в клочья.
        // Нет: спущенное колесо тоже видно со стороны.
        if (tyreBurst_ != nullptr && invokeNative<bool>(tyreBurst_, vehicle, wheel, false)) {
            state.tyresBurst |= static_cast<std::uint8_t>(1U << wheel);
        }
    }

    return state;
}

shared::VehicleAppearance VehicleSnapshot::readAppearance(int vehicle) const {
    shared::VehicleAppearance appearance;

    if (vehicle == 0) {
        return appearance;
    }

    if (getColours_ != nullptr) {
        int primary = 0;
        int secondary = 0;

        NativeContext context;
        context.push(vehicle);
        context.push(&primary);
        context.push(&secondary);
        getColours_(context.address());

        appearance.primaryColour = static_cast<std::uint8_t>(primary);
        appearance.secondaryColour = static_cast<std::uint8_t>(secondary);
    }

    if (getExtraColours_ != nullptr) {
        int pearlescent = 0;
        int wheel = 0;

        NativeContext context;
        context.push(vehicle);
        context.push(&pearlescent);
        context.push(&wheel);
        getExtraColours_(context.address());

        appearance.pearlescentColour = static_cast<std::uint8_t>(pearlescent);
        appearance.wheelColour = static_cast<std::uint8_t>(wheel);
    }

    if (getPlate_ != nullptr) {
        const char* plate = invokeNative<const char*>(getPlate_, vehicle);

        if (plate != nullptr) {
            appearance.plate.assign(plate);

            if (appearance.plate.size() > shared::kMaxPlateLength) {
                appearance.plate.resize(shared::kMaxPlateLength);
            }
        }
    }

    if (getPlateStyle_ != nullptr) {
        appearance.plateStyle = static_cast<std::uint8_t>(invokeNative<int>(getPlateStyle_, vehicle));
    }
    if (getLivery_ != nullptr) {
        appearance.livery = static_cast<std::int8_t>(invokeNative<int>(getLivery_, vehicle));
    }
    if (getWheelType_ != nullptr) {
        appearance.wheelType = static_cast<std::int8_t>(invokeNative<int>(getWheelType_, vehicle));
    }
    if (getWindowTint_ != nullptr) {
        appearance.windowTint = static_cast<std::int8_t>(invokeNative<int>(getWindowTint_, vehicle));
    }
    if (getDirt_ != nullptr) {
        appearance.dirtLevel = invokeNative<float>(getDirt_, vehicle);
    }

    if (getMod_ != nullptr) {
        for (int slot = 0; slot < shared::kVehicleModSlots; ++slot) {
            appearance.mods[static_cast<std::size_t>(slot)] =
                static_cast<std::int8_t>(invokeNative<int>(getMod_, vehicle, slot));
        }
    }

    if (toggleModOn_ != nullptr) {
        for (const int slot : kToggleModSlots) {
            if (invokeNative<bool>(toggleModOn_, vehicle, slot)) {
                appearance.toggleMods |= 1U << static_cast<std::uint32_t>(slot);
            }
        }
    }

    // Неон: четыре стороны по одной и общий цвет.
    if (neonOn_ != nullptr) {
        for (std::size_t side = 0; side < kNeonSides.size(); ++side) {
            if (invokeNative<bool>(neonOn_, vehicle, static_cast<int>(side))) {
                appearance.neonSides |= static_cast<std::uint8_t>(kNeonSides[side]);
            }
        }
    }

    if (getNeonColour_ != nullptr) {
        int red = 0;
        int green = 0;
        int blue = 0;

        NativeContext context;
        context.push(vehicle);
        context.push(&red);
        context.push(&green);
        context.push(&blue);
        getNeonColour_(context.address());

        appearance.neonRed = static_cast<std::uint8_t>(red);
        appearance.neonGreen = static_cast<std::uint8_t>(green);
        appearance.neonBlue = static_cast<std::uint8_t>(blue);
    }

    if (getTyreSmoke_ != nullptr) {
        int red = 0;
        int green = 0;
        int blue = 0;

        NativeContext context;
        context.push(vehicle);
        context.push(&red);
        context.push(&green);
        context.push(&blue);
        getTyreSmoke_(context.address());

        appearance.tyreSmokeRed = static_cast<std::uint8_t>(red);
        appearance.tyreSmokeGreen = static_cast<std::uint8_t>(green);
        appearance.tyreSmokeBlue = static_cast<std::uint8_t>(blue);
    }

    // Дополнения кузова: лестницы, багажники, антенны. Спрашиваются только те,
    // что у модели есть, — у остальных игра отвечает как придётся.
    if (extraOn_ != nullptr && extraExists_ != nullptr) {
        for (int extra = kFirstExtra; extra <= kLastExtra; ++extra) {
            if (!invokeNative<bool>(extraExists_, vehicle, extra)) {
                continue;
            }

            if (invokeNative<bool>(extraOn_, vehicle, extra)) {
                appearance.extras |= static_cast<std::uint16_t>(1U << (extra - kFirstExtra));
            }
        }
    }

    // Не заводские покрышки — свойство места дисков, а не отдельная вещь.
    if (getModVariation_ != nullptr) {
        appearance.customTyres = invokeNative<bool>(getModVariation_, vehicle, kFrontWheelSlot);
    }

    return appearance;
}

VehicleSnapshot::Hitched VehicleSnapshot::hitchedTo(int vehicle) const {
    if (vehicle == 0) {
        return {};
    }

    // Сперва сцепка: она у тягачей обычнее крюка, и спрашивается дешевле.
    if (trailerOf_ != nullptr) {
        // Ответ приходит ссылкой: натив возвращает не прицеп, а признак «есть ли
        // он», а сам прицеп кладёт в отданное ему место.
        int trailer = 0;

        NativeContext context;
        context.push(vehicle);
        context.push(&trailer);
        trailerOf_(context.address());

        // Признак «есть ли прицеп» и сам прицеп — разные вещи: натив вернул бы
        // прошлое содержимое отданного ему места, не найдя ничего.
        if (context.result<int>() != 0 && trailer != 0) {
            return Hitched{.vehicle = trailer, .onHook = false};
        }
    }

    // Затем крюк. Спрашивается он у всякой машины, а не только у эвакуатора:
    // отличить эвакуатор от прочих можно лишь по модели, а список моделей — это
    // список, который придётся вести вечно. Не эвакуатору натив отвечает нулём,
    // и этого довольно.
    if (hookedOf_ != nullptr) {
        if (const int hooked = invokeNative<int>(hookedOf_, vehicle); hooked != 0) {
            return Hitched{.vehicle = hooked, .onHook = true};
        }
    }

    return {};
}

void VehicleSnapshot::applyHitch(int vehicle, const Hitched& hitched, bool had) const {
    if (vehicle == 0) {
        return;
    }

    if (hitched.vehicle == 0) {
        // Расцепляем только на переходе: игра сцепляет прицеп сама, стоит
        // тягачу подать назад, и расцепляй мы каждый кадр — у зрителя сцепка
        // разваливалась бы в тот самый миг, когда у хозяина она сложилась.
        //
        // Снимаем и со сцепки, и с крюка: чем машина была занята, мы уже не
        // знаем — снимок сказал лишь, что теперь ничем.
        if (!had) {
            return;
        }

        if (detachTrailer_ != nullptr) {
            invokeNative<void>(detachTrailer_, vehicle);
        }

        if (detachHook_ != nullptr && hookAttached_ != nullptr &&
            invokeNative<bool>(hookAttached_, vehicle)) {
            // Натив снятия с крюка требует обе машины, а какая на нём висела,
            // спрашивается у самой игры: снимок этого больше не помнит.
            if (hookedOf_ != nullptr) {
                if (const int hooked = invokeNative<int>(hookedOf_, vehicle); hooked != 0) {
                    invokeNative<void>(detachHook_, vehicle, hooked);
                }
            }
        }

        return;
    }

    // А сцепляем, наоборот, пока не выйдет: прицеп мог ещё не появиться у нас,
    // а машины — не сойтись достаточно близко. Проверка каждый кадр стоит
    // одного натива и избавляет от памяти о том, удалась ли попытка.
    if (hitched.onHook) {
        const bool hung =
            hookAttached_ != nullptr && invokeNative<bool>(hookAttached_, vehicle);

        if (hung || attachHook_ == nullptr) {
            return;
        }

        // Сзади и без смещения крюка: смещение задают лишь те, кто вешает
        // машину нарочно криво, а у нас она уже висит у хозяина так, как висит.
        invokeNative<void>(attachHook_, vehicle, hitched.vehicle, true, kNoHookOffset,
                           kNoHookOffset, kNoHookOffset);
        return;
    }

    const bool attached =
        trailerAttached_ != nullptr && invokeNative<bool>(trailerAttached_, vehicle);

    if (attached || attachTrailer_ == nullptr) {
        // Уже сцеплен. Сцеплять заново каждый кадр нельзя: игра при этом
        // подтягивает прицеп к тягачу рывком, и на ходу это выглядит как
        // дёргающийся на сцепке прицеп.
        //
        // Что сцеплен именно с тем, с кем сказано, здесь не проверяется, и
        // проверить это нечем: натив отвечает только «сцеплен или нет». Случай
        // же, когда тягач подцепил чужой прицеп, разрешается сам: у хозяина
        // сцепка одна, и следующий его снимок скажет о ней.
        return;
    }

    // Радиус — то расстояние, на котором игра считает сцепку возможной. С
    // запасом: у хозяина прицеп уже сцеплен, а у нас обе машины стоят там, куда
    // их привели снимки, и между ними бывает метр расхождения.
    invokeNative<void>(attachTrailer_, vehicle, hitched.vehicle, kTrailerReach);
}

bool VehicleSnapshot::tooFar(const shared::Vec3& from, const shared::Vec3& to) {
    return length(shared::Vec3{to.x - from.x, to.y - from.y, to.z - from.z}) > kSnapDistance;
}

void VehicleSnapshot::applyMotion(int vehicle, const shared::VehicleState& state,
                                  float seconds) const {
    if (!ready() || vehicle == 0) {
        return;
    }

    NativeContext coords;
    coords.push(vehicle);
    coords.push(true);
    getCoords_(coords.address());

    const shared::Vec3 actual{coords.result<float>(0), coords.result<float>(1),
                              coords.result<float>(2)};

    // Далеко разошлись — ставим рывком. Вблизи — подводим понемногу: машина при
    // этом остаётся физическим телом, её колёса крутятся, а подвеска
    // отрабатывает дорогу. Ставить её точно по снимку каждый кадр означало бы
    // отнять у неё физику и получить скользящую по земле коробку.
    const float share = interpolation::catchUp(kCorrectionRate, seconds);

    const shared::Vec3 target =
        tooFar(actual, state.position)
            ? state.position
            : shared::Vec3{std::lerp(actual.x, state.position.x, share),
                           std::lerp(actual.y, state.position.y, share),
                           std::lerp(actual.z, state.position.z, share)};

    invokeNative<void>(setCoords_, vehicle, target.x, target.y, target.z, false, false, false);

    // Поворот приходит уже посчитанным на это мгновение — доводить его здесь
    // нечего. Считается он там же, где и положение: у обоих одно и то же время,
    // и разойтись им нельзя.
    invokeNative<void>(setRotation_, vehicle, state.rotation.x, state.rotation.y, state.rotation.z,
                       kRotationOrder, true);

    // Скорость задаётся своя, а не выводится из перемещения: по ней игра крутит
    // колёса, наклоняет кузов и решает, реветь ли двигателю.
    invokeNative<void>(setVelocity_, vehicle, state.velocity.x, state.velocity.y,
                       state.velocity.z);
}

void VehicleSnapshot::applyControls(int vehicle, const shared::VehicleState& state,
                                    const shared::VehicleState& previous) const {
    if (vehicle == 0) {
        return;
    }

    // Руль и тормоз задаются каждый кадр: они меняются непрерывно, пока машина
    // едет, и сравнивать их с прошлым значением дороже, чем задать заново.
    //
    // Именно здесь и появляется тот самый угол поворота колёс, которого раньше
    // не было: сама машина его не выведет — её ведём мы, задавая положение, а
    // не поворачивая руль.
    if (setSteerBias_ != nullptr) {
        invokeNative<void>(setSteerBias_, vehicle, state.steer);
    }

    if (setBrakeLights_ != nullptr) {
        invokeNative<void>(setBrakeLights_, vehicle, state.brake > 0.0F);
    }

    // Остальное — только на изменение. Свет, сирена и прочность меняются
    // считанные разы за поездку, а машин вокруг бывает три десятка: задавать им
    // всем одно и то же по тридцать раз в секунду значит тратить кадр на то,
    // что и так уже так.
    const bool flagsChanged = state.flags != previous.flags;

    if (flagsChanged && setHandbrake_ != nullptr) {
        invokeNative<void>(setHandbrake_, vehicle,
                           shared::has(state.flags, shared::VehicleFlag::Handbrake));
    }

    if (flagsChanged && setEngineOn_ != nullptr) {
        // Признаки: включить мгновенно, без анимации запуска, и не глушить
        // мотор, когда из машины выходят.
        invokeNative<void>(setEngineOn_, vehicle,
                           shared::has(state.flags, shared::VehicleFlag::EngineOn), true, true);
    }

    if (flagsChanged && setLights_ != nullptr) {
        invokeNative<void>(setLights_, vehicle,
                           shared::has(state.flags, shared::VehicleFlag::LightsOn)
                               ? kLightsForcedOn
                               : kLightsForcedOff);
    }

    if (flagsChanged && setFullBeam_ != nullptr) {
        invokeNative<void>(setFullBeam_, vehicle,
                           shared::has(state.flags, shared::VehicleFlag::HighBeams));
    }

    if (flagsChanged && setSiren_ != nullptr) {
        invokeNative<void>(setSiren_, vehicle,
                           shared::has(state.flags, shared::VehicleFlag::SirenOn));
    }

    // Разрушение — один раз, на переходе, и до всего остального: взорванной
    // машине ни свет, ни сирена, ни прочности уже не нужны.
    //
    // Неуязвимость перед этим снимается. Чужая машина неуязвима намеренно —
    // иначе она взорвалась бы здесь от одного выстрела и осталась бы целой у
    // того, кто в ней едет, — но неуязвимая не взрывается и по распоряжению.
    // Возвращать её обратно незачем: машина уже разбита.
    if (shared::has(state.flags, shared::VehicleFlag::Destroyed) &&
        !shared::has(previous.flags, shared::VehicleFlag::Destroyed) && explode_ != nullptr) {
        if (invincible_ != nullptr) {
            invokeNative<void>(invincible_, vehicle, false);
        }

        // Со звуком и на виду: взрыв, случившийся у хозяина, обязан выглядеть
        // так же и здесь.
        invokeNative<void>(explode_, vehicle, true, false);
        return;
    }

    // Обратный переход — и он такой же обязательный. Разбитую машину поставленные
    // прочности не поднимают: обгорелый остов так и остаётся остовом, сколько ему
    // ни задавай тысячу. Просьба о починке уходит с сервера одному ведущему, и
    // до сих пор все остальные смотрели на сгоревшую машину, пока хозяин ездил на
    // целой. А у брошенной машины ведущего нет вовсе — её не чинил никто.
    if (!shared::has(state.flags, shared::VehicleFlag::Destroyed) &&
        shared::has(previous.flags, shared::VehicleFlag::Destroyed) && fix_ != nullptr) {
        invokeNative<void>(fix_, vehicle);

        // Вмятины — отдельным вызовом: сам fix возвращает прочности и стёкла, но
        // смятое крыло оставляет как было.
        if (fixDeformation_ != nullptr) {
            invokeNative<void>(fixDeformation_, vehicle);
        }

        // И неуязвимость обратно: перед взрывом мы её сняли, а починенная машина
        // снова чужая. Оставленная снятой, она взорвалась бы здесь от первого же
        // взрыва по соседству — и осталась бы целой у того, кто в ней едет.
        if (invincible_ != nullptr) {
            invokeNative<void>(invincible_, vehicle, true);
        }
    }

    // Гудок — не на изменение, а пока признак стоит, и это не оплошность.
    // «Перестань гудеть» у игры нет вовсе: есть только «гуди столько-то». Гудок
    // поэтому заводится заново коротким сроком, и снятый признак глушит его сам
    // собой — через этот самый срок.
    if (shared::has(state.flags, shared::VehicleFlag::HornOn) && startHorn_ != nullptr) {
        invokeNative<void>(startHorn_, vehicle, kHornBurst, kDefaultHornMode, false);
    }

    // А крыша — наоборот, только на изменение: движение крыши длится секунды, и
    // заказанное повторно оно начиналось бы с начала каждый кадр.
    if (flagsChanged && raiseRoof_ != nullptr && lowerRoof_ != nullptr &&
        (convertible_ == nullptr || invokeNative<bool>(convertible_, vehicle, false))) {
        const bool open = shared::has(state.flags, shared::VehicleFlag::RoofOpen);

        // Не мгновенно: у хозяина крыша едет своим ходом, и мгновенная у
        // зрителя оказалась бы на месте за секунду до его.
        invokeNative<void>(open ? lowerRoof_ : raiseRoof_, vehicle, false);
    }

    // Шасси — тем же порядком, что и крыша: только на изменение, и только тому,
    // у кого оно есть. Заказанное каждым кадром движение начиналось бы заново, а
    // заказанное машине без шасси не сделает ничего — молча.
    if (flagsChanged && setLandingGear_ != nullptr &&
        (hasLandingGear_ == nullptr || invokeNative<bool>(hasLandingGear_, vehicle))) {
        const bool up = shared::has(state.flags, shared::VehicleFlag::LandingGearUp);

        invokeNative<void>(setLandingGear_, vehicle, up ? kGearRetract : kGearDeploy);
    }

    if (state.bodyHealth != previous.bodyHealth && setBodyHealth_ != nullptr) {
        invokeNative<void>(setBodyHealth_, vehicle, static_cast<float>(state.bodyHealth));
    }
    if (state.engineHealth != previous.engineHealth && setEngineHealth_ != nullptr) {
        invokeNative<void>(setEngineHealth_, vehicle, static_cast<float>(state.engineHealth));
    }
    if (state.tankHealth != previous.tankHealth && setTankHealth_ != nullptr) {
        invokeNative<void>(setTankHealth_, vehicle, static_cast<float>(state.tankHealth));
    }
}

void VehicleSnapshot::applyDamage(int vehicle, const shared::VehicleState& state,
                                  const shared::VehicleState& previous) const {
    if (vehicle == 0) {
        return;
    }

    for (int door = 0; door < shared::kVehicleDoorCount; ++door) {
        const auto bit = static_cast<std::uint8_t>(1U << door);

        const bool broken = (state.doorsBroken & bit) != 0;
        const bool wasBroken = (previous.doorsBroken & bit) != 0;

        if (broken && !wasBroken && breakDoor_ != nullptr) {
            // Второй довод — убрать дверь совсем или оставить валяться. Оставляем:
            // у хозяина она тоже отлетела, а не исчезла.
            invokeNative<void>(breakDoor_, vehicle, door, false);
            continue;
        }

        // Оторванную дверь открывать и закрывать бессмысленно — её нет.
        if (broken) {
            continue;
        }

        const bool open = (state.doorsOpen & bit) != 0;

        if (open == ((previous.doorsOpen & bit) != 0)) {
            continue;
        }

        if (open && openDoor_ != nullptr) {
            // Признаки: не болтается на петлях и открывается не мгновенно —
            // дверь должна распахнуться на глазах, а не оказаться открытой.
            invokeNative<void>(openDoor_, vehicle, door, false, false);
        } else if (!open && shutDoor_ != nullptr) {
            invokeNative<void>(shutDoor_, vehicle, door, false);
        }
    }

    for (int window = 0; window < shared::kVehicleWindowCount; ++window) {
        const auto bit = static_cast<std::uint8_t>(1U << window);

        if ((state.windowsBroken & bit) != 0 && (previous.windowsBroken & bit) == 0 &&
            smashWindow_ != nullptr) {
            invokeNative<void>(smashWindow_, vehicle, window);
        }
    }

    for (int wheel = 0; wheel < shared::kVehicleWheelCount; ++wheel) {
        const auto bit = static_cast<std::uint8_t>(1U << wheel);

        const bool burst = (state.tyresBurst & bit) != 0;

        if (burst == ((previous.tyresBurst & bit) != 0)) {
            continue;
        }

        if (burst && burstTyre_ != nullptr) {
            // Признаки: не до обода и повреждение полное — то же, что делает
            // с колесом обычный прострел.
            invokeNative<void>(burstTyre_, vehicle, wheel, false, kFullHealth);
        } else if (!burst && fixTyre_ != nullptr) {
            invokeNative<void>(fixTyre_, vehicle, wheel);
        }
    }
}

void VehicleSnapshot::applyAppearance(int vehicle,
                                      const shared::VehicleAppearance& appearance) const {
    if (vehicle == 0) {
        return;
    }

    if (setColours_ != nullptr) {
        invokeNative<void>(setColours_, vehicle, static_cast<int>(appearance.primaryColour),
                           static_cast<int>(appearance.secondaryColour));
    }

    if (setExtraColours_ != nullptr) {
        invokeNative<void>(setExtraColours_, vehicle,
                           static_cast<int>(appearance.pearlescentColour),
                           static_cast<int>(appearance.wheelColour));
    }

    // Своя краска ставится после номера цвета и перекрывает его — так у игры.
    // Снятие тоже обязательно: перекрасив машину номером, но не сняв краску, мы
    // получили бы машину, которая не слушается палитры вовсе.
    if (appearance.customPrimary) {
        if (setCustomPrimary_ != nullptr) {
            invokeNative<void>(setCustomPrimary_, vehicle,
                               static_cast<int>(appearance.customPrimaryRed),
                               static_cast<int>(appearance.customPrimaryGreen),
                               static_cast<int>(appearance.customPrimaryBlue));
        }
    } else if (clearCustomPrimary_ != nullptr) {
        invokeNative<void>(clearCustomPrimary_, vehicle);
    }

    if (appearance.customSecondary) {
        if (setCustomSecondary_ != nullptr) {
            invokeNative<void>(setCustomSecondary_, vehicle,
                               static_cast<int>(appearance.customSecondaryRed),
                               static_cast<int>(appearance.customSecondaryGreen),
                               static_cast<int>(appearance.customSecondaryBlue));
        }
    } else if (clearCustomSecondary_ != nullptr) {
        invokeNative<void>(clearCustomSecondary_, vehicle);
    }

    if (setPlate_ != nullptr && !appearance.plate.empty()) {
        invokeNative<void>(setPlate_, vehicle, appearance.plate.c_str());
    }

    if (setPlateStyle_ != nullptr) {
        invokeNative<void>(setPlateStyle_, vehicle, static_cast<int>(appearance.plateStyle));
    }

    if (setLivery_ != nullptr && appearance.livery != shared::kStockMod) {
        invokeNative<void>(setLivery_, vehicle, static_cast<int>(appearance.livery));
    }

    if (setDirt_ != nullptr) {
        invokeNative<void>(setDirt_, vehicle, appearance.dirtLevel);
    }

    if (setWindowTint_ != nullptr && appearance.windowTint != shared::kStockMod) {
        invokeNative<void>(setWindowTint_, vehicle, static_cast<int>(appearance.windowTint));
    }


    // Неон ставится по сторонам, а цвет — один на все: так устроено и в игре.
    if (setNeonOn_ != nullptr) {
        for (std::size_t side = 0; side < kNeonSides.size(); ++side) {
            const bool on = (appearance.neonSides &
                             static_cast<std::uint8_t>(kNeonSides[side])) != 0;

            invokeNative<void>(setNeonOn_, vehicle, static_cast<int>(side), on);
        }
    }

    if (setNeonColour_ != nullptr) {
        invokeNative<void>(setNeonColour_, vehicle, static_cast<int>(appearance.neonRed),
                           static_cast<int>(appearance.neonGreen),
                           static_cast<int>(appearance.neonBlue));
    }

    if (setTyreSmoke_ != nullptr) {
        invokeNative<void>(setTyreSmoke_, vehicle, static_cast<int>(appearance.tyreSmokeRed),
                           static_cast<int>(appearance.tyreSmokeGreen),
                           static_cast<int>(appearance.tyreSmokeBlue));
    }

    if (setExtra_ != nullptr) {
        for (int extra = kFirstExtra; extra <= kLastExtra; ++extra) {
            const bool on =
                (appearance.extras & static_cast<std::uint16_t>(1U << (extra - kFirstExtra))) != 0;

            // Второй довод у игры перевёрнут: ноль означает «включить». Так у
            // неё и записано, и спорить с этим негде.
            invokeNative<void>(setExtra_, vehicle, extra, on ? 0 : 1);
        }
    }

    // Тюнинг ставится только после того, как машине выдан набор деталей: без
    // него игра примет вызов и не сделает ничего. Ноль — обычный набор, других
    // у машин, кроме особых, и не бывает.
    if (setModKit_ == nullptr) {
        return;
    }

    invokeNative<void>(setModKit_, vehicle, 0);

    if (setWheelType_ != nullptr && appearance.wheelType != shared::kStockMod) {
        // Тип дисков ставится до самих дисков: он решает, из какого списка
        // берётся номер, и поставленный после — сменит их на первые попавшиеся.
        invokeNative<void>(setWheelType_, vehicle, static_cast<int>(appearance.wheelType));
    }

    if (setMod_ != nullptr) {
        for (int slot = 0; slot < shared::kVehicleModSlots; ++slot) {
            const std::int8_t mod = appearance.mods[static_cast<std::size_t>(slot)];

            if (mod == shared::kStockMod) {
                continue;
            }

            // Последний довод — ставить ли заодно нестандартные диски. Нет: тип
            // дисков мы задали сами выше, и переспорить его здесь значило бы
            // потерять его.
            invokeNative<void>(setMod_, vehicle, slot, static_cast<int>(mod), false);
        }
    }

    if (toggleMod_ != nullptr) {
        for (const int slot : kToggleModSlots) {
            const bool on = (appearance.toggleMods & (1U << static_cast<std::uint32_t>(slot))) != 0;
            invokeNative<void>(toggleMod_, vehicle, slot, on);
        }
    }
}

} // namespace oxymp::client::game
