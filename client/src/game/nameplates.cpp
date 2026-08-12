#include "nameplates.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace oxymp::client::game {
namespace {

/// Насколько подпись поднята над точкой, в которой стоит персонаж, в метрах.
///
/// Метр без малого: положение приходит от ног, а рост персонажа игры — около
/// метра восьмидесяти. Подпись должна висеть чуть выше макушки, а не на ней.
constexpr float kHeightAboveFeet = 1.1F;

/// Дальше этого подписи не рисуются, в метрах.
///
/// Не ради нагрузки — надписей десятки, а не тысячи, — а ради читаемости: сотня
/// подписей, слипшихся у горизонта в одну строку, мешает больше, чем помогает.
constexpr float kVisibleDistance = 120.0F;

/// Дальность, до которой подпись рисуется в полную величину.
constexpr float kFullSizeDistance = 15.0F;

/// Размеры подписи вблизи и у предела видимости.
constexpr float kNearScale = 0.34F;
constexpr float kFarScale = 0.20F;

/// Цвет подписи. Один на всех: раскрашивать игроков по-разному пока нечем —
/// команд в сессии нет.
constexpr Colour kNameColour{235, 240, 250, 255};

/// Цвет подписи мертвеца.
constexpr Colour kDeadColour{200, 110, 105, 255};

float distanceBetween(const shared::Vec3& from, const shared::Vec3& to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;

    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

Nameplates::Nameplates(const NativeTable& table, const Hud& hud) noexcept
    : hud_(hud),
      setOrigin_(table.handlerFor(natives::kSetDrawOrigin)),
      clearOrigin_(table.handlerFor(natives::kClearDrawOrigin)) {}

bool Nameplates::ready() const noexcept {
    return setOrigin_ != nullptr && clearOrigin_ != nullptr && hud_.ready();
}

void Nameplates::draw(const std::vector<RemotePlayerView>& players, shared::Vec3 viewer) const {
    if (!ready()) {
        return;
    }

    for (const RemotePlayerView& player : players) {
        const float distance = distanceBetween(viewer, player.state.position);
        if (distance > kVisibleDistance) {
            continue;
        }

        // Последний довод — «учитывать ли перекрытие». Ноль: подпись видна и
        // сквозь стену. Так задумано — в сессии важнее знать, кто где, чем
        // соблюсти честность видимости, и так же ведут себя подписи в GTA
        // Online.
        invokeNative<void>(setOrigin_, player.state.position.x, player.state.position.y,
                           player.state.position.z + kHeightAboveFeet, 0);

        const float nearness =
            std::clamp((distance - kFullSizeDistance) / (kVisibleDistance - kFullSizeDistance),
                       0.0F, 1.0F);
        const float scale = std::lerp(kNearScale, kFarScale, nearness);

        const bool dead = shared::has(player.state.flags, shared::PlayerFlag::Dead);

        // Координаты после переноса начала — это уже не доли экрана, а смещение
        // от точки в мире. Ноль означает «ровно в ней».
        hud_.drawText(std::format("{} [{}]", player.nickname, player.id), 0.0F, 0.0F, scale,
                      dead ? kDeadColour : kNameColour, true);

        invokeNative<void>(clearOrigin_);
    }
}

} // namespace oxymp::client::game
