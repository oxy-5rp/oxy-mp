#include "peds.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

/// Порядок углов поворота. Двойка — тот же, в котором игра отдаёт и принимает
/// поворот сущностей без пересчёта.
constexpr int kRotationOrder = 2;

/// Какого рода куклу заводим: PED_TYPE_MISSION, двадцать шестой в перечне игры.
///
/// Не прохожий с улицы (четвёртый, PED_TYPE_CIVMALE) нарочно. Уличных игра
/// считает своим населением: она вправе убрать их, когда их станет много, и
/// вправе втянуть в свои же сцены — драку, панику, разбегание от выстрела.
/// Кукла, поставленная режимом за прилавок, должна стоять за прилавком.
constexpr int kMissionPed = 26;

/// Сколько патронов выдаётся вместе с оружием.
///
/// Кукла не стреляет — она стоит, — и патроны здесь не для стрельбы. Без них
/// игра считает оружие пустым и убирает его из рук, а нужен как раз вид
/// вооружённого.
constexpr int kWeaponAmmo = 250;

/// Каким тело выходит из-под spawn.
///
/// Место и поворот у него уже те, что просили, — их задаёт само заведение; всё
/// остальное заводское. Нужно это затем, чтобы наложить на новорождённую куклу
/// ровно недостающее: сравнивать её с присланным состоянием как есть значило бы
/// переставить её туда, где она и так стоит.
[[nodiscard]] shared::PedState born(const shared::PedState& state) {
    shared::PedState fresh;
    fresh.position = state.position;
    fresh.rotation = state.rotation;
    return fresh;
}

} // namespace

Peds::Peds(const NativeTable& table) noexcept
    : requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      modelNoLongerNeeded_(table.handlerFor(natives::kSetModelAsNoLongerNeeded)),
      createPed_(table.handlerFor(natives::kCreatePed)),
      deletePed_(table.handlerFor(natives::kDeletePed)),
      doesExist_(table.handlerFor(natives::kDoesEntityExist)),
      setCoords_(table.handlerFor(natives::kSetEntityCoords)),
      setRotation_(table.handlerFor(natives::kSetEntityRotation)),
      freezePosition_(table.handlerFor(natives::kFreezeEntityPosition)),
      asMissionEntity_(table.handlerFor(natives::kSetEntityAsMissionEntity)),
      blockEvents_(table.handlerFor(natives::kSetBlockingOfNonTemporaryEvents)),
      canRagdoll_(table.handlerFor(natives::kSetPedCanRagdoll)),
      setHealth_(table.handlerFor(natives::kSetEntityHealth)),
      setMaxHealth_(table.handlerFor(natives::kSetPedMaxHealth)),
      setArmour_(table.handlerFor(natives::kSetPedArmour)),
      giveWeapon_(table.handlerFor(natives::kGiveWeaponToPed)) {}

Peds::~Peds() {
    // Куклы здесь уже не убрать: разрушение приходится на выгрузку модуля, а она
    // случается вне скриптового тика, где нативы звать нельзя. Убирать их
    // положено вызовом clear до этого мгновения.
}

bool Peds::ready() const noexcept {
    return requestModel_ != nullptr && hasModelLoaded_ != nullptr && createPed_ != nullptr &&
           deletePed_ != nullptr && doesExist_ != nullptr;
}

int Peds::spawn(const shared::PedState& state) {
    if (state.model == 0) {
        return 0;
    }

    invokeNative<void>(requestModel_, state.model);
    if (!invokeNative<bool>(hasModelLoaded_, state.model)) {
        return 0;
    }

    // Последние два признака: кукла не сетевая и принадлежит нашему скрипту.
    // Сетевых в одиночной сессии заводить нельзя — сетевой игры у игры нет.
    const int handle =
        invokeNative<int>(createPed_, kMissionPed, state.model, state.position.x,
                          state.position.y, state.position.z, state.rotation.z, false, false);

    if (handle == 0) {
        return 0;
    }

    if (setRotation_ != nullptr) {
        invokeNative<void>(setRotation_, handle, state.rotation.x, state.rotation.y,
                           state.rotation.z, kRotationOrder, true);
    }

    // Кукла принадлежит нам, а не миру: иначе игра вправе убрать её как лишнюю
    // ровно тогда, когда игрок отвернётся.
    if (asMissionEntity_ != nullptr) {
        invokeNative<void>(asMissionEntity_, handle, true, true);
    }

    // Чужие события ей не указ. Без этого кукла разбегается от выстрелов, идёт
    // смотреть на драку и уходит с места, куда её поставил режим.
    if (blockEvents_ != nullptr) {
        invokeNative<void>(blockEvents_, handle, true);
    }

    // Тряпичной куклой не падает: упавшая, она осталась бы лежать у одного и
    // стоять у другого — считает падение каждый у себя.
    if (canRagdoll_ != nullptr) {
        invokeNative<void>(canRagdoll_, handle, false);
    }

    if (freezePosition_ != nullptr) {
        invokeNative<void>(freezePosition_, handle, true);
    }

    if (modelNoLongerNeeded_ != nullptr) {
        invokeNative<void>(modelNoLongerNeeded_, state.model);
    }

    return handle;
}

