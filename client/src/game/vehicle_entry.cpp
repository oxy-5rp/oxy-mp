#include "vehicle_entry.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

/// Свободных мест не нашлось вовсе.
///
/// Отдельным значением, а не kNoSeat из протокола: то означает «ни в какой
/// машине», а здесь речь о конкретной машине, в которой всё занято.
constexpr int kNoFreeSeat = -100;

/// Сколько отводится задаче входа, в миллисекундах.
constexpr int kEntryTimeout = 10000;

/// С какой скоростью персонаж идёт к машине. Двойка — бегом, как и у кукол.
constexpr float kRunToVehicle = 2.0F;

/// Признак обычного входа: подойти, открыть дверь, сесть.
constexpr int kNormalEntry = 1;

/// Больше мест, чем бывает у машины в GTA.
///
/// Предел обхода, а не число: сколько их на самом деле, спрашивается у игры.
/// Нужен он на случай, если игра ответит бессмыслицей: обход по её ответу без
/// потолка — это цикл на четыре миллиарда шагов внутри кадра.
constexpr int kMaxSeats = 16;

/// Сколько ждать, прежде чем пересаживать в ту же машину заново.
///
/// Полсекунды: столько идёт задача входа до первого шага, и раньше повторять
/// её значит не давать ей начаться. Дольше — и человек, у которого место
/// заняли прямо перед носом, простоит у двери заметно долго.
constexpr std::int32_t kRedirectCooldown = 500;

} // namespace

VehicleEntry::VehicleEntry(const NativeTable& table) noexcept
    : gettingIn_(table.handlerFor(natives::kIsPedGettingIntoAVehicle)),
      vehicleEntering_(table.handlerFor(natives::kGetVehiclePedIsTryingToEnter)),
      seatEntering_(table.handlerFor(natives::kGetSeatPedIsTryingToEnter)),
      seatFree_(table.handlerFor(natives::kIsVehicleSeatFree)),
      maxPassengers_(table.handlerFor(natives::kGetVehicleMaxNumberOfPassengers)),
      enterVehicle_(table.handlerFor(natives::kTaskEnterVehicle)),
      clearTasks_(table.handlerFor(natives::kClearPedTasks)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)) {}

bool VehicleEntry::ready() const noexcept {
    return gettingIn_ != nullptr && vehicleEntering_ != nullptr && seatEntering_ != nullptr &&
           seatFree_ != nullptr && enterVehicle_ != nullptr && clearTasks_ != nullptr;
}

int VehicleEntry::freeSeat(int vehicle, int wanted) const {
    const auto vacant = [this, vehicle](int seat) {
        return invokeNative<bool>(seatFree_, vehicle, seat);
    };

    // Сколько мест у машины, знает игра: у мотоцикла их два, у автобуса
    // полтора десятка, и гадать здесь нечем.
    const int passengers =
        maxPassengers_ != nullptr ? invokeNative<int>(maxPassengers_, vehicle) : 0;
    const int last = passengers > kMaxSeats ? kMaxSeats : passengers;

    // Пассажирские — по порядку, начиная с переднего. Порядок не случайный: у
    // игры нулевое место рядом с водителем, и человек, не попавший за руль,
    // ожидает оказаться именно там.
    for (int seat = 0; seat < last; ++seat) {
        if (seat != wanted && vacant(seat)) {
            return seat;
        }
    }

    // Водительское — последним и только если метили не в него. Пересаживать за
    // руль того, кто лез назад, значит распорядиться чужой машиной вместо него.
    if (wanted != shared::kDriverSeat && vacant(shared::kDriverSeat)) {
        return shared::kDriverSeat;
    }

    return kNoFreeSeat;
}

void VehicleEntry::sync(int ped) {
    if (!ready() || ped == 0) {
        return;
    }

    if (!invokeNative<bool>(gettingIn_, ped)) {
        // Посадка кончилась — удачей или отказом, нам всё равно. Память о
        // пересадке снимается здесь, а не по её удаче: иначе следующая попытка
        // сесть в ту же машину прошла бы мимо нас.
        redirectedVehicle_ = 0;
        return;
    }

    const int vehicle = invokeNative<int>(vehicleEntering_, ped);
    if (vehicle == 0) {
        return;
    }

    const int seat = invokeNative<int>(seatEntering_, ped);

    if (invokeNative<bool>(seatFree_, vehicle, seat)) {
        // Место свободно — пусть игра садится сама. Она сделает это лучше нас:
        // с обходом машины, открыванием двери и всем прочим.
        return;
    }

    const std::int32_t now =
        gameTimer_ != nullptr ? invokeNative<std::int32_t>(gameTimer_) : 0;

    // Уже пересаживали в эту машину и на это место: задача идёт, и выдавать её
    // заново нельзя. Заново выданная, она не даёт себе начаться, и персонаж
    // топчется у двери, пока игрок не отпустит клавишу.
    //
    // Срок нужен на случай, когда занято оказалось и то место, на которое мы
    // пересадили: без него пересадка не повторилась бы никогда.
    if (redirectedVehicle_ == vehicle && redirectedSeat_ == seat &&
        now - redirectedAt_ < kRedirectCooldown) {
        return;
    }

    const int chosen = freeSeat(vehicle, seat);

    if (chosen == kNoFreeSeat) {
        // Мест нет. Отменяем посадку: без этого игрок будет тянуть чужого
        // водителя за шиворот, у себя выкинет его на асфальт, а у него самого
        // не изменится ничего.
        invokeNative<void>(clearTasks_, ped);

        redirectedVehicle_ = vehicle;
        redirectedSeat_ = seat;
        redirectedAt_ = now;

        spdlog::debug("в машине {} нет свободных мест, посадка отменена", vehicle);
        return;
    }

    invokeNative<void>(clearTasks_, ped);
    invokeNative<void>(enterVehicle_, ped, vehicle, kEntryTimeout, chosen, kRunToVehicle,
                       kNormalEntry, 0);

    redirectedVehicle_ = vehicle;
    redirectedSeat_ = chosen;
    redirectedAt_ = now;

    spdlog::debug("место {} в машине {} занято, садимся на {}", seat, vehicle, chosen);
}

} // namespace oxymp::client::game
