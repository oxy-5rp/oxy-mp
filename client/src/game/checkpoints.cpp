#include "checkpoints.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

/// Довод, которым игра выбирает цифру внутри точек 42–44. Остальным видам он не
/// нужен, и сами скрипты игры ставят здесь ноль.
constexpr int kNoReservedDigit = 0;

} // namespace

Checkpoints::Checkpoints(const NativeTable& table) noexcept
    : create_(table.handlerFor(natives::kCreateCheckpoint)),
      delete_(table.handlerFor(natives::kDeleteCheckpoint)),
      setHeight_(table.handlerFor(natives::kSetCheckpointCylinderHeight)),
      setColour_(table.handlerFor(natives::kSetCheckpointRgba)),
      setIconColour_(table.handlerFor(natives::kSetCheckpointIconRgba)) {}

bool Checkpoints::ready() const noexcept {
    return create_ != nullptr && delete_ != nullptr;
}

bool Checkpoints::sameShape(const shared::CheckpointState& left,
                            const shared::CheckpointState& right) noexcept {
    return left.type == right.type && left.position == right.position &&
           left.nextPosition == right.nextPosition && left.radius == right.radius;
}

int Checkpoints::create(const shared::CheckpointState& state) const {
    const int handle = invokeNative<int>(
        create_, static_cast<int>(state.type), state.position.x, state.position.y,
        state.position.z, state.nextPosition.x, state.nextPosition.y, state.nextPosition.z,
        state.radius, static_cast<int>(state.red), static_cast<int>(state.green),
        static_cast<int>(state.blue), static_cast<int>(state.alpha), kNoReservedDigit);

    if (handle == 0) {
        return 0;
    }

    // Высота задаётся отдельно от заведения, и обеими сразу: игра различает
    // столб вблизи и вдали, а различать их незачем — ресурс задал одну высоту.
    if (setHeight_ != nullptr) {
        invokeNative<void>(setHeight_, handle, state.height, state.height, state.radius);
    }

    // Цвет ставится и здесь, хотя он же назван при заведении. Не лишнее:
    // прозрачность игра при заведении принимает не для всех видов точек, а
    // этим нативом — для всех.
    if (setColour_ != nullptr) {
        invokeNative<void>(setColour_, handle, static_cast<int>(state.red),
                           static_cast<int>(state.green), static_cast<int>(state.blue),
                           static_cast<int>(state.alpha));
    }

    if (setIconColour_ != nullptr) {
        invokeNative<void>(setIconColour_, handle, static_cast<int>(state.iconRed),
                           static_cast<int>(state.iconGreen), static_cast<int>(state.iconBlue),
                           static_cast<int>(state.iconAlpha));
    }

    return handle;
}

void Checkpoints::destroy(int handle) const {
    if (handle != 0 && delete_ != nullptr) {
        invokeNative<void>(delete_, handle);
    }
}

void Checkpoints::apply(const shared::CheckpointState& state) {
    if (!ready()) {
        return;
    }

    const auto known = shown_.find(state.id);

    if (known != shown_.end()) {
        Shown& kept = known->second;

        // Место, вид и радиус игра принимает только при заведении. Совпали —
        // довольно перекрасить; разошлись — точку придётся завести заново, и
        // другого пути к ней нет.
        const bool rebuild = kept.handle != 0 && !sameShape(kept.state, state);

        if (rebuild) {
            destroy(kept.handle);
            kept.handle = 0;
        }

        kept.state = state;

        if (!state.visible) {
            // Погасить заведённую точку игре нечем: у неё нет ни признака
            // видимости, ни прозрачности, которая убрала бы столб целиком.
            // Поэтому гашение — это снятие, а состояние остаётся у нас: точка
            // вернётся, когда ресурс её зажжёт.
            destroy(kept.handle);
            kept.handle = 0;
            return;
        }

        if (kept.handle != 0) {
            if (setColour_ != nullptr) {
                invokeNative<void>(setColour_, kept.handle, static_cast<int>(state.red),
                                   static_cast<int>(state.green), static_cast<int>(state.blue),
                                   static_cast<int>(state.alpha));
            }

            if (setIconColour_ != nullptr) {
                invokeNative<void>(setIconColour_, kept.handle, static_cast<int>(state.iconRed),
                                   static_cast<int>(state.iconGreen),
                                   static_cast<int>(state.iconBlue),
                                   static_cast<int>(state.iconAlpha));
            }

            if (setHeight_ != nullptr) {
                invokeNative<void>(setHeight_, kept.handle, state.height, state.height,
                                   state.radius);
            }

            return;
        }

        kept.handle = create(state);

        if (kept.handle == 0) {
            spdlog::warn("checkpoint {} was not created", state.id);
        }

        return;
    }

    Shown fresh;
    fresh.state = state;
    fresh.handle = state.visible ? create(state) : 0;

    if (state.visible && fresh.handle == 0) {
        spdlog::warn("checkpoint {} was not created", state.id);
    }

    shown_.insert_or_assign(state.id, fresh);
}

void Checkpoints::remove(shared::CheckpointId id) {
    const auto known = shown_.find(id);
    if (known == shown_.end()) {
        return;
    }

    destroy(known->second.handle);
    shown_.erase(known);
}

void Checkpoints::clear() {
    for (const auto& [id, kept] : shown_) {
        destroy(kept.handle);
    }

    shown_.clear();
}

} // namespace oxymp::client::game
