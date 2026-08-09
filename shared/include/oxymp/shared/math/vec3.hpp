#pragma once

namespace oxymp::shared {

/// Точка или направление в мировых координатах игры.
struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    friend bool operator==(const Vec3&, const Vec3&) = default;
};

} // namespace oxymp::shared
