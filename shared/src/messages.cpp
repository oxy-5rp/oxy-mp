#include <oxymp/shared/protocol/messages.hpp>

#include <algorithm>
#include <cmath>

namespace oxymp::shared {
namespace {

/// Род сущности из байта. Незнакомый — None, то есть «ни к кому».
///
/// С проверкой, а не приведением напрямую, и это не перестраховка. Род решает,
/// в каком списке искать сущность; незнакомое число, приведённое молча, ушло бы
/// в этот выбор и вышло бы из него неизвестно чем. None же означает «отвязано»
/// — самое безобидное из всего, чем такое сообщение может оказаться.
[[nodiscard]] EntityKind readEntityKind(std::uint8_t value) noexcept {
    switch (static_cast<EntityKind>(value)) {
    case EntityKind::Player:
        return EntityKind::Player;
    case EntityKind::Vehicle:
        return EntityKind::Vehicle;
    case EntityKind::Object:
        return EntityKind::Object;
    case EntityKind::Ped:
        return EntityKind::Ped;
    case EntityKind::None:
        break;
    }

    return EntityKind::None;
}

} // namespace

void ClientHello::write(ByteWriter& writer) const {
    writer.writeU16(protocolVersion);
    writer.writeString(nickname);
    writer.writeString(password);
}

ClientHello ClientHello::read(ByteReader& reader) {
    ClientHello message;
    message.protocolVersion = reader.readU16();
    message.nickname = reader.readString();
    message.password = reader.readString();
    return message;
}

void ServerWelcome::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeVec3(spawnPosition);
    writer.writeU16(tickRate);
    writer.writeString(name);
}

ServerWelcome ServerWelcome::read(ByteReader& reader) {
    ServerWelcome message;
    message.playerId = reader.readU32();
    message.spawnPosition = reader.readVec3();
    message.tickRate = reader.readU16();
    message.name = reader.readString();
    return message;
}

void ServerReject::write(ByteWriter& writer) const {
    writer.writeU8(static_cast<std::uint8_t>(reason));
}

ServerReject ServerReject::read(ByteReader& reader) {
    ServerReject message;
    message.reason = static_cast<RejectReason>(reader.readU8());
    return message;
}

void Ping::write(ByteWriter& writer) const {
    writer.writeU64(timestampMs);
}

Ping Ping::read(ByteReader& reader) {
    Ping message;
    message.timestampMs = reader.readU64();
    return message;
}

void Pong::write(ByteWriter& writer) const {
    writer.writeU64(timestampMs);
}

Pong Pong::read(ByteReader& reader) {
    Pong message;
    message.timestampMs = reader.readU64();
    return message;
}

void PlayerAppearance::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeU32(model);

    // Длины наборов не пишутся: они заданы протоколом и одинаковы у обеих
    // сторон. Написанные в пакет, они стали бы полем, которому получатель обязан
    // верить, — а верить ему нельзя.
    for (const PedComponent& component : components) {
        writer.writeU8(component.drawable);
        writer.writeU8(component.texture);
        writer.writeU8(component.palette);
    }

    for (const PedProp& prop : props) {
        writer.writeU8(static_cast<std::uint8_t>(prop.drawable));
        writer.writeU8(static_cast<std::uint8_t>(prop.texture));
    }

    writer.writeU8(shapeFirst);
    writer.writeU8(shapeSecond);
    writer.writeU8(shapeThird);
    writer.writeU8(skinFirst);
    writer.writeU8(skinSecond);
    writer.writeU8(skinThird);
    writer.writeFloat(shapeMix);
    writer.writeFloat(skinMix);
    writer.writeFloat(thirdMix);

    for (const PedOverlay& overlay : overlays) {
        writer.writeU8(overlay.index);
        writer.writeU8(overlay.colourType);
        writer.writeU8(overlay.colour);
        writer.writeU8(overlay.secondColour);
        writer.writeFloat(overlay.opacity);
    }

    writer.writeU8(hairColour);
    writer.writeU8(hairHighlight);
    writer.writeU8(eyeColour);
}

PlayerAppearance PlayerAppearance::read(ByteReader& reader) {
    PlayerAppearance message;
    message.playerId = reader.readU32();
    message.model = reader.readU32();

    for (PedComponent& component : message.components) {
        component.drawable = reader.readU8();
        component.texture = reader.readU8();
        component.palette = reader.readU8();
    }

    for (PedProp& prop : message.props) {
        prop.drawable = static_cast<std::int8_t>(reader.readU8());
        prop.texture = static_cast<std::int8_t>(reader.readU8());
    }

    message.shapeFirst = reader.readU8();
    message.shapeSecond = reader.readU8();
    message.shapeThird = reader.readU8();
    message.skinFirst = reader.readU8();
    message.skinSecond = reader.readU8();
    message.skinThird = reader.readU8();
    message.shapeMix = reader.readFloat();
    message.skinMix = reader.readFloat();
    message.thirdMix = reader.readFloat();

    for (PedOverlay& overlay : message.overlays) {
        overlay.index = reader.readU8();
        overlay.colourType = reader.readU8();
        overlay.colour = reader.readU8();
        overlay.secondColour = reader.readU8();
        overlay.opacity = reader.readFloat();
    }

    message.hairColour = reader.readU8();
    message.hairHighlight = reader.readU8();
    message.eyeColour = reader.readU8();

    return message;
}

void PlayerJoined::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeString(nickname);
}

PlayerJoined PlayerJoined::read(ByteReader& reader) {
    PlayerJoined message;
    message.playerId = reader.readU32();
    message.nickname = reader.readString();
    return message;
}

void PlayerLeft::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
}

