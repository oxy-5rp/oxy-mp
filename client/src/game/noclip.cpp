#include "noclip.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Скорость обычного и ускоренного полёта, метров в секунду.
///
/// Отмеряется во времени, а не в кадрах, и это исправление: с шагом на кадр
/// полёт шёл тем быстрее, чем выше частота, и рвался на её просадках — а
/// проседает она в игре постоянно, стоит появиться городу вокруг. Значения
/// подобраны так, чтобы при шестидесяти кадрах скорость осталась прежней.
constexpr float kSpeed = 21.0F;
constexpr float kFastSpeed = 120.0F;

/// Предел одного шага по времени.
///
/// Кадр может задержаться надолго — на подгрузке мира или на переключении окна,
/// — и без предела накопившееся время швырнуло бы персонажа на километры.
constexpr float kMaxStepSeconds = 0.1F;

/// Ближе этого камера считается стоящей в самом персонаже, в метрах.
///
/// Так бывает от первого лица: направление из отрезка между ними уже не собрать,
/// потому что отрезка нет.
constexpr float kDegenerateCameraDistance = 0.5F;

/// На какой высоте от ступней камера держит персонажа, в метрах.
///
/// Это и была причина того, что полёт «немного получше, но всё ещё кривой».
/// Координаты персонажа игра отдаёт от ступней, а камера висит выше и целится
/// ему в грудь. Отрезок «камера — ступни» из-за этого смотрит вниз на добрых
/// двадцать градусов, и полёт вперёд уходил в землю.
constexpr float kChestHeight = 0.95F;

/// Насколько двум источникам направления позволено разойтись.
///
/// Косинус угла: 0.5 — это шестьдесят градусов. Точность отрезка «камера —
/// грудь» хуже этого не бывает, а разбор углов, если он неверен, ошибается
/// куда сильнее — обычно на пол-оборота или на весь наклон.
constexpr float kAgreement = 0.5F;

/// Ниже этого длина вектора считается нулевой.
constexpr float kTinyLength = 0.0001F;

/// Порядок углов для GET_GAMEPLAY_CAM_ROT.
///
/// Двойка: при ней наклон лежит в x, а поворот вокруг вертикали — в z. Так
/// принято во всей открытой базе нативов, и проверяется это здесь же — сверкой
/// с отрезком «камера — грудь».
constexpr int kRotationOrder = 2;

constexpr float kDegreesToRadians = std::numbers::pi_v<float> / 180.0F;
constexpr float kRadiansToDegrees = 180.0F / std::numbers::pi_v<float>;

bool keyDown(int key) noexcept {
    return (::GetAsyncKeyState(key) & 0x8000) != 0;
}

float length(const shared::Vec3& value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

shared::Vec3 normalised(const shared::Vec3& value) {
    const float size = length(value);
    if (size < kTinyLength) {
        return {};
    }

    return shared::Vec3{value.x / size, value.y / size, value.z / size};
}

/// Направление «вправо» при взгляде вдоль forward.
///
/// Векторное произведение направления взгляда на вертикаль. Раскрыто вручную,
/// потому что вертикаль здесь постоянная: лететь боком с креном мы не собираемся.
shared::Vec3 rightOf(const shared::Vec3& forward) {
    return normalised(shared::Vec3{forward.y, -forward.x, 0.0F});
}

/// Направление взгляда, переведённое в поворот персонажа.
///
/// Поворот в игре отсчитывается от севера против часовой стрелки, поэтому
/// вычисляется он из тех же двух чисел, что и направление, — но обратным ходом.
float headingOf(const shared::Vec3& forward) {
    return std::atan2(-forward.x, forward.y) * kRadiansToDegrees;
}

} // namespace

Noclip::Noclip(const NativeTable& table) noexcept
    : getCoords_(table.handlerFor(natives::kGetEntityCoords)),
      setCoords_(table.handlerFor(natives::kSetEntityCoords)),
      setHeading_(table.handlerFor(natives::kSetEntityHeading)),
      freeze_(table.handlerFor(natives::kFreezeEntityPosition)),
      setCollision_(table.handlerFor(natives::kSetEntityCollision)),
      cameraCoords_(table.handlerFor(natives::kGetGameplayCamCoord)),
      cameraRotation_(table.handlerFor(natives::kGetGameplayCamRot)) {}

bool Noclip::ready() const noexcept {
    return getCoords_ != nullptr && setCoords_ != nullptr && setHeading_ != nullptr &&
           freeze_ != nullptr && setCollision_ != nullptr && cameraCoords_ != nullptr;
}

void Noclip::setActive(int ped, bool active) {
    if (!ready() || ped == 0) {
        return;
    }

    active_ = active;
    movedAt_ = Clock::time_point{};

    // Разбор направления пишется заново на каждое включение: числа нужны ровно
    // тогда, когда полёт ведёт себя не так, как ожидалось.
    reported_ = !active;

    // Порядок обратный при включении и выключении: сперва снимаем физику, потом
    // двигаем; сперва ставим на место, потом возвращаем физику.
    invokeNative<void>(freeze_, ped, active);
    invokeNative<void>(setCollision_, ped, !active, !active);
}

shared::Vec3 Noclip::coordsOf(int entity) const {
    NativeContext context;
    context.push(entity);
    context.push(true);
    getCoords_(context.address());

    return shared::Vec3{context.result<float>(0), context.result<float>(1),
                        context.result<float>(2)};
}

shared::Vec3 Noclip::directionFromAngles() const {
    if (cameraRotation_ == nullptr) {
        return {};
    }

    NativeContext context;
    context.push(kRotationOrder);
    cameraRotation_(context.address());

    const float pitch = context.result<float>(0) * kDegreesToRadians;
    const float yaw = context.result<float>(2) * kDegreesToRadians;

    const float cosPitch = std::cos(pitch);

    return shared::Vec3{-std::sin(yaw) * cosPitch, std::cos(yaw) * cosPitch, std::sin(pitch)};
}