void Peds::dress(int handle, const shared::PedState& fresh,
                 const shared::PedState& previous) const {
    if (setMaxHealth_ != nullptr && fresh.maxHealth != previous.maxHealth) {
        // Предел раньше самого здоровья: поставленное сверх предела игра
        // обрежет, и кукла с двумя сотнями жизни осталась бы с сотней.
        invokeNative<void>(setMaxHealth_, handle, static_cast<int>(fresh.maxHealth));
    }

    if (setHealth_ != nullptr && fresh.health != previous.health) {
        invokeNative<void>(setHealth_, handle, static_cast<int>(fresh.health));
    }

    if (setArmour_ != nullptr && fresh.armour != previous.armour) {
        invokeNative<void>(setArmour_, handle, static_cast<int>(fresh.armour));
    }

    if (giveWeapon_ != nullptr && fresh.weapon != previous.weapon && fresh.weapon != 0) {
        // Признаки: не прятать и вложить в руку немедленно. Иначе оружие
        // окажется за спиной, и вооружённой кукла будет только по учёту.
        invokeNative<void>(giveWeapon_, handle, fresh.weapon, kWeaponAmmo, false, true);
    }

    const bool moved = fresh.position.x != previous.position.x ||
                       fresh.position.y != previous.position.y ||
                       fresh.position.z != previous.position.z;

    if (setCoords_ != nullptr && moved) {
        // Признаки те же, что игра ставит сама: не сбивать прохожих, не трогать
        // чужие сущности, поставить на землю.
        invokeNative<void>(setCoords_, handle, fresh.position.x, fresh.position.y,
                           fresh.position.z, false, false, false, true);
    }

    const bool turned = fresh.rotation.x != previous.rotation.x ||
                        fresh.rotation.y != previous.rotation.y ||
                        fresh.rotation.z != previous.rotation.z;

    if (setRotation_ != nullptr && turned) {
        invokeNative<void>(setRotation_, handle, fresh.rotation.x, fresh.rotation.y,
                           fresh.rotation.z, kRotationOrder, true);
    }
}

void Peds::apply(const shared::PedState& state) {
    if (state.id == shared::kInvalidPedId || !ready()) {
        return;
    }

    const auto known = peds_.find(state.id);

    if (known == peds_.end()) {
        Entry entry;
        entry.state = state;
        entry.handle = spawn(state);

        if (entry.handle != 0) {
            dress(entry.handle, state, born(state));
            spdlog::debug("прохожий {} заведён", state.id);
        }

        peds_.emplace(state.id, entry);
        return;
    }

    // Модель у заведённой куклы не меняется: смена модели — это новое тело, а не
    // правка. Сервер этого и не пришлёт, но поверить пакету на слово нельзя:
    // приняв её, мы получили бы куклу, у которой описание и тело о разном.
    const std::uint32_t model = known->second.state.model;

    if (known->second.handle != 0) {
        dress(known->second.handle, state, known->second.state);
    }

    known->second.state = state;
    known->second.state.model = model;
}

void Peds::remove(shared::PedId id) {
    const auto known = peds_.find(id);
    if (known == peds_.end()) {
        return;
    }

    destroy(known->second.handle);
    peds_.erase(known);
}

void Peds::destroy(int handle) const {
    if (handle == 0 || deletePed_ == nullptr) {
        return;
    }

    // Ссылка на номер, а не сам номер: натив обнуляет его у вызывающего.
    int local = handle;

    NativeContext context;
    context.push(&local);
    deletePed_(context.address());
}

void Peds::sync() {
    if (!ready()) {
        return;
    }

    for (auto& [id, entry] : peds_) {
        // Кукла, которую не удалось завести сразу: модель грузилась. Пробуем
        // снова — сервер о ней не забудет, и ждать можно сколько угодно.
        if (entry.handle == 0) {
            entry.handle = spawn(entry.state);

            if (entry.handle != 0) {
                dress(entry.handle, entry.state, born(entry.state));
                spdlog::debug("прохожий {} заведён", id);
            }

            continue;
        }

        // Куклы могло не стать помимо нас — например, её убрал сам движок.
        // Заведём заново в следующем кадре.
        if (!invokeNative<bool>(doesExist_, entry.handle)) {
            entry.handle = 0;
        }
    }
}

int Peds::handleFor(shared::PedId id) const {
    const auto found = peds_.find(id);
    return found == peds_.end() ? 0 : found->second.handle;
}

void Peds::clear() {
    if (!ready()) {
        return;
    }

    for (const auto& [id, entry] : peds_) {
        destroy(entry.handle);
    }

    peds_.clear();
}

} // namespace oxymp::client::game
