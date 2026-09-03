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

bool VehicleDirectory::place(shared::VehicleId id, const shared::Vec3& position,
                             float heading) {
    const auto found = vehicles_.find(id);
    if (found == vehicles_.end()) {
        return false;
    }

    shared::VehicleState& state = found->second.state;

    state.position = position;

    // Только по вертикальной оси: ставить машину набок распоряжением незачем, а
    // перевернувшуюся поднимет физика у ведущего.
    state.rotation = shared::Vec3{.x = 0.0F, .y = 0.0F, .z = heading};

    // Скорость обнуляется вместе с местом, и это не мелочь: переставленная на
    // ходу машина, сохранив её, поехала бы на новом месте сама.
    state.velocity = shared::Vec3{};
    state.angularVelocity = shared::Vec3{};

    // Состояние изменилось — значит его надо разослать. Без этой отметки
    // переставленная машина доезжала только до вошедших позже: тем, кто уже
    // на неё смотрит, рассылка её не показывала, потому что решает она по
    // этой самой отметке.
    //
    // Ведущего это не касается — ему уходит отдельная просьба, и он пришлёт
    // снимок сам. А вот у брошенной машины ведущего нет вовсе, и прислать
    // снимок о ней некому: она так и оставалась стоять на прежнем месте у
    // всех, кто её видел.
    found->second.stateAt = Clock::now();

    return true;
}

bool VehicleDirectory::repair(shared::VehicleId id) {
    const auto found = vehicles_.find(id);
    if (found == vehicles_.end()) {
        return false;
    }

    shared::VehicleState& state = found->second.state;

    state.bodyHealth = shared::kFullVehicleHealth;
    state.engineHealth = shared::kFullVehicleHealth;
    state.tankHealth = shared::kFullVehicleHealth;

    state.doorLevels = 0;
    state.doorsBroken = 0;
    state.windowsBroken = 0;
    state.tyresBurst = 0;

    // И признак разрушения — вместе с прочностями. Без него починка выходила
    // наполовину: прочности целые, а машина всё ещё числится разбитой. Вошедший
    // позже получал её такой и взрывал у себя сразу, а тем, кто уже смотрел, она
    // так и оставалась обгорелым остовом — переход из разбитой в целую, по
    // которому они её чинят, не случался никогда.
    state.flags = shared::without(state.flags, shared::VehicleFlag::Destroyed);

    // Как и у place: прочности изменились, и рассылка узнаёт об этом только по
    // отметке. Брошенную машину чинить некому — снимка о ней не пришлёт никто.
    found->second.stateAt = Clock::now();

    return true;
}

bool VehicleDirectory::setLockState(shared::VehicleId id, std::uint8_t lockState) {
    const auto found = vehicles_.find(id);
    if (found == vehicles_.end()) {
        return false;
    }

    found->second.lockState = lockState;
    return true;
}

bool VehicleDirectory::setWindows(shared::VehicleId id, std::uint8_t windowsOpen) {
    const auto found = vehicles_.find(id);
    if (found == vehicles_.end()) {
        return false;
    }

    found->second.windowsOpen = windowsOpen;
    found->second.windowsTold = true;
    return true;
}

bool VehicleDirectory::setDoorLevel(shared::VehicleId id, int door, std::uint32_t level) {
    const auto found = vehicles_.find(id);
    if (found == vehicles_.end() || door < 0 || door >= shared::kVehicleDoorCount) {
        return false;
    }

    shared::VehicleState& state = found->second.state;
    state.doorLevels = shared::withDoorLevel(state.doorLevels, door, level);

    // Состояние изменилось — значит его надо разослать. Без отметки открытая
    // дверь доезжала бы только до вошедших позже: тем, кто уже смотрит, рассылка
    // её не показала бы, потому что решает она по этой самой отметке.
    found->second.stateAt = Clock::now();

    return true;
}

bool VehicleDirectory::setDimension(shared::VehicleId id, std::int32_t dimension) {
    const auto found = vehicles_.find(id);
    if (found == vehicles_.end()) {
        return false;
    }

    found->second.dimension = dimension;
    return true;
}

