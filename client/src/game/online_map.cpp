#include "online_map.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {

// SET_INSTANCE_PRIORITY_MODE отсюда убран, и это не уборка, а починка.
//
// Звали его затем, чтобы отменить поведение сетевого режима: тот отдаёт
// предпочтение другим игрокам в ущерб окружению, а игроков в нашей сессии
// заведомо меньше, чем рассчитывала Rockstar.
//
// Довод не работает по двум причинам, и каждой хватило бы.
//
// Первая: отменять нечего. Поведение, которое компенсировалось, включается в
// сетевой сессии, а игра в неё не входит — признак сетевой игры равен нулю, это
// измерено, а не предположено.
//
// Вторая: сам натив портит то, ради чего его звали. В CitizenFX он занесён в
// список запрещённых во всех сборках с пометкой «понижение плотности пропов» —
// то есть ровно то, что выглядит как проплешины на карте: земля без текстуры и
// растительность поверх неё. Хеш сверен по их же таблице соответствий сборок:
// 0x2268617D0B5A5B35 и канонический 0x9BAE5AD2508DF078 — один и тот же натив,
// разные сборки.
//
// Ноль при этом выглядел безобидным значением, и в этом была ловушка: вредным
// оказался не довод, а самый факт вызова.

OnlineMap::OnlineMap(const NativeTable& table) noexcept
    : onEnterMp_(table.handlerFor(natives::kOnEnterMp)),
      gameInProgress_(table.handlerFor(natives::kNetworkIsGameInProgress)) {}

bool OnlineMap::ready() const noexcept {
    return onEnterMp_ != nullptr && gameInProgress_ != nullptr;
}

bool OnlineMap::active() const {
    if (gameInProgress_ == nullptr) {
        return false;
    }

    return invokeNative<bool>(gameInProgress_);
}

void OnlineMap::enable() const {
    if (!ready()) {
        return;
    }

    invokeNative<void>(onEnterMp_);

    spdlog::debug("мир переключён на карту сетевого режима");
}

} // namespace oxymp::client::game