PlayerLeft PlayerLeft::read(ByteReader& reader) {
    PlayerLeft message;
    message.playerId = reader.readU32();
    return message;
}

void PlayerState::write(ByteWriter& writer) const {
    writer.writeU32(sentAt);
    writer.writeU32(playerId);
    writer.writeVec3(position);

    // Угол и скорость — квантованными: шесть тысячных градуса и полтора
    // сантиметра в секунду там, где раньше стояла полная точность плавающего
    // числа. Разглядеть разницу нельзя, а восемь байт со снимка снимается.
    writer.writeAngle(heading);
    writer.writeVelocity(velocity);

    // Здоровье и броня — по байту: в нумерации игры их двести и сто, и больше
    // не бывает.
    writer.writeU8(static_cast<std::uint8_t>(health > kMaxHealth ? kMaxHealth : health));
    writer.writeU8(static_cast<std::uint8_t>(armour > kMaxArmour ? kMaxArmour : armour));

    writer.writeU32(flags);

    // Дальше — то, чего у большинства нет. Идущий безоружный человек — самый
    // частый случай в сессии, и платить за оружие, точку прицеливания и машину
    // должен тот, у кого они есть.
    std::uint8_t present = 0;
    if (weapon != 0) {
        present |= kHasWeapon;
    }
    if (has(flags, PlayerFlag::Aiming) || has(flags, PlayerFlag::Shooting)) {
        present |= kHasAim;
    } else if (aimAt != Vec3{}) {
        // Не целится, но куда-то смотрит. Едет это не точкой, а двумя углами:
        // точка в мире — двенадцать байт, а углы — четыре, и разницу платит
        // самый частый в сессии человек, идущий безоружным.
        //
        // Собрать её обратно можно потому, что оба конца знают одно и то же:
        // взгляд пущен от головы и уходит ровно на kLookRange.
        present |= kHasLook;
    }
    if (vehicleId != kInvalidVehicleId) {
        present |= kHasVehicle;
    }

    writer.writeU8(present);

    if ((present & kHasWeapon) != 0) {
        writer.writeU32(weapon);
        writer.writeU16(ammo);
    }

    if ((present & kHasAim) != 0) {
        writer.writeVec3(aimAt);
    }

    if ((present & kHasLook) != 0) {
        const Vec3 head{position.x, position.y, position.z + kLookHeight};

        const float dx = aimAt.x - head.x;
        const float dy = aimAt.y - head.y;
        const float dz = aimAt.z - head.z;

        const float flat = std::sqrt((dx * dx) + (dy * dy));

        // Углы в тех же градусах и том же круге, что и всё прочее в снимке:
        // рыскание отсчитывается от севера против часовой, наклон — от
        // горизонта вверх.
        writer.writeAngle(std::atan2(-dx, dy) * kDegrees);
        writer.writeAngle(std::atan2(dz, flat) * kDegrees);
    }

    if ((present & kHasVehicle) != 0) {
        writer.writeU32(vehicleId);
        writer.writeU8(static_cast<std::uint8_t>(seat));
    }

    writer.writeU8(static_cast<std::uint8_t>(action));
    writer.writeU8(actionSequence);
}

PlayerState PlayerState::read(ByteReader& reader) {
    PlayerState message;
    message.sentAt = reader.readU32();
    message.playerId = reader.readU32();
    message.position = reader.readVec3();
    message.heading = reader.readAngle();
    message.velocity = reader.readVelocity();
    message.health = reader.readU8();
    message.armour = reader.readU8();
    message.flags = reader.readU32();

    const std::uint8_t present = reader.readU8();

    if ((present & kHasWeapon) != 0) {
        message.weapon = reader.readU32();
        message.ammo = reader.readU16();
    }

    if ((present & kHasAim) != 0) {
        message.aimAt = reader.readVec3();
    }

    if ((present & kHasLook) != 0) {
        const float yaw = reader.readAngle() * kRadians;
        const float pitch = reader.readAngle() * kRadians;

        const float flat = std::cos(pitch);

        message.aimAt = Vec3{
            message.position.x - (std::sin(yaw) * flat * kLookRange),
            message.position.y + (std::cos(yaw) * flat * kLookRange),
            message.position.z + kLookHeight + (std::sin(pitch) * kLookRange),
        };
    }

    if ((present & kHasVehicle) != 0) {
        message.vehicleId = reader.readU32();
        message.seat = static_cast<std::int8_t>(reader.readU8());
    }

    message.action = static_cast<PedAction>(reader.readU8());
    message.actionSequence = reader.readU8();
    return message;
}

namespace {

/// Пороги, за которыми снимок считается изменившимся.
///
/// Взяты по тому, что видно. Сантиметр — вдесятеро меньше того, что игра
/// показывает движением ног; десятая градуса — поворот, неразличимый и вблизи;
/// пять сантиметров у точки взгляда — она уходит на два десятка метров вперёд,
/// и там это доли градуса.
///
/// Точным сравнением обойтись нельзя: положение и скорость приходят из игры
/// числами с плавающей точкой и в последних разрядах дрожат даже у стоящего
/// намертво.
constexpr float kMovedEnough = 0.01F;
constexpr float kTurnedEnough = 0.1F;
constexpr float kAimedEnough = 0.05F;
constexpr float kSpedEnough = 0.05F;

/// Сдвинулось ли заметно.
[[nodiscard]] bool moved(const Vec3& from, const Vec3& to, float enough) noexcept {
    return distanceSquared(from, to) > enough * enough;
}

} // namespace