bool VehicleDirectory::pinOwner(shared::VehicleId id, shared::PlayerId owner, bool sticky) {
    const auto found = vehicles_.find(id);
    if (found == vehicles_.end()) {
        return false;
    }

    found->second.pinned = owner;
    found->second.pinnedSticky = sticky;
    return true;
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

std::vector<std::pair<std::int8_t, shared::PlayerId>> VehicleDirectory::seatedIn(
    shared::VehicleId vehicle) const {
    std::vector<std::pair<std::int8_t, shared::PlayerId>> sitting;

    if (vehicle == shared::kInvalidVehicleId) {
        return sitting;
    }

    for (const auto& [player, seat] : seats_) {
        if (seat.vehicle == vehicle) {
            sitting.emplace_back(seat.index, player);
        }
    }

    // Порядок у хеш-таблицы свой, и наружу он уходил бы разным от запуска к
    // запуску. Скрипту это встало бы боком не сразу, а в тот день, когда он
    // возьмёт «первого пассажира»: сегодня им окажется один, завтра другой.
    std::ranges::sort(sitting);
    return sitting;
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

    // Изменилось ли что-нибудь. Сравнивается до подмены, потому что после неё
    // сравнивать будет не с чем.
    //
    // Стоящая машина шлёт неизменный снимок раз в четверть секунды — реже
    // нельзя, иначе её ведущий сойдёт за пропавшего. Пересылать такой снимок
    // тем, кто машину видит, незачем: у них она и так стоит там же. Отметка
    // поэтому обновляется только по изменению, а по ней рассылка и решает, кого
    // пересылать в этот такт.
    const bool changed = shared::differs(it->second.state, state);

    it->second.state = state;

    if (changed) {
        it->second.stateAt = Clock::now();
    }

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

    // Внешность, назначенная сервером, объявлениями ведущего не перебивается.
    // Порядок здесь такой: сервер назначил тюнинг и разослал его, а ведущий тем
    // же тактом прислал снятое со своей игры — то есть ещё без тюнинга. Прими мы
    // это объявление, назначенное пропало бы, не успев доехать, и выглядело бы
    // это как «setMod не работает через раз».
    if (it->second.appearanceFromServer) {
        return false;
    }

    it->second.appearance = appearance;
    return true;
}

bool VehicleDirectory::setAppearance(shared::VehicleId id,
                                     const shared::VehicleAppearance& appearance) {
    const auto it = vehicles_.find(id);
    if (it == vehicles_.end()) {
        return false;
    }

    it->second.appearance = appearance;

    // Номер берётся у машины, а не у описания: описание пришло из скрипта, и
    // поверить ему на слово значило бы позволить перекрасить соседнюю машину.
    it->second.appearance->id = id;
    it->second.appearanceFromServer = true;

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
    //
    // Назначение скриптом снимается тоже, а не только владение, и это не
    // симметрично оставленному владению нарочно. Номер ушедшего — наименьший
    // свободный, и его выдадут следующему вошедшему (PlayerRegistry::freeId).
    // Не сними мы pinned здесь, ближайший пересмотр отдал бы машину чужому
    // человеку, которому просто достался старый номер, — назначение выбирается
    // раньше расстояния и без проверки, тот ли это игрок, которому его давали.
    for (auto& [id, vehicle] : vehicles_) {
        if (vehicle.owner == player) {
            vehicle.owner = shared::kInvalidPlayerId;
        }

        if (vehicle.pinned == player) {
            vehicle.pinned = shared::kInvalidPlayerId;
            vehicle.pinnedSticky = false;
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

    // Назначенный скриптом идёт сразу после водителя и раньше расстояния: ради
    // этого назначение и заводят. Дальности он не подчиняется — назначивший
    // знает, чего хочет, а мы не знаем, зачем.
    if (vehicle.pinned != shared::kInvalidPlayerId &&
        findPlayer(players, vehicle.pinned) != nullptr) {
        return vehicle.pinned;
    }

    constexpr float kRangeSquared = kOwnershipRange * kOwnershipRange;

    const PlayerPlacement* nearest = nullptr;
    float nearestSquared = kRangeSquared;

    for (const PlayerPlacement& player : players) {
        // Одна точка карты бывает занята много раз (см. script/dimension.hpp):
        // игрок вплотную к машине, но в другом измерении, её не видит и не
        // пришлёт по ней ни одного снимка. Назначить его ведущим значило бы
        // заморозить машину для всех, кто её действительно видит, — а
        // ближайший пересмотр не спохватился бы, сочтя ведущего уже назначенным.
        if (!script::dimensionsMeet(player.dimension, vehicle.dimension)) {
            continue;
        }

        const float squared = shared::distanceSquared(player.position, vehicle.state.position);

        if (squared <= nearestSquared) {
            nearest = &player;
            nearestSquared = squared;
        }
    }

    // За рулём никого. Машину держит тот, кто её уже ведёт, пока он в пределах
    // досягаемости, в том же измерении и пока никто не оказался заметно ближе.
    if (const PlayerPlacement* current = findPlayer(players, vehicle.owner); current != nullptr) {
        const float currentSquared =
            shared::distanceSquared(current->position, vehicle.state.position);

        if (currentSquared <= kRangeSquared &&
            script::dimensionsMeet(current->dimension, vehicle.dimension)) {
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

        // Неудерживаемое назначение — одноразовое: оно сказало, кому вести
        // машину сейчас, и на этом кончилось. Снимается после выбора, а не до:
        // сниматься ему положено, только когда оно уже сработало.
        if (vehicle.pinned != shared::kInvalidPlayerId && !vehicle.pinnedSticky) {
            vehicle.pinned = shared::kInvalidPlayerId;
        }

        if (owner == vehicle.owner) {
            continue;
        }

        changes.push_back(OwnerChange{.id = id, .owner = owner, .was = vehicle.owner});
        vehicle.owner = owner;
    }

    return changes;
}

} // namespace oxymp::server
