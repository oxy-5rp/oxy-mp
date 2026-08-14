#include "vehicle_directory.hpp"

#include <algorithm>
#include <cmath>

namespace oxymp::server {
namespace {

/// Есть ли такой игрок среди подключённых.
[[nodiscard]] const VehicleDirectory::PlayerPlacement* findPlayer(
    std::span<const VehicleDirectory::PlayerPlacement> players, shared::PlayerId id) {
    if (id == shared::kInvalidPlayerId) {
        return nullptr;
    }

    const auto it = std::ranges::find(players, id, &VehicleDirectory::PlayerPlacement::id);
    return it == players.end() ? nullptr : &*it;
}

} // namespace

shared::VehicleId VehicleDirectory::add(std::uint32_t model, const shared::Vec3& position,
                                        float heading, shared::PlayerId owner, std::size_t limit) {
    if (model == 0 || vehicles_.size() >= limit) {
        return shared::kInvalidVehicleId;
    }

    const shared::VehicleId id = ++nextId_;

    shared::VehicleState state;
    state.id = id;
    state.model = model;
    state.position = position;

    // Из направления взгляда получается только поворот вокруг вертикали.
    // Остальные два остаются нулевыми, и это верно: машину ставят на землю
    // колёсами вниз, а не боком в воздухе.
    state.rotation = shared::Vec3{.x = 0.0F, .y = 0.0F, .z = heading};

    Vehicle vehicle;
    vehicle.state = state;

    // Ведущим сразу назначается тот, кто просил. Он стоит рядом — иначе не
    // просил бы, — и ждать общего пересмотра незачем: до него машина простояла
    // бы неподвижной у него на глазах.
    vehicle.owner = owner;
    vehicle.stateAt = Clock::now();

    vehicles_.emplace(id, std::move(vehicle));
    return id;
}

bool VehicleDirectory::remove(shared::VehicleId id) {
    if (vehicles_.erase(id) == 0) {
        return false;
    }

    // Сидевшие в ней больше в ней не сидят. Без этой уборки за игроком осталось
    // бы место в машине, которой нет, и он навсегда получил бы преимущество
    // водителя над машиной-призраком.
    for (auto it = seats_.begin(); it != seats_.end();) {
        it = it->second.vehicle == id ? seats_.erase(it) : std::next(it);
    }

    return true;
}

bool VehicleDirectory::isSeatedIn(shared::PlayerId player, shared::VehicleId vehicle) const {
    const auto it = seats_.find(player);
    return it != seats_.end() && it->second.vehicle == vehicle &&
           vehicle != shared::kInvalidVehicleId;
}

VehicleDirectory::Seat VehicleDirectory::seatOf(shared::PlayerId player) const {
    const auto it = seats_.find(player);
    return it == seats_.end() ? Seat{} : it->second;
}

const VehicleDirectory::Vehicle* VehicleDirectory::find(shared::VehicleId id) const {
    const auto it = vehicles_.find(id);
    return it == vehicles_.end() ? nullptr : &it->second;
}

bool VehicleDirectory::applyState(shared::PlayerId sender, const shared::VehicleState& state) {
    const auto it = vehicles_.find(state.id);
    if (it == vehicles_.end() || it->second.owner != sender) {
        return false;
    }

    const std::uint32_t model = it->second.state.model;

    it->second.state = state;
    it->second.stateAt = Clock::now();

    // Модель берётся не из снимка, а из записи. Машина заводится один раз и с
    // одной моделью; позволить снимку менять её значило бы позволить ведущему
    // превратить чужую машину во что угодно — а ведущим побывает каждый.
    it->second.state.model = model;

    return true;
}

bool VehicleDirectory::applyAppearance(shared::PlayerId sender,
                                       const shared::VehicleAppearance& appearance) {
    const auto it = vehicles_.find(appearance.id);
    if (it == vehicles_.end() || it->second.owner != sender) {
        return false;
    }

    it->second.appearance = appearance;
    return true;
}

bool VehicleDirectory::setSeat(shared::PlayerId player, shared::VehicleId vehicle,
                               std::int8_t seat) {
    if (vehicle == shared::kInvalidVehicleId) {
        return seats_.erase(player) != 0;
    }

    const Seat fresh{.vehicle = vehicle, .index = seat};

    const auto known = seats_.find(player);
    if (known != seats_.end() && known->second.vehicle == fresh.vehicle &&
        known->second.index == fresh.index) {
        return false;
    }

    seats_[player] = fresh;
    return true;
}

void VehicleDirectory::forgetPlayer(shared::PlayerId player) {
    seats_.erase(player);

    // Машины ушедшего остаются без ведущего и замирают там, где он их оставил.
    // Нового назначит ближайший пересмотр — если рядом есть кому.
    for (auto& [id, vehicle] : vehicles_) {
        if (vehicle.owner == player) {
            vehicle.owner = shared::kInvalidPlayerId;
        }
    }
}

shared::PlayerId VehicleDirectory::driverOf(shared::VehicleId vehicle) const {
    const auto it = std::ranges::find_if(seats_, [vehicle](const auto& entry) {
        return entry.second.vehicle == vehicle && entry.second.index == shared::kDriverSeat;
    });

    return it == seats_.end() ? shared::kInvalidPlayerId : it->first;
}

shared::PlayerId VehicleDirectory::chooseOwner(const Vehicle& vehicle,
                                               std::span<const PlayerPlacement> players) const {
    // Первое правило и самое сильное: машину ведёт тот, кто за рулём. Отдать её
    // кому-то другому нельзя ни при каком расстоянии — ехать будет один, а
    // считать поездку другой, и разъедутся они мгновенно.
    const shared::PlayerId driver = driverOf(vehicle.state.id);
    if (findPlayer(players, driver) != nullptr) {
        return driver;
    }

    constexpr float kRangeSquared = kOwnershipRange * kOwnershipRange;

    const PlayerPlacement* nearest = nullptr;
    float nearestSquared = kRangeSquared;

    for (const PlayerPlacement& player : players) {
        const float squared = shared::distanceSquared(player.position, vehicle.state.position);

        if (squared <= nearestSquared) {
            nearest = &player;
            nearestSquared = squared;
        }
    }

    // За рулём никого. Машину держит тот, кто её уже ведёт, пока он в пределах
    // досягаемости и пока никто не оказался заметно ближе.
    if (const PlayerPlacement* current = findPlayer(players, vehicle.owner); current != nullptr) {
        const float currentSquared =
            shared::distanceSquared(current->position, vehicle.state.position);

        if (currentSquared <= kRangeSquared) {
            if (nearest == nullptr || nearest->id == current->id) {
                return current->id;
            }

            // Здесь без корня не обойтись: сравнивается не расстояние с
            // расстоянием, а разница между ними с запасом. Считается это
            // несколько раз в секунду, и дешевизна тут ничего не решает.
            const float currentDistance = std::sqrt(currentSquared);
            const float nearestDistance = std::sqrt(nearestSquared);

            if (currentDistance - nearestDistance <= kHandoverMargin) {
                return current->id;
            }
        }
    }

    return nearest == nullptr ? shared::kInvalidPlayerId : nearest->id;
}

std::vector<VehicleDirectory::OwnerChange> VehicleDirectory::reassign(
    std::span<const PlayerPlacement> players) {
    std::vector<OwnerChange> changes;

    for (auto& [id, vehicle] : vehicles_) {
        const shared::PlayerId owner = chooseOwner(vehicle, players);

        if (owner == vehicle.owner) {
            continue;
        }

        vehicle.owner = owner;
        changes.push_back(OwnerChange{.id = id, .owner = owner});
    }

    return changes;
}

} // namespace oxymp::server
