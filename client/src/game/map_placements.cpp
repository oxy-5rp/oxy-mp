#include "map_placements.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace oxymp::client::game {

MapPlacements::MapPlacements(const NativeTable& table) noexcept
    : request_(table.handlerFor(natives::kRequestIpl)),
      active_(table.handlerFor(natives::kIsIplActive)) {}

bool MapPlacements::ready() const noexcept {
    return request_ != nullptr;
}

void MapPlacements::add(std::string name) {
    if (name.empty()) {
        return;
    }

    const std::lock_guard guard{mutex_};

    if (!asked_.insert(name).second) {
        return;
    }

    pending_.push_back(std::move(name));
}

std::size_t MapPlacements::pump() {
    std::vector<std::string> batch;

    {
        const std::lock_guard guard{mutex_};
        batch.swap(pending_);
    }

    if (batch.empty() || !ready()) {
        return 0;
    }

    std::size_t standing = 0;

    for (const std::string& name : batch) {
        invokeNative<void>(request_, name.c_str());

        // Спрашиваем сразу же, а не считаем просьбы: имя, которого игра не
        // знает, она пропускает молча, и по числу просьб не отличить
        // поставленную карту от бесследно пропавшей.
        if (active_ != nullptr && invokeNative<bool>(active_, name.c_str())) {
            ++standing;
        }
    }

    if (standing == batch.size()) {
        spdlog::info("Map placements standing: {}", standing);
    } else {
        // Разница важнее самих чисел: она означает, что часть расстановки игра
        // не признала, и искать это надо в именах, а не в чём-то ещё.
        spdlog::warn("Map placements: {} of {} stood up; the game does not know the rest",
                     standing, batch.size());
    }

    return standing;
}

} // namespace oxymp::client::game
