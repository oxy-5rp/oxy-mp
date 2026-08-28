#include "objects.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

/// Порядок углов поворота. Двойка — тот же, в котором игра отдаёт и принимает
/// поворот сущностей без пересчёта.
constexpr int kRotationOrder = 2;

/// Дальность, с которой предмет остаётся видимым, в метрах.
constexpr int kLodDistance = 1000;

} // namespace

Objects::Objects(const NativeTable& table) noexcept
    : requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      modelNoLongerNeeded_(table.handlerFor(natives::kSetModelAsNoLongerNeeded)),
      createObject_(table.handlerFor(natives::kCreateObjectNoOffset)),
      deleteObject_(table.handlerFor(natives::kDeleteObject)),
      doesExist_(table.handlerFor(natives::kDoesEntityExist)),
      setCoords_(table.handlerFor(natives::kSetEntityCoords)),
      setRotation_(table.handlerFor(natives::kSetEntityRotation)),
      freezePosition_(table.handlerFor(natives::kFreezeEntityPosition)),
      asMissionEntity_(table.handlerFor(natives::kSetEntityAsMissionEntity)),
      lodDistance_(table.handlerFor(natives::kSetEntityLodDist)) {}

Objects::~Objects() {
    // Предметы здесь уже не убрать: разрушение приходится на выгрузку модуля, а
    // она случается вне скриптового тика, где нативы звать нельзя. Убирать их
    // положено вызовом clear до этого момента.
}

bool Objects::ready() const noexcept {
    return requestModel_ != nullptr && hasModelLoaded_ != nullptr && createObject_ != nullptr &&
           deleteObject_ != nullptr && doesExist_ != nullptr;
}

int Objects::spawn(const Entry& entry) {
    if (entry.model == 0) {
        return 0;
    }

    invokeNative<void>(requestModel_, entry.model);
    if (!invokeNative<bool>(hasModelLoaded_, entry.model)) {
        return 0;
    }

    // Без смещения: точка, присланная сервером, — это то место, где предмет
    // стоит, а не то, откуда игра должна отмерить его собственный центр.
    // Признаки: не сетевой, принадлежит скрипту, без динамики.
    const int handle = invokeNative<int>(createObject_, entry.model, entry.position.x,
                                         entry.position.y, entry.position.z, false, false, false);
    if (handle == 0) {
        return 0;
    }

    if (setRotation_ != nullptr) {
        invokeNative<void>(setRotation_, handle, entry.rotation.x, entry.rotation.y,
                           entry.rotation.z, kRotationOrder, true);
    }

    // Предмет принадлежит нам, а не миру: иначе игра вправе убрать его как
    // лишний ровно тогда, когда игрок отвернётся.
    if (asMissionEntity_ != nullptr) {
        invokeNative<void>(asMissionEntity_, handle, true, true);
    }
    if (lodDistance_ != nullptr) {
        invokeNative<void>(lodDistance_, handle, kLodDistance);
    }

    // Замораживается сразу и навсегда. Отпущенный на попечение физики предмет
    // сползёт по уклону, а от удара машиной укатится — и укатится у каждого
    // по-своему, потому что считать его будет каждый у себя. Неподвижный
    // одинаков у всех без всякой пересылки.
    if (freezePosition_ != nullptr) {
        invokeNative<void>(freezePosition_, handle, true);
    }

    if (modelNoLongerNeeded_ != nullptr) {
        invokeNative<void>(modelNoLongerNeeded_, entry.model);
    }

    return handle;
}

void Objects::place(const Entry& entry) const {
    if (entry.handle == 0) {
        return;
    }

    if (setCoords_ != nullptr) {
        // Последние четыре довода — те же, что и у перестановки игрока:
        // не смещать по осям и не искать землю под ногами. Предмет ставится
        // ровно туда, куда сказал сервер.
        invokeNative<void>(setCoords_, entry.handle, entry.position.x, entry.position.y,
                           entry.position.z, false, false, false, false);
    }

    if (setRotation_ != nullptr) {
        invokeNative<void>(setRotation_, entry.handle, entry.rotation.x, entry.rotation.y,
                           entry.rotation.z, kRotationOrder, true);
    }
}

void Objects::destroy(int handle) const {
    if (handle == 0 || deleteObject_ == nullptr) {
        return;
    }

    // Ссылка на номер, а не сам номер: натив обнуляет его у вызывающего.
    int local = handle;

    NativeContext context;
    context.push(&local);
    deleteObject_(context.address());
}

void Objects::add(const shared::ObjectAdded& object) {
    if (object.id == shared::kInvalidObjectId || !ready()) {
        return;
    }

    // Повторное объявление того же предмета — обычное дело: игрок отошёл, потом
    // вернулся, либо сервер его переставил. Заводить второй такой же не нужно —
    // нужно поправить тот, что уже стоит.
    if (const auto known = objects_.find(object.id); known != objects_.end()) {
        Entry& entry = known->second;

        // Модель сменить у готового предмета нельзя: модель это и есть его
        // тело. Присланная другая означает «завести заново».
        if (entry.model != object.model) {
            destroy(entry.handle);
            objects_.erase(known);
        } else {
            const bool moved = entry.position != object.position ||
                               entry.rotation != object.rotation;

            entry.position = object.position;
            entry.rotation = object.rotation;

            // Незаведённый предмет переставлять нечем: тела у него ещё нет.
            // Новое место при этом уже записано, и заведётся он сразу на нём.
            if (moved && entry.handle != 0) {
                place(entry);
            }

            return;
        }
    }

    Entry entry;
    entry.model = object.model;
    entry.position = object.position;
    entry.rotation = object.rotation;
    entry.handle = spawn(entry);

    objects_.emplace(object.id, entry);
}

void Objects::remove(shared::ObjectId id) {
    const auto known = objects_.find(id);
    if (known == objects_.end()) {
        return;
    }

    destroy(known->second.handle);
    objects_.erase(known);
}

void Objects::sync() {
    if (!ready()) {
        return;
    }

    for (auto& [id, entry] : objects_) {
        // Предмет, который не удалось завести сразу: модель грузилась. Пробуем
        // снова — сервер о нём не забудет, и ждать можно сколько угодно.
        if (entry.handle == 0) {
            entry.handle = spawn(entry);
            continue;
        }

        // Предмета могло не стать помимо нас — например, его убрал сам движок.
        // Заведём заново в следующем кадре.
        if (!invokeNative<bool>(doesExist_, entry.handle)) {
            entry.handle = 0;
        }
    }
}

int Objects::handleFor(shared::ObjectId id) const {
    const auto found = objects_.find(id);
    return found == objects_.end() ? 0 : found->second.handle;
}

void Objects::clear() {
    if (!ready()) {
        return;
    }

    for (const auto& [id, entry] : objects_) {
        destroy(entry.handle);
    }

    objects_.clear();
}

} // namespace oxymp::client::game