bool differs(const PlayerState& sent, const PlayerState& fresh) noexcept {
    if (sent.flags != fresh.flags || sent.weapon != fresh.weapon || sent.ammo != fresh.ammo ||
        sent.health != fresh.health || sent.armour != fresh.armour ||
        sent.vehicleId != fresh.vehicleId || sent.seat != fresh.seat ||
        sent.action != fresh.action || sent.actionSequence != fresh.actionSequence) {
        return true;
    }

    // Поворот сравнивается по кругу: с 359 градусов на 1 человек повернулся на
    // два, а не на триста пятьдесят восемь.
    const float apart = std::fmod(std::abs(sent.heading - fresh.heading), 360.0F);
    const float turned = std::min(apart, 360.0F - apart);

    return turned > kTurnedEnough || moved(sent.position, fresh.position, kMovedEnough) ||
           moved(sent.velocity, fresh.velocity, kSpedEnough) ||
           moved(sent.aimAt, fresh.aimAt, kAimedEnough);
}

void PlayerStates::write(ByteWriter& writer) const {
    // Длина — одним байтом, и она обязана быть написана: связка переменной
    // длины, и без числа получателю не отличить конца списка от продолжения.
    const auto count =
        static_cast<std::uint8_t>(std::min(players.size(), kMaxStatesInBundle));

    writer.writeU8(count);

    for (std::size_t i = 0; i < count; ++i) {
        players[i].write(writer);
    }
}

PlayerStates PlayerStates::read(ByteReader& reader) {
    PlayerStates message;

    const std::uint8_t count = reader.readU8();
    message.players.reserve(count);

    for (std::uint8_t i = 0; i < count; ++i) {
        // Чтение прекращается на первой же нехватке байт: испорченная связка не
        // должна превратиться в сотню снимков из мусора.
        if (!reader.ok()) {
            break;
        }

        message.players.push_back(PlayerState::read(reader));
    }

    return message;
}

void VehicleState::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU32(sentAt);
    writer.writeU32(model);
    writer.writeVec3(position);

    // Всё, кроме положения, — квантованным. Снимок машины вдвое тяжелее снимка
    // игрока, а ходит с той же частотой; двадцать семь байт с него снимается
    // без единой различимой глазом потери.
    //
    // Положение оставлено полным нарочно: мир GTA — шестнадцать километров в
    // поперечнике, и деление его на шестьдесят пять тысяч частей дало бы шаг в
    // четверть метра. Машина дрожала бы на месте.
    writer.writeAngle(rotation.x);
    writer.writeAngle(rotation.y);
    writer.writeAngle(rotation.z);
    writer.writeVelocity(velocity);
    writer.writeAngularVelocity(angularVelocity);
    writer.writeControl(steer);
    writer.writeControl(throttle);
    writer.writeControl(brake);
    writer.writeU16(bodyHealth);
    writer.writeU16(engineHealth);
    writer.writeU16(tankHealth);
    writer.writeU16(flags);
    writer.writeU8(doorsOpen);
    writer.writeU8(doorsBroken);
    writer.writeU8(windowsBroken);
    writer.writeU8(tyresBurst);
    writer.writeU32(trailer);
}

VehicleState VehicleState::read(ByteReader& reader) {
    VehicleState message;
    message.id = reader.readU32();
    message.sentAt = reader.readU32();
    message.model = reader.readU32();
    message.position = reader.readVec3();

    // Углы поворота читаются по одному: порядок вычисления доводов не задан, и
    // собранные в одном выражении оси разъехались бы местами на другом
    // компиляторе.
    const float pitch = reader.readAngle();
    const float roll = reader.readAngle();
    const float yaw = reader.readAngle();
    message.rotation = Vec3{pitch, roll, yaw};

    message.velocity = reader.readVelocity();
    message.angularVelocity = reader.readAngularVelocity();
    message.steer = reader.readControl();
    message.throttle = reader.readControl();
    message.brake = reader.readControl();
    message.bodyHealth = reader.readU16();
    message.engineHealth = reader.readU16();
    message.tankHealth = reader.readU16();
    message.flags = reader.readU16();
    message.doorsOpen = reader.readU8();
    message.doorsBroken = reader.readU8();
    message.windowsBroken = reader.readU8();
    message.tyresBurst = reader.readU8();
    message.trailer = reader.readU32();
    return message;
}

namespace {

/// То же для машины: угловая скорость и ход педалей.
///
/// Сотая радиана в секунду — это оборот за десять минут; сотая доля хода педали
/// не видна ни в стоп-сигналах, ни в угле колёс.
constexpr float kSpunEnough = 0.01F;
constexpr float kNudgedEnough = 0.01F;

/// Разошлись ли два угла заметно, с приведением к кругу.
///
/// По кругу, а не как числа: с 359 градусов на 1 машина повернулась на два, а
/// не на триста пятьдесят восемь.
[[nodiscard]] bool turned(float from, float to) noexcept {
    const float apart = std::fmod(std::abs(from - to), 360.0F);

    return std::min(apart, 360.0F - apart) > kTurnedEnough;
}

} // namespace

bool differs(const VehicleState& sent, const VehicleState& fresh) noexcept {
    if (sent.id != fresh.id || sent.model != fresh.model || sent.flags != fresh.flags ||
        sent.doorsOpen != fresh.doorsOpen || sent.doorsBroken != fresh.doorsBroken ||
        sent.windowsBroken != fresh.windowsBroken || sent.tyresBurst != fresh.tyresBurst ||
        sent.bodyHealth != fresh.bodyHealth || sent.engineHealth != fresh.engineHealth ||
        sent.tankHealth != fresh.tankHealth || sent.trailer != fresh.trailer) {
        return true;
    }

    if (std::abs(sent.steer - fresh.steer) > kNudgedEnough ||
        std::abs(sent.throttle - fresh.throttle) > kNudgedEnough ||
        std::abs(sent.brake - fresh.brake) > kNudgedEnough) {
        return true;
    }

    if (turned(sent.rotation.x, fresh.rotation.x) || turned(sent.rotation.y, fresh.rotation.y) ||
        turned(sent.rotation.z, fresh.rotation.z)) {
        return true;
    }

    return moved(sent.position, fresh.position, kMovedEnough) ||
           moved(sent.velocity, fresh.velocity, kSpedEnough) ||
           moved(sent.angularVelocity, fresh.angularVelocity, kSpunEnough);
}

