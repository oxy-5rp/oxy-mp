#include "markers.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Довод, у которого нет назначения.
///
/// В подписи натива он зовётся p19, и в скриптах самой игры на его месте стоит
/// двойка. Что она означает, не знает и открытая база: «no effect, default value
/// in script is 2». Ставим то же — не потому, что верим в её смысл, а потому,
/// что отличаться от игры без причины дороже.
constexpr int kUnusedMarkerArgument = 2;

} // namespace

Markers::Markers(const NativeTable& table) noexcept
    : drawMarker_(table.handlerFor(natives::kDrawMarker)) {}

bool Markers::ready() const noexcept {
    return drawMarker_ != nullptr;
}

void Markers::apply(const shared::MarkerState& state) {
    markers_.insert_or_assign(state.id, state);
}

void Markers::remove(shared::MarkerId id) {
    markers_.erase(id);
}

void Markers::clear() {
    markers_.clear();
}

void Markers::draw(const shared::Vec3& viewer) const {
    if (!ready()) {
        return;
    }

    for (const auto& [id, marker] : markers_) {
        if (!marker.visible) {
            continue;
        }

        // Ноль означает «видно отовсюду», и так же его толкует alt:V. Сравнение
        // по квадратам: корень здесь не нужен никому.
        if (marker.streamingDistance > 0.0F &&
            shared::distanceSquared(viewer, marker.position) >
                marker.streamingDistance * marker.streamingDistance) {
            continue;
        }

        // Текстура не задаётся: у alt:V её у маркера нет вовсе, и подставлять
        // сюда пустые строки нельзя — натив ждёт указатель, а не строку, и
        // ноль для него означает «текстуры нет». Именно так его зовут и сами
        // скрипты игры.
        invokeNative<void>(drawMarker_, static_cast<int>(marker.type), marker.position.x,
                           marker.position.y, marker.position.z, marker.direction.x,
                           marker.direction.y, marker.direction.z, marker.rotation.x,
                           marker.rotation.y, marker.rotation.z, marker.scale.x, marker.scale.y,
                           marker.scale.z, static_cast<int>(marker.red),
                           static_cast<int>(marker.green), static_cast<int>(marker.blue),
                           static_cast<int>(marker.alpha), marker.bobUpAndDown,
                           marker.faceCamera, kUnusedMarkerArgument, marker.rotate,
                           static_cast<const char*>(nullptr), static_cast<const char*>(nullptr),
                           false);
    }
}

} // namespace oxymp::client::game
