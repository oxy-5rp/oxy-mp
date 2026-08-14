#include "interpolation.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oxymp::client::interpolation {
namespace {

/// Полный оборот и половина оборота в градусах.
constexpr float kFullTurn = 360.0F;
constexpr float kHalfTurn = 180.0F;

/// Сколько градусов в радиане. Угловая скорость приходит от игры в радианах, а
/// углы поворота — в градусах: игра отдаёт их в разных единицах, и складывать их
/// без пересчёта нельзя.
constexpr float kDegreesPerRadian = 180.0F / std::numbers::pi_v<float>;

/// Приводит угол к промежутку от минус половины оборота до половины.
float wrap(float degrees) noexcept {
    return std::fmod(degrees + kHalfTurn + kFullTurn, kFullTurn) - kHalfTurn;
}

} // namespace

Blend blend(std::chrono::milliseconds span, std::chrono::nanoseconds since) noexcept {
    // Показываем не последний снимок, а положение на kDelay позади него: между
    // снимками должно оставаться что показывать.
    const float ahead = std::chrono::duration<float>{since - kDelay}.count();

    if (ahead > 0.0F) {
        // Свежих снимков нет дольше отставания: достраиваем движение по
        // последней известной скорости, но не бесконечно.
        const float limit = std::chrono::duration<float>{kMaxExtrapolation}.count();
        return Blend{.progress = 1.0F, .ahead = std::min(ahead, limit)};
    }

    const float seconds = std::chrono::duration<float>{span}.count();

    if (seconds <= 0.0F) {
        // Снимок пока один — смешивать не с чем.
        return Blend{};
    }

    // ahead здесь отрицателен: он говорит, насколько мы не дошли до последнего
    // снимка. Отсчёт ведётся от него назад, а не от предыдущего вперёд, — так
    // доля не зависит от того, когда снимок пришёл.
    return Blend{.progress = std::clamp(1.0F + ahead / seconds, 0.0F, 1.0F), .ahead = 0.0F};
}

float catchUp(float rate, float seconds) noexcept {
    if (rate <= 0.0F || seconds <= 0.0F) {
        // Время не шло — закрывать нечего. Ноль здесь честнее любой доли: кадр
        // нулевой длины случается на первом же кадре, когда сравнивать не с чем.
        return 0.0F;
    }

    return std::clamp(1.0F - std::exp(-rate * seconds), 0.0F, 1.0F);
}

shared::Vec3 mix(const shared::Vec3& from, const shared::Vec3& to, float progress) noexcept {
    return shared::Vec3{
        std::lerp(from.x, to.x, progress),
        std::lerp(from.y, to.y, progress),
        std::lerp(from.z, to.z, progress),
    };
}

shared::Vec3 advance(const shared::Vec3& position, const shared::Vec3& velocity,
                     float seconds) noexcept {
    return shared::Vec3{
        position.x + velocity.x * seconds,
        position.y + velocity.y * seconds,
        position.z + velocity.z * seconds,
    };
}

float mixAngle(float from, float to, float progress) noexcept {
    return wrap(from + wrap(to - from) * progress);
}

shared::Vec3 mixAngles(const shared::Vec3& from, const shared::Vec3& to,
                       float progress) noexcept {
    return shared::Vec3{
        mixAngle(from.x, to.x, progress),
        mixAngle(from.y, to.y, progress),
        mixAngle(from.z, to.z, progress),
    };
}

shared::Vec3 advanceAngles(const shared::Vec3& angles, const shared::Vec3& angularVelocity,
                           float seconds) noexcept {
    return shared::Vec3{
        wrap(angles.x + angularVelocity.x * kDegreesPerRadian * seconds),
        wrap(angles.y + angularVelocity.y * kDegreesPerRadian * seconds),
        wrap(angles.z + angularVelocity.z * kDegreesPerRadian * seconds),
    };
}

} // namespace oxymp::client::interpolation