void VehicleStates::write(ByteWriter& writer) const {
    // Слово в слово как у PlayerStates, и намеренно: правило у обеих связок
    // одно, и разойтись им нельзя.
    const auto count =
        static_cast<std::uint8_t>(std::min(vehicles.size(), kMaxStatesInBundle));

    writer.writeU8(count);

    for (std::size_t i = 0; i < count; ++i) {
        vehicles[i].write(writer);
    }
}

VehicleStates VehicleStates::read(ByteReader& reader) {
    VehicleStates message;

    const std::uint8_t count = reader.readU8();
    message.vehicles.reserve(count);

    for (std::uint8_t i = 0; i < count; ++i) {
        // Чтение прекращается на первой же нехватке байт: испорченная связка не
        // должна превратиться в сотню снимков из мусора.
        if (!reader.ok()) {
            break;
        }

        message.vehicles.push_back(VehicleState::read(reader));
    }

    return message;
}

void VehicleAppearance::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU8(primaryColour);
    writer.writeU8(secondaryColour);
    writer.writeU8(pearlescentColour);
    writer.writeU8(wheelColour);
    writer.writeString(plate);
    writer.writeU8(plateStyle);
    writer.writeU8(static_cast<std::uint8_t>(livery));
    writer.writeU8(static_cast<std::uint8_t>(wheelType));
    writer.writeU8(static_cast<std::uint8_t>(windowTint));
    writer.writeFloat(dirtLevel);

    // Длина набора не пишется: она задана протоколом и одинакова у обеих сторон.
    // Написанная в пакет, она стала бы полем, которому получатель обязан верить,
    // — а верить ему нельзя.
    for (const std::int8_t mod : mods) {
        writer.writeU8(static_cast<std::uint8_t>(mod));
    }

    writer.writeU32(toggleMods);

    writer.writeU8(customTyres ? 1 : 0);

    writer.writeU8(tyreSmokeRed);
    writer.writeU8(tyreSmokeGreen);
    writer.writeU8(tyreSmokeBlue);

    writer.writeU8(neonSides);
    writer.writeU8(neonRed);
    writer.writeU8(neonGreen);
    writer.writeU8(neonBlue);

    writer.writeU16(extras);

    writer.writeU8(customPrimary ? 1 : 0);
    writer.writeU8(customPrimaryRed);
    writer.writeU8(customPrimaryGreen);
    writer.writeU8(customPrimaryBlue);

    writer.writeU8(customSecondary ? 1 : 0);
    writer.writeU8(customSecondaryRed);
    writer.writeU8(customSecondaryGreen);
    writer.writeU8(customSecondaryBlue);
}

VehicleAppearance VehicleAppearance::read(ByteReader& reader) {
    VehicleAppearance message;
    message.id = reader.readU32();
    message.primaryColour = reader.readU8();
    message.secondaryColour = reader.readU8();
    message.pearlescentColour = reader.readU8();
    message.wheelColour = reader.readU8();
    message.plate = reader.readString();
    message.plateStyle = reader.readU8();
    message.livery = static_cast<std::int8_t>(reader.readU8());
    message.wheelType = static_cast<std::int8_t>(reader.readU8());
    message.windowTint = static_cast<std::int8_t>(reader.readU8());
    message.dirtLevel = reader.readFloat();

    for (std::int8_t& mod : message.mods) {
        mod = static_cast<std::int8_t>(reader.readU8());
    }

    message.toggleMods = reader.readU32();

    message.customTyres = reader.readU8() != 0;

    message.tyreSmokeRed = reader.readU8();
    message.tyreSmokeGreen = reader.readU8();
    message.tyreSmokeBlue = reader.readU8();

    message.neonSides = reader.readU8();
    message.neonRed = reader.readU8();
    message.neonGreen = reader.readU8();
    message.neonBlue = reader.readU8();

    message.extras = reader.readU16();

    message.customPrimary = reader.readU8() != 0;
    message.customPrimaryRed = reader.readU8();
    message.customPrimaryGreen = reader.readU8();
    message.customPrimaryBlue = reader.readU8();

    message.customSecondary = reader.readU8() != 0;
    message.customSecondaryRed = reader.readU8();
    message.customSecondaryGreen = reader.readU8();
    message.customSecondaryBlue = reader.readU8();

    if (message.plate.size() > kMaxPlateLength) {
        message.plate.resize(kMaxPlateLength);
    }

    return message;
}

void VehicleAdded::write(ByteWriter& writer) const {
    // Снимок вкладывается целиком и своим же порядком полей. Переписывать его
    // здесь заново значило бы завести второе описание одних и тех же данных,
    // которое рано или поздно разойдётся с первым.
    state.write(writer);
    writer.writeU32(owner);
}

VehicleAdded VehicleAdded::read(ByteReader& reader) {
    VehicleAdded message;
    message.state = VehicleState::read(reader);
    message.owner = reader.readU32();
    return message;
}

void VehicleRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

