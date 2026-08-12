#include "world.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Множитель плотности населения. Ноль означает «не создавать вовсе».
constexpr float kNoPopulation = 0.0F;

/// Сколько служб реагирования у игры.
///
/// Нумерация сплошная и начинается с единицы: полиция, скорая, пожарные,
/// вертолёты и прочие. Перебор по номерам вместо поимённого списка выбран
/// потому, что имена этих номеров игра нигде не хранит, а выключить нужно все.
constexpr int kFirstDispatchService = 1;
constexpr int kLastDispatchService = 15;

/// Обычный ход времени.
constexpr float kNormalTimeScale = 1.0F;

} // namespace

World::World(const NativeTable& table) noexcept
    : pedDensity_(table.handlerFor(natives::kSetPedDensityMultiplier)),
      scenarioPedDensity_(table.handlerFor(natives::kSetScenarioPedDensityMultiplier)),
      vehicleDensity_(table.handlerFor(natives::kSetVehicleDensityMultiplier)),
      randomVehicleDensity_(table.handlerFor(natives::kSetRandomVehicleDensityMultiplier)),
      parkedVehicleDensity_(table.handlerFor(natives::kSetParkedVehicleDensityMultiplier)),
      randomCops_(table.handlerFor(natives::kSetCreateRandomCops)),
      randomCopsNotOnScenarios_(table.handlerFor(natives::kSetCreateRandomCopsNotOnScenarios)),
      garbageTrucks_(table.handlerFor(natives::kSetGarbageTrucks)),
      randomBoats_(table.handlerFor(natives::kSetRandomBoats)),
      randomTrains_(table.handlerFor(natives::kSetRandomTrains)),
      deleteAllTrains_(table.handlerFor(natives::kDeleteAllTrains)),
      clearPeds_(table.handlerFor(natives::kClearAreaOfPeds)),
      clearVehicles_(table.handlerFor(natives::kClearAreaOfVehicles)),
      clearCops_(table.handlerFor(natives::kClearAreaOfCops)),
      maxWanted_(table.handlerFor(natives::kSetMaxWantedLevel)),
      clearWanted_(table.handlerFor(natives::kClearPlayerWantedLevel)),
      policeIgnore_(table.handlerFor(natives::kSetPoliceIgnorePlayer)),
      dispatchService_(table.handlerFor(natives::kEnableDispatchService)),
      timeScale_(table.handlerFor(natives::kSetTimeScale)) {}

bool World::ready() const noexcept {
    return pedDensity_ != nullptr && scenarioPedDensity_ != nullptr && vehicleDensity_ != nullptr &&
           randomVehicleDensity_ != nullptr && parkedVehicleDensity_ != nullptr &&
           randomCops_ != nullptr && randomCopsNotOnScenarios_ != nullptr &&
           garbageTrucks_ != nullptr && randomBoats_ != nullptr && randomTrains_ != nullptr &&
           deleteAllTrains_ != nullptr && clearPeds_ != nullptr && clearVehicles_ != nullptr &&
           clearCops_ != nullptr && maxWanted_ != nullptr && clearWanted_ != nullptr &&
           policeIgnore_ != nullptr && dispatchService_ != nullptr && timeScale_ != nullptr;
}

void World::keepTimeFlowing() const {
    if (timeScale_ == nullptr) {
        return;
    }

    invokeNative<void>(timeScale_, kNormalTimeScale);
}

void World::suppressPopulation() const {
    if (!ready()) {
        return;
    }

    invokeNative<void>(pedDensity_, kNoPopulation);
    invokeNative<void>(scenarioPedDensity_, kNoPopulation, kNoPopulation);
    invokeNative<void>(vehicleDensity_, kNoPopulation);
    invokeNative<void>(randomVehicleDensity_, kNoPopulation);
    invokeNative<void>(parkedVehicleDensity_, kNoPopulation);

    // Эти четыре, в отличие от множителей, держатся сами и повторного вызова не
    // требуют. Он тем не менее делается: сюжетные скрипты игры продолжают
    // работать и вправе включить всё обратно в любой момент.
    invokeNative<void>(randomCops_, false);
    invokeNative<void>(randomCopsNotOnScenarios_, false);
    invokeNative<void>(garbageTrucks_, false);
    invokeNative<void>(randomBoats_, false);
    invokeNative<void>(randomTrains_, false);
}

void World::suppressWanted(int player) const {
    if (!ready()) {
        return;
    }

    invokeNative<void>(maxWanted_, 0);
    invokeNative<void>(clearWanted_, player);
    invokeNative<void>(policeIgnore_, player, true);

    for (int service = kFirstDispatchService; service <= kLastDispatchService; ++service) {
        invokeNative<void>(dispatchService_, service, false);
    }
}

void World::clearArea(shared::Vec3 centre, float radius) const {
    if (!ready()) {
        return;
    }

    // Последние признаки нативов очистки означают «не щадить сюжетные объекты»:
    // прохожий, поставленный миссией, для нас такой же лишний, как случайный.
    invokeNative<void>(clearPeds_, centre.x, centre.y, centre.z, radius, false);
    invokeNative<void>(clearVehicles_, centre.x, centre.y, centre.z, radius, false, false, false,
                       false, false);
    invokeNative<void>(clearCops_, centre.x, centre.y, centre.z, radius, false);
    invokeNative<void>(deleteAllTrains_);
}

} // namespace oxymp::client::game
