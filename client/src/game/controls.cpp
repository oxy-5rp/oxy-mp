#include "controls.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Наборы управления, из которых игра читает нажатия. Ноль — игровой, двойка —
/// меню. Запрет и подача идут в оба: какой из них читает нужное нам место, со
/// стороны не видно, а лишний ничему не мешает.
constexpr int kControlGroups[] = {0, 2};

/// Номер действия «ввести читкод» в наборе управления.
constexpr int kCheatAction = 243;

/// Номер действия «колесо выбора персонажа» — та самая клавиша Tab.
constexpr int kCharacterWheelAction = 19;

} // namespace

Controls::Controls(const NativeTable& table) noexcept
    : disableAction_(table.handlerFor(natives::kDisableControlAction)),
      disableAll_(table.handlerFor(natives::kDisableAllControlActions)) {}

bool Controls::ready() const noexcept {
    return disableAction_ != nullptr && disableAll_ != nullptr;
}

void Controls::suppressCheats() const {
    if (disableAction_ == nullptr) {
        return;
    }

    for (const int group : kControlGroups) {
        invokeNative<void>(disableAction_, group, kCheatAction, true);
    }
}

void Controls::suppressCharacterWheel() const {
    if (disableAction_ == nullptr) {
        return;
    }

    for (const int group : kControlGroups) {
        invokeNative<void>(disableAction_, group, kCharacterWheelAction, true);
    }
}


void Controls::suppressEverything() const {
    if (disableAll_ == nullptr) {
        return;
    }

    for (const int group : kControlGroups) {
        invokeNative<void>(disableAll_, group);
    }
}

} // namespace oxymp::client::game