VehicleRemoved VehicleRemoved::read(ByteReader& reader) {
    VehicleRemoved message;
    message.id = reader.readU32();
    return message;
}

void VehicleAuthority::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU32(owner);
}

VehicleAuthority VehicleAuthority::read(ByteReader& reader) {
    VehicleAuthority message;
    message.id = reader.readU32();
    message.owner = reader.readU32();
    return message;
}

void WorldState::write(ByteWriter& writer) const {
    writer.writeString(weather);
    writer.writeU8(hour);
    writer.writeU8(minute);
    writer.writeU8(second);
}

WorldState WorldState::read(ByteReader& reader) {
    WorldState message;
    message.weather = reader.readString();
    message.hour = reader.readU8();
    message.minute = reader.readU8();
    message.second = reader.readU8();

    if (message.weather.size() > kMaxWeatherLength) {
        message.weather.resize(kMaxWeatherLength);
    }

    return message;
}

void PlayerLoadout::write(ByteWriter& writer) const {
    const auto count = static_cast<std::uint16_t>(
        std::min<std::size_t>(weapons.size(), kMaxWeaponSlots));

    writer.writeU8(static_cast<std::uint8_t>(replace));
    writer.writeU16(count);

    for (std::uint16_t index = 0; index < count; ++index) {
        const WeaponSlot& slot = weapons[index];

        writer.writeU32(slot.weapon);
        writer.writeU16(slot.ammo);
        writer.writeU8(slot.tint);

        const auto parts = static_cast<std::uint8_t>(
            std::min<std::size_t>(slot.components.size(), kMaxWeaponComponents));

        writer.writeU8(parts);

        for (std::uint8_t part = 0; part < parts; ++part) {
            writer.writeU32(slot.components[part]);
        }
    }
}

PlayerLoadout PlayerLoadout::read(ByteReader& reader) {
    PlayerLoadout message;
    message.replace = reader.readU8() != 0;

    const std::uint16_t count = reader.readU16();

    // Длине из пакета верить нельзя, но и отбрасывать её молча — тоже. Память
    // резервируется по пределу, а не по названному числу: испорченное или
    // враждебное число иначе заставило бы выделить её под список, которого нет.
    //
    // Читается при этом ровно столько, сколько названо. Пакет, назвавший больше,
    // чем принёс, оборвётся на нехватке данных, и разбор откажет сам — а
    // молчаливый возврат пустого списка выглядел бы как «сервер разоружил
    // игрока», то есть был бы ложью, а не отказом.
    message.weapons.reserve(std::min<std::size_t>(count, kMaxWeaponSlots));

    for (std::uint16_t index = 0; index < count; ++index) {
        WeaponSlot slot;
        slot.weapon = reader.readU32();
        slot.ammo = reader.readU16();
        slot.tint = reader.readU8();

        const std::uint8_t parts = reader.readU8();
        slot.components.reserve(std::min<std::size_t>(parts, kMaxWeaponComponents));

        // Читается ровно столько, сколько названо, а кладётся не больше предела:
        // то же правило, что и у списка оружия выше, и по той же причине.
        for (std::uint8_t part = 0; part < parts; ++part) {
            const std::uint32_t component = reader.readU32();

            if (slot.components.size() < kMaxWeaponComponents) {
                slot.components.push_back(component);
            }
        }

        if (message.weapons.size() < kMaxWeaponSlots) {
            message.weapons.push_back(std::move(slot));
        }
    }

    return message;
}

void HealthChanged::write(ByteWriter& writer) const {
    writer.writeU16(health);
    writer.writeU16(armour);
    writer.writeU32(attacker);
}

HealthChanged HealthChanged::read(ByteReader& reader) {
    HealthChanged message;
    message.health = reader.readU16();
    message.armour = reader.readU16();
    message.attacker = reader.readU32();
    return message;
}

void ObjectAdded::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU32(model);
    writer.writeVec3(position);
    writer.writeVec3(rotation);
}

ObjectAdded ObjectAdded::read(ByteReader& reader) {
    ObjectAdded message;
    message.id = reader.readU32();
    message.model = reader.readU32();
    message.position = reader.readVec3();
    message.rotation = reader.readVec3();
    return message;
}

void ObjectRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

ObjectRemoved ObjectRemoved::read(ByteReader& reader) {
    ObjectRemoved message;
    message.id = reader.readU32();
    return message;
}

void ChatSay::write(ByteWriter& writer) const {
    writer.writeString(text);
}

ChatSay ChatSay::read(ByteReader& reader) {
    ChatSay message;
    message.text = reader.readString();
    return message;
}

void ChatLine::write(ByteWriter& writer) const {
    writer.writeU8(static_cast<std::uint8_t>(kind));
    writer.writeU32(playerId);
    writer.writeString(nickname);
    writer.writeString(text);
}

ChatLine ChatLine::read(ByteReader& reader) {
    ChatLine message;
    message.kind = static_cast<ChatKind>(reader.readU8());
    message.playerId = reader.readU32();
    message.nickname = reader.readString();
    message.text = reader.readString();
    return message;
}

void DamageReport::write(ByteWriter& writer) const {
    writer.writeU32(victim);
    writer.writeU16(amount);
    writer.writeU32(weapon);
}

DamageReport DamageReport::read(ByteReader& reader) {
    DamageReport message;
    message.victim = reader.readU32();
    message.amount = reader.readU16();
    message.weapon = reader.readU32();
    return message;
}

void DamageTaken::write(ByteWriter& writer) const {
    writer.writeU32(attacker);
    writer.writeU16(amount);
    writer.writeU32(weapon);
}