shared::Vec3 Noclip::directionFromGeometry(shared::Vec3 position) const {
    NativeContext context;
    cameraCoords_(context.address());

    const shared::Vec3 camera{context.result<float>(0), context.result<float>(1),
                              context.result<float>(2)};

    // Целится камера в грудь, а не в ступни, и разница здесь не мелочь: на
    // пяти метрах отрыва метр по высоте — это двенадцать градусов вниз.
    const shared::Vec3 towards{position.x - camera.x, position.y - camera.y,
                               position.z + kChestHeight - camera.z};

    if (length(towards) < kDegenerateCameraDistance) {
        return {};
    }

    return normalised(towards);
}

/// Направление, положенное на горизонт.
///
/// Именно им и летают. Направление взгляда годилось бы, будь камера там же, где
/// глаза, — но она висит позади и выше персонажа и смотрит на него сверху вниз:
/// градусов на десять даже тогда, когда игрок держит её ровно. Полёт «вперёд»
/// по такому направлению всё время сносит в землю, и это и было тем, что
/// оставалось кривым после всех исправлений: направление считалось верно, а
/// летело не туда, куда игрок целился.
///
/// Вверх и вниз теперь отдельными клавишами. Так устроен свободный полёт во всех
/// знакомых средствах отладки, и причина у всех одна и та же.
shared::Vec3 flatten(const shared::Vec3& direction) {
    const shared::Vec3 flat{direction.x, direction.y, 0.0F};

    if (length(flat) < kTinyLength) {
        // Взгляд строго вниз или строго вверх: горизонтального направления у
        // него нет вовсе, и брать его неоткуда.
        return {};
    }

    return normalised(flat);
}

shared::Vec3 Noclip::lookDirection(shared::Vec3 position) {
    const shared::Vec3 fromAngles = directionFromAngles();
    const shared::Vec3 fromGeometry = directionFromGeometry(position);

    const bool haveAngles = length(fromAngles) > kTinyLength;
    const bool haveGeometry = length(fromGeometry) > kTinyLength;

    if (!haveAngles) {
        return fromGeometry;
    }
    if (!haveGeometry) {
        // От первого лица сверять не с чем, и углы остаются единственным
        // источником. Полагаться на них тут можно: разошлись бы они с
        // геометрией — мы бы уже об этом узнали от третьего лица.
        return fromAngles;
    }

    const float agreement = fromAngles.x * fromGeometry.x + fromAngles.y * fromGeometry.y +
                            fromAngles.z * fromGeometry.z;

    const bool agree = agreement >= kAgreement;

    if (!reported_) {
        reported_ = true;

        spdlog::debug("свободный полёт: углы дают ({:.2f} {:.2f} {:.2f}), геометрия — "
                     "({:.2f} {:.2f} {:.2f}), сходство {:.2f} — берём {}",
                     fromAngles.x, fromAngles.y, fromAngles.z, fromGeometry.x, fromGeometry.y,
                     fromGeometry.z, agreement, agree ? "углы" : "геометрию");
    }

    return agree ? fromAngles : fromGeometry;
}

float Noclip::elapsed() {
    const Clock::time_point now = Clock::now();

    // Первый кадр после включения шага не даёт: сравнивать не с чем.
    if (movedAt_ == Clock::time_point{}) {
        movedAt_ = now;
        return 0.0F;
    }

    const float seconds = std::chrono::duration<float>{now - movedAt_}.count();
    movedAt_ = now;

    return std::min(seconds, kMaxStepSeconds);
}

void Noclip::update(int ped) {
    if (!ready() || !active_ || ped == 0) {
        return;
    }

    const shared::Vec3 position = coordsOf(ped);

    if (const shared::Vec3 looking = lookDirection(position); length(looking) > kTinyLength) {
        lastDirection = looking;
    }

    const shared::Vec3 forward = flatten(lastDirection);
    const shared::Vec3 right = rightOf(lastDirection);

    shared::Vec3 move{};

    if (keyDown('W')) {
        move.x += forward.x;
        move.y += forward.y;
    }
    if (keyDown('S')) {
        move.x -= forward.x;
        move.y -= forward.y;
    }
    if (keyDown('D')) {
        move.x += right.x;
        move.y += right.y;
    }
    if (keyDown('A')) {
        move.x -= right.x;
        move.y -= right.y;
    }
    if (keyDown(VK_SPACE)) {
        move.z += 1.0F;
    }
    if (keyDown(VK_CONTROL)) {
        move.z -= 1.0F;
    }

    // Время отсчитывается и на месте тоже: иначе первый шаг после стояния
    // вобрал бы в себя всю паузу.
    const float seconds = elapsed();

    if (length(move) < kTinyLength) {
        return;
    }

    // Нормировка обязательна: без неё «вперёд и вбок» разгоняло бы в полтора
    // раза сильнее, чем просто «вперёд».
    const shared::Vec3 step = normalised(move);
    const float distance = (keyDown(VK_SHIFT) ? kFastSpeed : kSpeed) * seconds;

    const shared::Vec3 destination{position.x + step.x * distance,
                                   position.y + step.y * distance,
                                   position.z + step.z * distance};

    // Последние признаки те же, что при обычном переносе: не искать землю и
    // считать перемещение мгновенным.
    invokeNative<void>(setCoords_, ped, destination.x, destination.y, destination.z, false, false,
                       false, true);

    // Персонаж разворачивается по камере, иначе он летит боком.
    invokeNative<void>(setHeading_, ped, headingOf(lastDirection));
}

} // namespace oxymp::client::game
