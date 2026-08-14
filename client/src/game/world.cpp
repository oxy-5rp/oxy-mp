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
      timeScale_(table.handlerFor(natives::kSetTimeScale)),
      setWeather_(table.handlerFor(natives::kSetWeatherTypeNow)),
      setClock_(table.handlerFor(natives::kOverrideClockTime)),
      vehicleBudget_(table.handlerFor(natives::kSetVehiclePopulationBudget)),
      pedBudget_(table.handlerFor(natives::kSetPedPopulationBudget)),
      parkedVehicles_(table.handlerFor(natives::kSetNumberOfParkedVehicles)),
      lowPriorityGenerators_(
          table.handlerFor(natives::kSetAllLowPriorityVehicleGeneratorsActive)),
      clearGenerators_(table.handlerFor(natives::kRemoveVehiclesFromGeneratorsInArea)),
      closestVehicle_(table.handlerFor(natives::kGetClosestVehicle)),
      isMissionEntity_(table.handlerFor(natives::kIsEntityAMissionEntity)),
      deleteVehicle_(table.handlerFor(natives::kDeleteVehicle)) {}

void World::sweepStrayVehicle(shared::Vec3 centre, float radius) const {
    if (closestVehicle_ == nullptr || isMissionEntity_ == nullptr || deleteVehicle_ == nullptr) {
        return;
    }

    // Признаки поиска: любая модель (ноль) и обычный набор условий — на колёсах,
    // не горит, не в воде. Семьдесят — то же число, которым пользуются сами
    // скрипты игры, когда ищут машину поблизости.
    constexpr std::uint32_t kAnyModel = 0;
    constexpr int kOrdinaryVehicle = 70;

    const int vehicle = invokeNative<int>(closestVehicle_, centre.x, centre.y, centre.z, radius,
                                          kAnyModel, kOrdinaryVehicle);

    if (vehicle == 0) {
        return;
    }

    // Наша машина принадлежит скрипту — мы сами её такой пометили при создании.
    // Случайная принадлежит миру, и только её здесь и убирают.
    if (invokeNative<bool>(isMissionEntity_, vehicle)) {
        return;
    }

    int handle = vehicle;

    NativeContext context;
    context.push(&handle);
    deleteVehicle_(context.address());
}

void World::applyWorldState(const shared::WorldState& state) {
    // Погода — только на изменение. Натив меняет её мгновенно и рвёт плавный
    // переход, а сервер повторяет одно и то же раз в две секунды: ставь мы её
    // каждый раз — небо дёргалось бы всю сессию.
    if (setWeather_ != nullptr && !state.weather.empty() && state.weather != weather_) {
        invokeNative<void>(setWeather_, state.weather.c_str());
        weather_ = state.weather;
    }

    // Время — каждый раз, и это верно: между рассылками часы у клиента стоят,
    // а идущие сами по себе разошлись бы у всех по-разному. Раз в две секунды
    // сервер сдвигает их ровно на игровую минуту — как они и тикают у игры.
    if (setClock_ != nullptr) {
        invokeNative<void>(setClock_, static_cast<int>(state.hour), static_cast<int>(state.minute),
                           static_cast<int>(state.second));
    }
}

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

    // Множителей плотности мало, и это выяснилось на живой игре: машины с
    // водителями продолжали появляться. Множитель говорит, сколько заводить
    // сверх уже намеченного, а намечает игра заранее — двумя другими способами.
    //
    // Запас населения — сколько всего игра готова держать на свете. Нулевой
    // запас не даёт ей наметить никого.
    if (vehicleBudget_ != nullptr) {
        invokeNative<void>(vehicleBudget_, 0);
    }
    if (pedBudget_ != nullptr) {
        invokeNative<void>(pedBudget_, 0);
    }

    // Точки появления машин расставлены по карте заранее и от плотности не
    // зависят вовсе: из них машина выезжает и едет по своим делам. Минус
    // единица означает «ни одной», а не «сколько было».
    if (parkedVehicles_ != nullptr) {
        invokeNative<void>(parkedVehicles_, -1);
    }
    if (lowPriorityGenerators_ != nullptr) {
        invokeNative<void>(lowPriorityGenerators_, false);
    }
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