DamageTaken DamageTaken::read(ByteReader& reader) {
    DamageTaken message;
    message.attacker = reader.readU32();
    message.amount = reader.readU16();
    message.weapon = reader.readU32();
    return message;
}

void PlayerTeleport::write(ByteWriter& writer) const {
    writer.writeVec3(position);
}

PlayerTeleport PlayerTeleport::read(ByteReader& reader) {
    PlayerTeleport message;
    message.position = reader.readVec3();
    return message;
}

namespace {

/// Пишет именованное событие.
///
/// Обрезка при записи, а не только при чтении: длинное имя или нагрузку следует
/// оборвать у того, кто их сочинил, — иначе получатель отвергнет пакет целиком,
/// и отправитель об этом не узнает.
void writeEvent(ByteWriter& writer, const std::string& name, const std::string& payload) {
    writer.writeString(name.substr(0, std::min(name.size(), kMaxEventNameLength)));
    writer.writeText(payload);
}

/// Читает его обратно.
void readEvent(ByteReader& reader, std::string& name, std::string& payload) {
    name = reader.readString();
    payload = reader.readText();

    // Предел применяется и при чтении: пакет мог прийти откуда угодно, и верить
    // его полям нельзя.
    if (name.size() > kMaxEventNameLength) {
        name.resize(kMaxEventNameLength);
    }
    if (payload.size() > kMaxEventPayloadLength) {
        payload.resize(kMaxEventPayloadLength);
    }
}

} // namespace

void ClientEvent::write(ByteWriter& writer) const {
    writeEvent(writer, name, payload);
}

ClientEvent ClientEvent::read(ByteReader& reader) {
    ClientEvent message;
    readEvent(reader, message.name, message.payload);
    return message;
}

void ServerEvent::write(ByteWriter& writer) const {
    writeEvent(writer, name, payload);
}

ServerEvent ServerEvent::read(ByteReader& reader) {
    ServerEvent message;
    readEvent(reader, message.name, message.payload);
    return message;
}

void MoneyChanged::write(ByteWriter& writer) const {
    // Знаковое пишется беззнаковым, и обратное превращение возвращает то же
    // число: дополнительный код и его обход по кругу определены языком, а не
    // оставлены на усмотрение сборки.
    writer.writeU64(static_cast<std::uint64_t>(amount));
}

MoneyChanged MoneyChanged::read(ByteReader& reader) {
    MoneyChanged message;
    message.amount = static_cast<std::int64_t>(reader.readU64());
    return message;
}

void ResourceList::write(ByteWriter& writer) const {
    const auto count =
        static_cast<std::uint16_t>(std::min<std::size_t>(entries.size(), kMaxResources));

    writer.writeU16(count);

    for (std::uint16_t i = 0; i < count; ++i) {
        writer.writeString(entries[i].name);
        writer.writeString(entries[i].hash);
        writer.writeU64(entries[i].size);
        writer.writeU8(entries[i].page ? 1 : 0);
        writer.writeString(entries[i].dataFile);
        writer.writeString(entries[i].bundle);
    }
}

ResourceList ResourceList::read(ByteReader& reader) {
    ResourceList message;

    const std::uint16_t count = reader.readU16();

    // Предел применяется при чтении, а не только при записи: пакет мог прийти
    // откуда угодно, и верить его полю длины нельзя.
    const std::uint16_t safe = std::min<std::uint16_t>(count, kMaxResources);

    message.entries.reserve(safe);

    for (std::uint16_t i = 0; i < safe; ++i) {
        ResourceEntry entry;
        entry.name = reader.readString();
        entry.hash = reader.readString();
        entry.size = reader.readU64();
        entry.page = reader.readU8() != 0;
        entry.dataFile = reader.readString();
        entry.bundle = reader.readString();

        message.entries.push_back(std::move(entry));
    }

    return message;
}

void VehicleTeleport::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeVec3(position);
    writer.writeAngle(heading);
}

VehicleTeleport VehicleTeleport::read(ByteReader& reader) {
    VehicleTeleport message;
    message.id = reader.readU32();
    message.position = reader.readVec3();
    message.heading = reader.readAngle();
    return message;
}

void VehicleRepair::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

VehicleRepair VehicleRepair::read(ByteReader& reader) {
    VehicleRepair message;
    message.id = reader.readU32();
    return message;
}

void BlipState::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeVec3(position);
    writer.writeU16(sprite);
    writer.writeU8(colour);
    writer.writeU8(alpha);
    writer.writeU8(display);
    writer.writeU8(shortRange ? 1 : 0);
    writer.writeFloat(scale);
    writer.writeString(name);
}

BlipState BlipState::read(ByteReader& reader) {
    BlipState message;
    message.id = reader.readU32();
    message.position = reader.readVec3();
    message.sprite = reader.readU16();
    message.colour = reader.readU8();
    message.alpha = reader.readU8();
    message.display = reader.readU8();
    message.shortRange = reader.readU8() != 0;
    message.scale = reader.readFloat();
    message.name = reader.readString();
    return message;
}

void BlipRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

BlipRemoved BlipRemoved::read(ByteReader& reader) {
    BlipRemoved message;
    message.id = reader.readU32();
    return message;
}

void MarkerState::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU8(type);
    writer.writeVec3(position);
    writer.writeVec3(rotation);
    writer.writeVec3(direction);
    writer.writeVec3(scale);
    writer.writeU8(red);
    writer.writeU8(green);
    writer.writeU8(blue);
    writer.writeU8(alpha);

    // Признаки — одним байтом, а не четырьмя: их четыре, и каждый занял бы
    // целый байт ради одного бита.
    std::uint8_t flags = 0;
    flags |= visible ? static_cast<std::uint8_t>(MarkerFlag::Visible) : 0;
    flags |= bobUpAndDown ? static_cast<std::uint8_t>(MarkerFlag::BobUpAndDown) : 0;
    flags |= faceCamera ? static_cast<std::uint8_t>(MarkerFlag::FaceCamera) : 0;
    flags |= rotate ? static_cast<std::uint8_t>(MarkerFlag::Rotate) : 0;

    writer.writeU8(flags);
    writer.writeFloat(streamingDistance);
}

