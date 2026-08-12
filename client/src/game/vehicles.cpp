#include "vehicles.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace oxymp::client::game {
namespace {

/// Сколько пассажирских мест перебирается в поисках персонажа.
///
/// Восемь с запасом: у самой вместительной машины игры мест меньше, а лишний
/// пустой опрос не стоит ничего.
constexpr int kMaxPassengerSeats = 8;

/// Дальность, с которой машина остаётся видимой, в метрах.
constexpr int kLodDistance = 1000;

/// Порядок углов поворота. Двойка — тот же, что и у камеры, и тот, в котором
/// игра отдаёт и принимает поворот машины без пересчёта.
constexpr int kRotationOrder = 2;

/// Насколько машина должна разойтись со снимком, чтобы её переставить рывком.
///
/// Обычно расхождение закрывается плавно, но после потери связи или въезда в
/// туннель машина оказывается за сотню метров, и доводить её туда плавно — это
/// показать, как она едет сквозь дома.
constexpr float kSnapDistance = 15.0F;

/// Какую долю расхождения съедать за кадр, когда оно невелико.
constexpr float kCorrectionRate = 0.35F;

float distanceBetween(const shared::Vec3& from, const shared::Vec3& to) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;

    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

Vehicles::Vehicles(const NativeTable& table) noexcept
    : requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      modelNoLongerNeeded_(table.handlerFor(natives::kSetModelAsNoLongerNeeded)),
      createVehicle_(table.handlerFor(natives::kCreateVehicle)),
      deleteVehicle_(table.handlerFor(natives::kDeleteVehicle)),
      doesExist_(table.handlerFor(natives::kDoesEntityExist)),
      setCoords_(table.handlerFor(natives::kSetEntityCoordsNoOffset)),
      setRotation_(table.handlerFor(natives::kSetEntityRotation)),
      setVelocity_(table.handlerFor(natives::kSetEntityVelocity)),
      getRotation_(table.handlerFor(natives::kGetEntityRotation)),
      getVelocity_(table.handlerFor(natives::kGetEntityVelocity)),
      getCoords_(table.handlerFor(natives::kGetEntityCoords)),
      getModel_(table.handlerFor(natives::kGetEntityModel)),
      engineOn_(table.handlerFor(natives::kSetVehicleEngineOn)),
      asMissionEntity_(table.handlerFor(natives::kSetEntityAsMissionEntity)),
      lodDistance_(table.handlerFor(natives::kSetEntityLodDist)),
      invincible_(table.handlerFor(natives::kSetEntityInvincible)),
      isInAnyVehicle_(table.handlerFor(natives::kIsPedInAnyVehicle)),
      vehiclePedIsIn_(table.handlerFor(natives::kGetVehiclePedIsIn)),
      pedInSeat_(table.handlerFor(natives::kGetPedInVehicleSeat)) {}

Vehicles::~Vehicles() {
    // Машины здесь уже не убрать: разрушение приходится на выгрузку модуля, а
    // она случается вне скриптового тика, где нативы звать нельзя. Убирать их
    // положено вызовом clear до этого момента.
}

bool Vehicles::ready() const noexcept {
    return requestModel_ != nullptr && hasModelLoaded_ != nullptr && createVehicle_ != nullptr &&
           deleteVehicle_ != nullptr && doesExist_ != nullptr && setCoords_ != nullptr &&
           setRotation_ != nullptr && setVelocity_ != nullptr && getRotation_ != nullptr &&
           getVelocity_ != nullptr && getCoords_ != nullptr && getModel_ != nullptr &&
           engineOn_ != nullptr && asMissionEntity_ != nullptr && lodDistance_ != nullptr &&
           isInAnyVehicle_ != nullptr && vehiclePedIsIn_ != nullptr && pedInSeat_ != nullptr;
}

std::optional<Vehicles::Seat> Vehicles::seatOf(int ped) const {
    if (!ready() || ped == 0) {
        return std::nullopt;
    }

    if (!invokeNative<bool>(isInAnyVehicle_, ped, false)) {
        return std::nullopt;
    }

    // Последний довод: не считать машиной ту, в которую персонаж только
    // залезает. Пока он висит на подножке, машина ещё не его, и объявить её
    // своей значит начать возить её по сети за чужой счёт.
    const int vehicle = invokeNative<int>(vehiclePedIsIn_, ped, false);
    if (vehicle == 0) {
        return std::nullopt;
    }

    Seat seat;
    seat.vehicle = vehicle;
    seat.driverPed = invokeNative<int>(pedInSeat_, vehicle, shared::kDriverSeat);

    if (seat.driverPed == ped) {
        seat.index = shared::kDriverSeat;
        return seat;
    }

    for (int index = 0; index < kMaxPassengerSeats; ++index) {
        if (invokeNative<int>(pedInSeat_, vehicle, index) == ped) {
            seat.index = static_cast<std::int8_t>(index);
            return seat;
        }
    }

    // Сидит, но места не нашлось: такое бывает у мест, которых у этой модели
    // нет в обычной нумерации. Считаем пассажиром на первом месте — это лучше,
    // чем объявить идущим пешком того, кто едет.
    seat.index = 0;
    return seat;
}

shared::VehicleState Vehicles::describe(int vehicle) const {
    shared::VehicleState state;

    if (!ready() || vehicle == 0) {
        return state;
    }

    state.model = invokeNative<std::uint32_t>(getModel_, vehicle);

    {
        NativeContext context;
        context.push(vehicle);
        context.push(true);
        getCoords_(context.address());

        state.position = shared::Vec3{context.result<float>(0), context.result<float>(1),
                                      context.result<float>(2)};
    }

    {
        NativeContext context;
        context.push(vehicle);
        context.push(kRotationOrder);
        getRotation_(context.address());

        state.rotation = shared::Vec3{context.result<float>(0), context.result<float>(1),
                                      context.result<float>(2)};
    }

    {
        NativeContext context;
        context.push(vehicle);
        getVelocity_(context.address());

        state.velocity = shared::Vec3{context.result<float>(0), context.result<float>(1),
                                      context.result<float>(2)};
    }

    return state;
}

int Vehicles::spawn(const shared::VehicleState& state) {
    if (state.model == 0) {
        return 0;
    }

    invokeNative<void>(requestModel_, state.model);
    if (!invokeNative<bool>(hasModelLoaded_, state.model)) {
        return 0;
    }

    // Последние признаки: машина не сетевая и не принадлежит скрипту. Сетевых в
    // одиночной игре не бывает, а принадлежность скрипту передаётся отдельно,
    // ниже. Третий передаётся явно, чтобы не полагаться на то, что игра
    // прочитает ноль из необъявленной ячейки нашего же контекста.
    const int vehicle =
        invokeNative<int>(createVehicle_, state.model, state.position.x, state.position.y,
                          state.position.z, 0.0F, false, false, false);
    if (vehicle == 0) {
        return 0;
    }

    // Машина принадлежит нам, а не миру: иначе игра вправе убрать её как лишнюю
    // ровно тогда, когда игрок отвернётся.
    invokeNative<void>(asMissionEntity_, vehicle, true, true);
    invokeNative<void>(lodDistance_, vehicle, kLodDistance);

    // Неуязвима намеренно. Прочность машины считает её водитель у себя: разбей
    // её кто-то здесь — и мы бы возили обломки под игрока, который едет целым.
    if (invincible_ != nullptr) {
        invokeNative<void>(invincible_, vehicle, true);
    }

    invokeNative<void>(engineOn_, vehicle, true, true, false);

    if (modelNoLongerNeeded_ != nullptr) {
        invokeNative<void>(modelNoLongerNeeded_, state.model);
    }

    spdlog::info("машина игрока {} показана номером {}", state.owner, vehicle);
    return vehicle;
}

void Vehicles::remove(int vehicle) const {
    if (vehicle == 0 || deleteVehicle_ == nullptr) {
        return;
    }

    // Ссылка на номер, а не сам номер: натив обнуляет его у вызывающего.
    int handle = vehicle;

    NativeContext context;
    context.push(&handle);
    deleteVehicle_(context.address());
}

void Vehicles::place(int vehicle, const shared::VehicleState& state) const {
    NativeContext coords;
    coords.push(vehicle);
    coords.push(true);
    getCoords_(coords.address());

    const shared::Vec3 actual{coords.result<float>(0), coords.result<float>(1),
                              coords.result<float>(2)};

    const float error = distanceBetween(actual, state.position);

    // Далеко разошлись — ставим рывком. Вблизи — подводим долей расхождения за
    // кадр: машина при этом остаётся физическим телом, её колёса крутятся, а
    // подвеска отрабатывает дорогу. Ставить её точно по снимку каждый кадр
    // означало бы отнять у неё физику и получить скользящую по земле коробку.
    const shared::Vec3 target =
        error > kSnapDistance
            ? state.position
            : shared::Vec3{std::lerp(actual.x, state.position.x, kCorrectionRate),
                           std::lerp(actual.y, state.position.y, kCorrectionRate),
                           std::lerp(actual.z, state.position.z, kCorrectionRate)};

    invokeNative<void>(setCoords_, vehicle, target.x, target.y, target.z, false, false, false);
    invokeNative<void>(setRotation_, vehicle, state.rotation.x, state.rotation.y, state.rotation.z,
                       kRotationOrder, true);

    // Скорость задаётся своя, а не выводится из перемещения: по ней игра
    // крутит колёса, наклоняет кузов и решает, реветь ли двигателю.
    invokeNative<void>(setVelocity_, vehicle, state.velocity.x, state.velocity.y,
                       state.velocity.z);
}

void Vehicles::sync(const std::vector<shared::VehicleState>& vehicles) {
    if (!ready()) {
        return;
    }

    for (auto it = puppets_.begin(); it != puppets_.end();) {
        const bool present = std::any_of(vehicles.begin(), vehicles.end(),
                                         [&it](const shared::VehicleState& state) {
                                             return state.owner == it->first;
                                         });

        if (present) {
            ++it;
            continue;
        }

        spdlog::info("игрок {} вышел из машины, номер {} убран", it->first, it->second.vehicle);
        remove(it->second.vehicle);
        it = puppets_.erase(it);
    }

    for (const shared::VehicleState& state : vehicles) {
        const auto known = puppets_.find(state.owner);

        if (known == puppets_.end()) {
            if (const int vehicle = spawn(state); vehicle != 0) {
                puppets_.emplace(state.owner, Puppet{.vehicle = vehicle, .model = state.model});
            }
            continue;
        }

        Puppet& puppet = known->second;

        // Машину могло не стать помимо нас, а хозяин мог пересесть в другую.
        // И то и другое означает одно: эта нам больше не годится.
        if (!invokeNative<bool>(doesExist_, puppet.vehicle) || puppet.model != state.model) {
            remove(puppet.vehicle);
            puppets_.erase(known);
            continue;
        }

        place(puppet.vehicle, state);
    }
}

int Vehicles::handleFor(shared::PlayerId owner) const {
    const auto it = puppets_.find(owner);
    return it == puppets_.end() ? 0 : it->second.vehicle;
}

void Vehicles::clear() {
    if (!ready()) {
        return;
    }

    for (const auto& [owner, puppet] : puppets_) {
        remove(puppet.vehicle);
    }

    puppets_.clear();
}

} // namespace oxymp::client::game
