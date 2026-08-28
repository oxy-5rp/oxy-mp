#include "vehicles.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

#include <algorithm>

namespace oxymp::client::game {
namespace {

/// Сколько ждать модель, прежде чем счесть, что её не будет.
///
/// Пять секунд: за это время грузится с диска что угодно, включая модель на
/// десяток мегабайт, а ждать дольше значит держать человека в неведении.
constexpr auto kModelPatience = std::chrono::seconds{5};

/// Сколько пассажирских мест перебирается в поисках персонажа, если спросить у
/// самой машины не удалось.
///
/// Восемь с запасом: у самой вместительной машины игры мест меньше, а лишний
/// пустой опрос не стоит ничего.
constexpr int kMaxPassengerSeats = 8;

/// Дальность, с которой машина остаётся видимой, в метрах.
constexpr int kLodDistance = 1000;

} // namespace

Vehicles::Vehicles(const NativeTable& table) noexcept
    : snapshot_(table),
      requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      modelNoLongerNeeded_(table.handlerFor(natives::kSetModelAsNoLongerNeeded)),
      createVehicle_(table.handlerFor(natives::kCreateVehicle)),
      deleteVehicle_(table.handlerFor(natives::kDeleteVehicle)),
      doesExist_(table.handlerFor(natives::kDoesEntityExist)),
      asMissionEntity_(table.handlerFor(natives::kSetEntityAsMissionEntity)),
      lodDistance_(table.handlerFor(natives::kSetEntityLodDist)),
      selectedWeapon_(table.handlerFor(natives::kGetSelectedPedWeapon)),
      freezePosition_(table.handlerFor(natives::kFreezeEntityPosition)),
      onGroundProperly_(table.handlerFor(natives::kSetVehicleOnGroundProperly)),
      engineOn_(table.handlerFor(natives::kSetVehicleEngineOn)),
      isInAnyVehicle_(table.handlerFor(natives::kIsPedInAnyVehicle)),
      vehiclePedIsIn_(table.handlerFor(natives::kGetVehiclePedIsIn)),
      pedInSeat_(table.handlerFor(natives::kGetPedInVehicleSeat)),
      maxPassengers_(table.handlerFor(natives::kGetVehicleMaxNumberOfPassengers)),
      gameTimer_(table.handlerFor(natives::kGetGameTimer)),
      setCoords_(table.handlerFor(natives::kSetEntityCoords)),
      setHeading_(table.handlerFor(natives::kSetEntityHeading)),
      setVelocity_(table.handlerFor(natives::kSetEntityVelocity)),
      fix_(table.handlerFor(natives::kSetVehicleFixed)),
      fixDeformation_(table.handlerFor(natives::kSetVehicleDeformationFixed)),
      intoVehicle_(table.handlerFor(natives::kSetPedIntoVehicle)) {}

Vehicles::~Vehicles() {
    // Машины здесь уже не убрать: разрушение приходится на выгрузку модуля, а
    // она случается вне скриптового тика, где нативы звать нельзя. Убирать их
    // положено вызовом clear до этого момента.
}

bool Vehicles::ready() const noexcept {
    return snapshot_.ready() && requestModel_ != nullptr && hasModelLoaded_ != nullptr &&
           createVehicle_ != nullptr && deleteVehicle_ != nullptr && doesExist_ != nullptr &&
           asMissionEntity_ != nullptr && lodDistance_ != nullptr && isInAnyVehicle_ != nullptr &&
           vehiclePedIsIn_ != nullptr && pedInSeat_ != nullptr;
}

std::optional<Vehicles::Seat> Vehicles::seatOf(int ped) const {
    if (!ready() || ped == 0) {
        return std::nullopt;
    }

    if (!invokeNative<bool>(isInAnyVehicle_, ped, false)) {
        return std::nullopt;
    }

    // Последний довод: не считать машиной ту, в которую персонаж только
    // залезает. Пока он висит на подножке, он в ней ещё не сидит.
    const int vehicle = invokeNative<int>(vehiclePedIsIn_, ped, false);
    if (vehicle == 0) {
        return std::nullopt;
    }

    Seat seat;
    seat.vehicle = vehicle;

    if (invokeNative<int>(pedInSeat_, vehicle, shared::kDriverSeat) == ped) {
        seat.index = shared::kDriverSeat;
        return seat;
    }

    // Сколько у этой модели мест, знает сама игра. Спрашиваем её, а не перебираем
    // вслепую: у мотоцикла мест два, и опрашивать у него восьмое незачем.
    const int seats = maxPassengers_ != nullptr
                          ? invokeNative<int>(maxPassengers_, vehicle)
                          : kMaxPassengerSeats;

    for (int index = 0; index < std::max(seats, 1); ++index) {
        if (invokeNative<int>(pedInSeat_, vehicle, index) == ped) {
            seat.index = static_cast<std::int8_t>(index);
            return seat;
        }
    }

    // Сидит, но места не нашлось: такое бывает у мест, которых у этой модели нет
    // в обычной нумерации. Считаем пассажиром на первом месте — это лучше, чем
    // объявить идущим пешком того, кто едет.
    //
    // Но это догадка, и она уходит на сервер наравне с правдой: остальные усадят
    // нашего персонажа туда, куда мы им скажем. Поэтому догадка не молчит — с
    // неё начинается разбор всякой жалобы вида «у меня я за рулём, а у него я
    // пассажир».
    spdlog::debug("seat of ped {} in vehicle {} is not in the usual numbering, calling it 0", ped,
                  vehicle);

    seat.index = 0;
    return seat;
}

shared::VehicleId Vehicles::idOf(int vehicle) const {
    if (vehicle == 0) {
        return shared::kInvalidVehicleId;
    }

    const auto it = std::ranges::find_if(vehicles_, [vehicle](const auto& entry) {
        return entry.second.vehicle == vehicle;
    });

    return it == vehicles_.end() ? shared::kInvalidVehicleId : it->first;
}

int Vehicles::handleFor(shared::VehicleId id) const {
    const auto it = vehicles_.find(id);
    return it == vehicles_.end() ? 0 : it->second.vehicle;
}

std::vector<shared::VehicleAppearance> Vehicles::describeOwnedAppearances(int localPed,
                                                                         bool resample) {
    /// Сколько машин разрешено снять за один кадр.
    constexpr std::size_t kPerFrame = 2;

    std::vector<shared::VehicleAppearance> appearances;

    if (!ready()) {
        return appearances;
    }

    const auto seat = seatOf(localPed);
    const int driven = seat && seat->index == shared::kDriverSeat ? seat->vehicle : 0;

    for (auto& [id, entry] : vehicles_) {
        if (!entry.ours) {
            continue;
        }

        // Машина под нами — единственная, чью внешность мог изменить игрок.
        const bool ours = entry.vehicle == driven;

        if (entry.described && !(ours && resample)) {
            continue;
        }

        shared::VehicleAppearance appearance = snapshot_.readAppearance(entry.vehicle);
        appearance.id = id;

        appearances.push_back(appearance);
        entry.described = true;

        if (appearances.size() >= kPerFrame) {
            break;
        }
    }

    return appearances;
}

int Vehicles::spawn(const shared::VehicleState& state) {
    if (state.model == 0) {
        return 0;
    }

    invokeNative<void>(requestModel_, state.model);

    if (!invokeNative<bool>(hasModelLoaded_, state.model)) {
        // Молчать здесь нельзя, и это стоило вечера. Модель, которой у игры
        // нет, ведёт себя ровно так же, как модель, которая ещё грузится:
        // машина просто не появляется, и ни одной строки о том, почему. Хозяин
        // сервера при этом видит «машина заведена» в своём журнале и ищет
        // причину где угодно, только не здесь.
        //
        // Отсюда терпение и жалоба: модель, не пришедшая за отведённое время,
        // не придёт уже никогда — либо её нет в игре, либо ресурс, который её
        // приносит, не поднялся.
        const auto now = std::chrono::steady_clock::now();
        const auto [entry, added] = requestedAt_.try_emplace(state.model, now);

        if (!added && now - entry->second >= kModelPatience &&
            !complained_.contains(state.model)) {
            complained_.insert(state.model);

            spdlog::warn("vehicle model {:#010x} never loaded: the game does not have it, "
                         "or the resource that brings it did not start",
                         state.model);
        }

        return 0;
    }

    // Пришедшая модель забывается: заказана она может быть снова, и второй
    // отсчёт должен начаться с чистого места.
    requestedAt_.erase(state.model);

    // Последние признаки: машина не сетевая и не принадлежит скрипту. Сетевых в
    // одиночной игре не бывает, а принадлежность скрипту передаётся отдельно,
    // ниже. Третий передаётся явно, чтобы не полагаться на то, что игра прочитает
    // ноль из необъявленной ячейки нашего же контекста.
    const int vehicle =
        invokeNative<int>(createVehicle_, state.model, state.position.x, state.position.y,
                          state.position.z, state.rotation.z, false, false, false);
    if (vehicle == 0) {
        return 0;
    }

    // Машина принадлежит нам, а не миру: иначе игра вправе убрать её как лишнюю
    // ровно тогда, когда игрок отвернётся.
    invokeNative<void>(asMissionEntity_, vehicle, true, true);
    invokeNative<void>(lodDistance_, vehicle, kLodDistance);

    if (modelNoLongerNeeded_ != nullptr) {
        invokeNative<void>(modelNoLongerNeeded_, state.model);
    }

    spdlog::debug("машина {} показана номером {}", state.id, vehicle);
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

void Vehicles::answerTo(Entry& entry, shared::PlayerId owner, bool ours, bool towed) const {
    const bool ownerChanged = !entry.answered || entry.owner != owner || entry.ours != ours;

    entry.owner = owner;
    entry.ours = ours;
    entry.answered = true;

    // Ничья машина замирает там, где её оставили. Считать её физику некому, а
    // отпущенная без присмотра она сползёт по уклону или провалится сквозь
    // землю, когда игра подгрузит мир под ней заново.
    //
    // Прицепленная — исключение, и без него сцепка не работает вовсе: в прицепе
    // никто не сидит, ведущего у него нет, и замерший на месте он остался бы
    // стоять посреди дороги, пока тягач уезжает.
    const bool shouldFreeze = owner == shared::kInvalidPlayerId && !towed;

    if (freezePosition_ != nullptr && entry.frozen != shouldFreeze) {
        // На колёса — перед тем как замереть, и только тогда. Машину, которую
        // кто-то ведёт, поправит её ведущий; ничью не поправит никто, и
        // застывшая в воздухе или наполовину в асфальте она такой и останется.
        if (shouldFreeze && onGroundProperly_ != nullptr) {
            invokeNative<void>(onGroundProperly_, entry.vehicle, 5.0F);
        }

        invokeNative<void>(freezePosition_, entry.vehicle, shouldFreeze);
        entry.frozen = shouldFreeze;
    }

    if (!ownerChanged) {
        return;
    }

    // Внешность придётся объявить заново, если машина стала нашей: пока её вёл
    // другой, он мог её перекрасить, и наше прошлое объявление её больше не
    // описывает.
    entry.described = false;

    // Неуязвимость зависит от того, чья машина, и в этом вся суть. Свою мы ведём
    // по-настоящему: она мнётся, ломается и горит, и о случившемся рассказываем
    // мы. Чужую трогать нельзя — уязвимая, она взорвалась бы здесь от одного
    // выстрела и осталась бы целой у того, кто в ней едет.
    //
    // Что при этом теряется, стоит знать: вмятины. Их форма нативами не
    // читается и не задаётся вовсе. Выбитые стёкла, оторванные двери, пробитые
    // колёса и прочности передаются — они задаются явно.
    snapshot_.protect(entry.vehicle, !ours);

    // Зажигание: заглушить машину, оставшуюся без ведущего. У ведомой им
    // распоряжаются снимки, а ничью снимками не поправить — их больше не будет.
    if (engineOn_ != nullptr && owner == shared::kInvalidPlayerId) {
        invokeNative<void>(engineOn_, entry.vehicle, false, true, false);
    }
}

void Vehicles::dress(shared::VehicleId id, Entry& entry) {
    const auto known = appearances_.find(id);

    if (known == appearances_.end() || entry.dressed) {
        return;
    }

    snapshot_.applyAppearance(entry.vehicle, known->second);
    entry.dressed = true;
}

bool Vehicles::place(shared::VehicleId id, const shared::Vec3& position, float heading) {
    const int vehicle = handleFor(id);
    if (vehicle == 0 || setCoords_ == nullptr) {
        return false;
    }

    // Скорость гасится раньше места, а не после: машина, переставленная на
    // ходу, приехала бы на новом месте туда, куда ехала на старом.
    if (setVelocity_ != nullptr) {
        invokeNative<void>(setVelocity_, vehicle, 0.0F, 0.0F, 0.0F);
    }

    // Последние признаки — те же, что игра ставит сама: не сбивать прохожих, не
    // трогать чужие сущности, поставить на землю. Передаются явно, чтобы не
    // полагаться на то, что игра прочтёт ноль из необъявленной ячейки нашего же
    // контекста.
    invokeNative<void>(setCoords_, vehicle, position.x, position.y, position.z, false, false,
                       false, true);

    if (setHeading_ != nullptr) {
        invokeNative<void>(setHeading_, vehicle, heading);
    }

    if (onGroundProperly_ != nullptr) {
        invokeNative<bool>(onGroundProperly_, vehicle);
    }

    spdlog::debug("машина {} переставлена в {:.1f} {:.1f} {:.1f}", id, position.x, position.y,
                  position.z);
    return true;
}

bool Vehicles::seat(int ped, shared::VehicleId id, std::int8_t place) {
    const int vehicle = handleFor(id);
    if (ped == 0 || vehicle == 0 || intoVehicle_ == nullptr) {
        return false;
    }

    invokeNative<void>(intoVehicle_, ped, vehicle, static_cast<int>(place));

    spdlog::debug("персонаж посажен в машину {} на место {}", id, static_cast<int>(place));
    return true;
}

bool Vehicles::repair(shared::VehicleId id) {
    const int vehicle = handleFor(id);
    if (vehicle == 0 || fix_ == nullptr) {
        return false;
    }

    invokeNative<void>(fix_, vehicle);

    // Вмятины — отдельным вызовом, и без него починка выглядит наполовину: сам
    // fix возвращает прочности и стёкла, но смятое крыло оставляет как было.
    if (fixDeformation_ != nullptr) {
        invokeNative<void>(fixDeformation_, vehicle);
    }

    spdlog::debug("машина {} починена", id);
    return true;
}

void Vehicles::applyAppearance(const shared::VehicleAppearance& appearance) {
    if (appearance.id == shared::kInvalidVehicleId) {
        return;
    }

    appearances_[appearance.id] = appearance;

    const auto known = vehicles_.find(appearance.id);
    if (known == vehicles_.end()) {
        // Машины ещё нет — оденем, когда появится.
        return;
    }

    // Внешность сменилась у уже показанной машины: накладываем заново.
    known->second.dressed = false;
    dress(appearance.id, known->second);
}

bool Vehicles::occupied(int vehicle) const {
    if (vehicle == 0 || pedInSeat_ == nullptr) {
        return false;
    }

    if (invokeNative<int>(pedInSeat_, vehicle, shared::kDriverSeat) != 0) {
        return true;
    }

    const int seats = maxPassengers_ != nullptr ? invokeNative<int>(maxPassengers_, vehicle)
                                                : kMaxPassengerSeats;

    for (int index = 0; index < std::max(seats, 1); ++index) {
        if (invokeNative<int>(pedInSeat_, vehicle, index) != 0) {
            return true;
        }
    }

    return false;
}

void Vehicles::sweep() {
    if (!ready()) {
        return;
    }

    for (auto it = vehicles_.begin(); it != vehicles_.end();) {
        Entry& entry = it->second;

        if (!entry.doomed) {
            ++it;
            continue;
        }

        // В машине ещё кто-то сидит. Такое бывает: чужой персонаж выходит из неё
        // не мгновенно, а его хозяин мог и вовсе замолчать, не успев сказать, что
        // вышел. Оставляем машину до следующего кадра — она уже помечена, и
        // никуда от нас не денется.
        //
        // Удалить её сейчас значило бы оставить игре персонажа, сидящего в
        // несуществующей машине. Игра при этом не падает на месте — она падает
        // позже, на отрисовке, и след ведёт в d3d11, а не сюда.
        if (occupied(entry.vehicle)) {
            ++it;
            continue;
        }

        spdlog::debug("машины {} больше не видно, номер {} убран", it->first, entry.vehicle);

        remove(entry.vehicle);
        appearances_.erase(it->first);
        it = vehicles_.erase(it);
    }
}

std::uint32_t Vehicles::weaponInHand(int localPed) const {
    if (selectedWeapon_ == nullptr || localPed == 0) {
        return 0;
    }

    return invokeNative<std::uint32_t>(selectedWeapon_, localPed);
}

void Vehicles::harm(shared::VehicleId id, const shared::VehicleHarm& harm) const {
    const auto known = vehicles_.find(id);

    if (known == vehicles_.end() || !known->second.ours) {
        // Машина не наша либо её у нас нет вовсе. Отнимать прочность не у чего:
        // у чужой она принадлежит её ведущему, и он же о ней расскажет.
        return;
    }

    snapshot_.takeHealth(known->second.vehicle, harm);
}

void Vehicles::sync(const std::vector<View>& vehicles, shared::PlayerId self, int localPed) {
    if (!ready()) {
        return;
    }

    // Длительность кадра — один раз на все машины: часы у игры одни, а машин
    // бывает десяток.
    const auto now = gameTimer_ != nullptr ? invokeNative<std::int32_t>(gameTimer_) : 0;

    // Первый кадр сравнивать не с чем, и отрицательная длительность после
    // переполнения счётчика — тоже не длительность. И то и другое означает
    // «времени не прошло»: расхождение просто подождёт следующего кадра.
    const float seconds =
        framedAt_ != 0 && now > framedAt_ ? static_cast<float>(now - framedAt_) / 1000.0F : 0.0F;

    framedAt_ = now;

    // Все машины считаются ушедшими, пока список не скажет обратного. Пометка
    // снимается ниже с каждой, что в нём нашлась, а оставшиеся помеченными
    // уберёт sweep — после того, как расставят людей.
    for (auto& [id, entry] : vehicles_) {
        entry.doomed = true;
    }

    // Кто сейчас чей прицеп — отдельным проходом, до всего остального.
    // Прицепленную машину ведёт тягач, а не снимки: ей нельзя ни задавать
    // положение, ни замирать на месте. А узнать об этом можно только из снимка
    // тягача, который в списке может стоять и после неё.
    towed_.clear();

    for (const View& view : vehicles) {
        if (view.state.trailer != shared::kInvalidVehicleId) {
            towed_.insert(view.state.trailer);
        }
    }

    for (const View& view : vehicles) {
        const shared::VehicleState& state = view.state;

        if (state.id == shared::kInvalidVehicleId) {
            continue;
        }

        const bool ours = view.owner != shared::kInvalidPlayerId && view.owner == self;

        auto known = vehicles_.find(state.id);

        if (known == vehicles_.end()) {
            const int vehicle = spawn(state);
            if (vehicle == 0) {
                // Модель ещё грузится. Заведём в одном из следующих кадров:
                // сервер о машине не забудет, и список придёт снова.
                continue;
            }

            // Прошлое состояние остаётся заводским, а не приравнивается к
            // пришедшему. Только что заведённая машина именно такова: целая, с
            // закрытыми дверями и целыми стёклами. Приравняй мы его — и
            // повреждения, с которыми машина пришла, не наложились бы никогда:
            // разницы с прошлым разом у них не оказалось бы.
            known = vehicles_.emplace(state.id, Entry{.vehicle = vehicle, .model = state.model})
                        .first;

            answerTo(known->second, view.owner, ours, towed_.contains(state.id));
            dress(state.id, known->second);
            continue;
        }

        Entry& entry = known->second;
        entry.doomed = false;

        // Машины могло не стать помимо нас — например, её убрал сам движок при
        // переполнении своего предела. Заведём заново в следующем кадре.
        if (!invokeNative<bool>(doesExist_, entry.vehicle)) {
            vehicles_.erase(known);
            continue;
        }

        const bool towed = towed_.contains(state.id);

        answerTo(entry, view.owner, ours, towed);
        dress(state.id, entry);

        // Свою машину не трогаем. Её ведёт игра, а мы лишь снимаем с неё снимки:
        // наложить на неё пришедшее состояние значило бы бороться с собственной
        // физикой — и проиграть ей, потому что физика считается каждый кадр, а
        // снимки приходят каждый такт сервера.
        if (ours) {
            entry.applied = state;
            continue;
        }

        // Прицепленную машину ведёт тягач. Задавать ей положение снимками
        // значило бы тянуть её в две стороны: игра держит её на сцепке, а мы
        // ставили бы её туда, где её видел хозяин, — и прицеп бился бы о тягач
        // тридцать раз в секунду.
        if (!towed) {
            snapshot_.applyMotion(entry.vehicle, state, seconds);
        }

        // Прочность — здесь, а не в applyControls, и сверяется она с тем, что у
        // машины есть на самом деле. Пуля, попавшая по ней здесь, прошлому
        // снимку не видна: у того, кто его прислал, прочность не менялась.
        if (const shared::VehicleHarm hit = snapshot_.settleHealth(entry.vehicle, state, localPed);
            hit.any() && onDamage_) {
            onDamage_(state.id, hit, weaponInHand(localPed));
        }

        // Сцепка — по снимку тягача, и только по нему: прицеп о ней не знает.
        // Номер сцепленного в игре ищется по номеру сессии; не нашёлся — его у
        // нас ещё нет, и сцепим в следующем кадре.
        snapshot_.applyHitch(
            entry.vehicle,
            game::VehicleSnapshot::Hitched{
                .vehicle =
                    state.trailer == shared::kInvalidVehicleId ? 0 : handleFor(state.trailer),
                .onHook = shared::has(state.flags, shared::VehicleFlag::TowHook)},
            entry.applied.trailer != shared::kInvalidVehicleId);

        const bool repaired = snapshot_.applyControls(entry.vehicle, state, entry.applied);

        snapshot_.applyDamage(entry.vehicle, state, entry.applied);

        entry.applied = state;

        // Починенная машина вышла из-под ремонта заводской, и вместе с вмятинами
        // с неё снялось всё, что мы накладывали когда-то: свет, сирена,
        // зажигание, крыша. Накладывается это только на изменение, а изменения
        // не будет — у приславшего снимок ничего не поменялось.
        //
        // Поэтому наложенное объявляется забытым: следующий кадр наложит его
        // заново. Заводское состояние здесь берётся не с потолка — ровно таким
        // машина и выходит из-под SET_VEHICLE_FIXED.
        if (repaired) {
            entry.applied = shared::VehicleState{};
        }
    }
}

std::vector<shared::VehicleState> Vehicles::describeOwned(int localPed) const {
    std::vector<shared::VehicleState> snapshots;

    if (!ready()) {
        return snapshots;
    }

    // За рулём какой машины мы сидим — спрашивается один раз, а не для каждой
    // ведомой: машин у нас бывает десяток, а сидим мы в лучшем случае в одной.
    const auto seat = seatOf(localPed);
    const int driven =
        seat && seat->index == shared::kDriverSeat ? seat->vehicle : 0;

    for (const auto& [id, entry] : vehicles_) {
        if (!entry.ours) {
            continue;
        }

        shared::VehicleState state = snapshot_.read(entry.vehicle, entry.vehicle == driven);
        state.id = id;
        state.model = entry.model;

        // Сцепленное называется номером сессии, а не номером в игре: у
        // получателя номера игры свои, и по нашему он не найдёт ничего. То, о
        // чём сессия не знает, не называется вовсе — сказать о нём нечего.
        if (const auto hitched = snapshot_.hitchedTo(entry.vehicle); hitched.vehicle != 0) {
            state.trailer = idOf(hitched.vehicle);

            if (hitched.onHook) {
                state.flags |= static_cast<std::uint16_t>(shared::VehicleFlag::TowHook);
            }
        }

        snapshots.push_back(state);
    }

    return snapshots;
}

void Vehicles::clear() {
    if (!ready()) {
        return;
    }

    for (const auto& [id, entry] : vehicles_) {
        remove(entry.vehicle);
    }

    vehicles_.clear();
    appearances_.clear();
}

} // namespace oxymp::client::game