MarkerState MarkerState::read(ByteReader& reader) {
    MarkerState message;
    message.id = reader.readU32();
    message.type = reader.readU8();
    message.position = reader.readVec3();
    message.rotation = reader.readVec3();
    message.direction = reader.readVec3();
    message.scale = reader.readVec3();
    message.red = reader.readU8();
    message.green = reader.readU8();
    message.blue = reader.readU8();
    message.alpha = reader.readU8();
    const std::uint8_t flags = reader.readU8();
    message.visible = has(flags, MarkerFlag::Visible);
    message.bobUpAndDown = has(flags, MarkerFlag::BobUpAndDown);
    message.faceCamera = has(flags, MarkerFlag::FaceCamera);
    message.rotate = has(flags, MarkerFlag::Rotate);

    message.streamingDistance = reader.readFloat();
    return message;
}

void MarkerRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

MarkerRemoved MarkerRemoved::read(ByteReader& reader) {
    MarkerRemoved message;
    message.id = reader.readU32();
    return message;
}

void CheckpointState::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU8(type);
    writer.writeVec3(position);
    writer.writeVec3(nextPosition);
    writer.writeFloat(radius);
    writer.writeFloat(height);
    writer.writeU8(red);
    writer.writeU8(green);
    writer.writeU8(blue);
    writer.writeU8(alpha);
    writer.writeU8(iconRed);
    writer.writeU8(iconGreen);
    writer.writeU8(iconBlue);
    writer.writeU8(iconAlpha);
    writer.writeU8(visible ? 1 : 0);
    writer.writeFloat(streamingDistance);
}

CheckpointState CheckpointState::read(ByteReader& reader) {
    CheckpointState message;
    message.id = reader.readU32();
    message.type = reader.readU8();
    message.position = reader.readVec3();
    message.nextPosition = reader.readVec3();
    message.radius = reader.readFloat();
    message.height = reader.readFloat();
    message.red = reader.readU8();
    message.green = reader.readU8();
    message.blue = reader.readU8();
    message.alpha = reader.readU8();
    message.iconRed = reader.readU8();
    message.iconGreen = reader.readU8();
    message.iconBlue = reader.readU8();
    message.iconAlpha = reader.readU8();
    message.visible = reader.readU8() != 0;
    message.streamingDistance = reader.readFloat();
    return message;
}

void CheckpointRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

CheckpointRemoved CheckpointRemoved::read(ByteReader& reader) {
    CheckpointRemoved message;
    message.id = reader.readU32();
    return message;
}

void PlayerAnimation::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeString(dictionary);
    writer.writeString(name);
    writer.writeFloat(blendIn);
    writer.writeFloat(blendOut);
    writer.writeU32(static_cast<std::uint32_t>(duration));
    writer.writeU32(static_cast<std::uint32_t>(flags));
    writer.writeFloat(playbackRate);
    writer.writeU8(locks);
}

PlayerAnimation PlayerAnimation::read(ByteReader& reader) {
    PlayerAnimation message;
    message.playerId = reader.readU32();
    message.dictionary = reader.readString();
    message.name = reader.readString();
    message.blendIn = reader.readFloat();
    message.blendOut = reader.readFloat();
    message.duration = static_cast<std::int32_t>(reader.readU32());
    message.flags = static_cast<std::int32_t>(reader.readU32());
    message.playbackRate = reader.readFloat();
    message.locks = reader.readU8();
    return message;
}

void PedState::write(ByteWriter& writer) const {
    writer.writeU32(id);
    writer.writeU32(model);
    writer.writeVec3(position);
    writer.writeVec3(rotation);
    writer.writeU16(health);
    writer.writeU16(maxHealth);
    writer.writeU16(armour);
    writer.writeU32(weapon);
}

PedState PedState::read(ByteReader& reader) {
    PedState message;
    message.id = reader.readU32();
    message.model = reader.readU32();
    message.position = reader.readVec3();
    message.rotation = reader.readVec3();
    message.health = reader.readU16();
    message.maxHealth = reader.readU16();
    message.armour = reader.readU16();
    message.weapon = reader.readU32();
    return message;
}

void PedRemoved::write(ByteWriter& writer) const {
    writer.writeU32(id);
}

PedRemoved PedRemoved::read(ByteReader& reader) {
    PedRemoved message;
    message.id = reader.readU32();
    return message;
}

void EntityAttachment::write(ByteWriter& writer) const {
    writer.writeU8(static_cast<std::uint8_t>(kind));
    writer.writeU32(id);
    writer.writeU8(static_cast<std::uint8_t>(targetKind));
    writer.writeU32(target);
    writer.writeU32(static_cast<std::uint32_t>(bone));
    writer.writeString(boneName);
    writer.writeVec3(position);
    writer.writeVec3(rotation);

    // Два признака одним байтом: четыре байта ради двух бит — расточительство,
    // и так же уложены признаки маркера и движения.
    writer.writeU8(static_cast<std::uint8_t>((collision ? 1U : 0U) | (fixedRotation ? 2U : 0U)));
}

