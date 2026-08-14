#pragma once

namespace oxymp::shared {

/// Точка или направление в мировых координатах игры.
struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    friend bool operator==(const Vec3&, const Vec3&) = default;
};

/// Квадрат расстояния между двумя точками.
///
/// Квадрат, а не расстояние: корень здесь не нужен никому. Расстояния сравнивают
/// — с порогом или друг с другом, — а порядок у квадратов тот же самый.
[[nodiscard]] constexpr float distanceSquared(const Vec3& from, const Vec3& to) noexcept {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;

    return (dx * dx) + (dy * dy) + (dz * dz);
}

} // namespace oxymp::shared
