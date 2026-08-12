#include "streaming.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

/// Радиус сферы подгрузки, в метрах.
///
/// Полсотни: этого хватает, чтобы под ногами оказалась земля со столкновениями и
/// ближайшие дома. Больше — дольше ждать, а игрок всё это время стоит.
constexpr float kLoadRadius = 50.0F;

/// Сколько ждать подгрузки, в миллисекундах.
///
/// Срок обязателен: сферу, которую игре нечем заполнить, не догрузить никогда.
/// Лучше отпустить игрока в недогруженный мир, чем держать его замороженным
/// навсегда.
constexpr std::int32_t kLoadTimeout = 5000;

} // namespace

Streaming::Streaming(const NativeTable& table) noexcept
    : requestCollision_(table.handlerFor(natives::kRequestCollisionAtCoord)),
      loadCollisionFlag_(table.handlerFor(natives::kSetEntityLoadCollisionFlag)),
      startScene_(table.handlerFor(natives::kNewLoadSceneStartSphere)),
      sceneLoaded_(table.handlerFor(natives::kIsNewLoadSceneLoaded)),
      stopScene_(table.handlerFor(natives::kNewLoadSceneStop)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)) {}

bool Streaming::ready() const noexcept {
    return requestCollision_ != nullptr && startScene_ != nullptr && sceneLoaded_ != nullptr &&
           stopScene_ != nullptr && gameTimer_ != nullptr;
}

void Streaming::keepCollisionAround(int ped) const {
    if (ped == 0) {
        return;
    }

    if (loadCollisionFlag_ != nullptr) {
        invokeNative<void>(loadCollisionFlag_, ped, true);
    }
}

void Streaming::beginLoad(shared::Vec3 point) {
    if (!ready()) {
        return;
    }

    // Столкновения просятся отдельно от сферы, и это не одно и то же: сфера
    // подгружает всё, а эта просьба — именно то, обо что можно опереться.
    invokeNative<void>(requestCollision_, point.x, point.y, point.z);

    // Прежняя сфера снимается: их не бывает двух, и оставленная перекрыла бы
    // новую.
    if (loading_) {
        invokeNative<void>(stopScene_);
    }

    // Последний довод — признаки подгрузки. Ноль означает обычную.
    invokeNative<void>(startScene_, point.x, point.y, point.z, kLoadRadius, 0);

    loading_ = true;
    startedAt_ = invokeNative<std::int32_t>(gameTimer_);
}

bool Streaming::advance() {
    if (!loading_) {
        return true;
    }

    const std::int32_t now = invokeNative<std::int32_t>(gameTimer_);
    const std::int32_t waited = now - startedAt_;

    const bool loaded = invokeNative<bool>(sceneLoaded_);

    if (!loaded && waited < kLoadTimeout) {
        return false;
    }

    invokeNative<void>(stopScene_);
    loading_ = false;

    if (loaded) {
        spdlog::debug("мир вокруг точки подгружен за {} мс", waited);
    } else {
        spdlog::warn("мир вокруг точки не догрузился за {} мс — отпускаем игрока как есть",
                     waited);
    }

    return true;
}

} // namespace oxymp::client::game