EntityAttachment EntityAttachment::read(ByteReader& reader) {
    EntityAttachment message;
    message.kind = readEntityKind(reader.readU8());
    message.id = reader.readU32();
    message.targetKind = readEntityKind(reader.readU8());
    message.target = reader.readU32();
    message.bone = static_cast<std::int32_t>(reader.readU32());
    message.boneName = reader.readString();
    message.position = reader.readVec3();
    message.rotation = reader.readVec3();

    const std::uint8_t flags = reader.readU8();
    message.collision = (flags & 1U) != 0;
    message.fixedRotation = (flags & 2U) != 0;

    return message;
}

void PlayerIntoVehicle::write(ByteWriter& writer) const {
    writer.writeU32(vehicle);
    writer.writeU8(static_cast<std::uint8_t>(seat));
}

PlayerIntoVehicle PlayerIntoVehicle::read(ByteReader& reader) {
    PlayerIntoVehicle message;
    message.vehicle = reader.readU32();
    message.seat = static_cast<std::int8_t>(reader.readU8());
    return message;
}

void Explosion::write(ByteWriter& writer) const {
    writer.writeVec3(position);
    writer.writeU32(static_cast<std::uint32_t>(kind));
    writer.writeFloat(scale);
    writer.writeFloat(shake);

    // Оба признака — одним байтом: каждый по своему байту стоил бы вдвое
    // дороже, а взрыв идёт по надёжному каналу, где байты не бесплатны.
    std::uint8_t traits = 0;
    traits |= audible ? 1U << 0U : 0U;
    traits |= invisible ? 1U << 1U : 0U;

    writer.writeU8(traits);
}

Explosion Explosion::read(ByteReader& reader) {
    Explosion message;
    message.position = reader.readVec3();
    message.kind = static_cast<std::int32_t>(reader.readU32());
    message.scale = reader.readFloat();
    message.shake = reader.readFloat();

    const std::uint8_t traits = reader.readU8();
    message.audible = (traits & (1U << 0U)) != 0;
    message.invisible = (traits & (1U << 1U)) != 0;

    return message;
}

void WeaponFired::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeU32(weapon);
    writer.writeVec3(target);
}

WeaponFired WeaponFired::read(ByteReader& reader) {
    WeaponFired message;
    message.playerId = reader.readU32();
    message.weapon = reader.readU32();
    message.target = reader.readVec3();
    return message;
}

void PlayerWeapon::write(ByteWriter& writer) const {
    writer.writeU32(playerId);
    writer.writeU32(weapon);
    writer.writeU8(tint);

    // Предел тот же, что и у снаряжения: список насадок здесь описывает то же
    // самое оружие, только увиденное со стороны.
    const auto parts = static_cast<std::uint8_t>(
        std::min<std::size_t>(components.size(), kMaxWeaponComponents));

    writer.writeU8(parts);

    for (std::uint8_t part = 0; part < parts; ++part) {
        writer.writeU32(components[part]);
    }
}

PlayerWeapon PlayerWeapon::read(ByteReader& reader) {
    PlayerWeapon message;
    message.playerId = reader.readU32();
    message.weapon = reader.readU32();
    message.tint = reader.readU8();

    const std::uint8_t parts = reader.readU8();

    if (parts > kMaxWeaponComponents) {
        // Насадок больше, чем их бывает у оружия: пакет испорчен либо собран не
        // нами. Разбор прекращается — читать по нему список значило бы верить
        // числу, которое уже назвалось неверным.
        reader.fail();
        return message;
    }

    message.components.reserve(parts);

    for (std::uint8_t part = 0; part < parts && reader.ok(); ++part) {
        message.components.push_back(reader.readU32());
    }

    return message;
}

std::optional<MessageId> peekMessageId(ByteView packet) noexcept {
    if (packet.empty()) {
        return std::nullopt;
    }

    switch (static_cast<MessageId>(packet.front())) {
    case MessageId::ClientHello:
    case MessageId::ServerWelcome:
    case MessageId::ServerReject:
    case MessageId::Ping:
    case MessageId::Pong:
    case MessageId::PlayerJoined:
    case MessageId::PlayerLeft:
    case MessageId::PlayerState:
    case MessageId::VehicleState:
    case MessageId::VehicleAppearance:
    case MessageId::ChatSay:
    case MessageId::ChatLine:
    case MessageId::DamageReport:
    case MessageId::DamageTaken:
    case MessageId::PlayerTeleport:
    case MessageId::MoneyChanged:
    case MessageId::ResourceList:
    case MessageId::VehicleAdded:
    case MessageId::VehicleRemoved:
    case MessageId::VehicleAuthority:
    case MessageId::WorldState:
    case MessageId::PlayerLoadout:
    case MessageId::HealthChanged:
    case MessageId::ObjectAdded:
    case MessageId::ObjectRemoved:
    case MessageId::ClientEvent:
    case MessageId::ServerEvent:
    case MessageId::PlayerAppearance:
    case MessageId::PlayerStates:
    case MessageId::VehicleTeleport:
    case MessageId::VehicleRepair:
    case MessageId::BlipState:
    case MessageId::BlipRemoved:
    case MessageId::PlayerIntoVehicle:
    case MessageId::MarkerState:
    case MessageId::MarkerRemoved:
    case MessageId::CheckpointState:
    case MessageId::CheckpointRemoved:
    case MessageId::PlayerAnimation:
    case MessageId::EntityAttachment:
    case MessageId::PedState:
    case MessageId::PedRemoved:
    case MessageId::VehicleStates:
    case MessageId::Explosion:
    case MessageId::WeaponFired:
    case MessageId::PlayerWeapon:
        return static_cast<MessageId>(packet.front());
    }

    return std::nullopt;
}

} // namespace oxymp::shared
