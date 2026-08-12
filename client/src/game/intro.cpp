#include "intro.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>

namespace oxymp::client::game {

void skipLegalScreens(const EngineAddresses& addresses) {
    auto* duration = addresses.pointerTo<std::uint32_t*>("legal_screen_duration");
    if (duration == nullptr) {
        return;
    }

    const std::uint32_t before = *duration;

    *duration = 0;

    // Значение перечитывается обратно, а не считается записанным. Запись в чужой
    // процесс может не лечь молча — например если по адресу лежит копия, а
    // читает игра другую, — и тогда «обнуляем» в журнале означало бы ровно
    // ничего.
    spdlog::info("юридическая заставка держалась {} мс, после записи {} мс", before, *duration);
}

} // namespace oxymp::client::game
