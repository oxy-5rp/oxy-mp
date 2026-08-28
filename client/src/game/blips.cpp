#include "blips.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {

Blips::Blips(const NativeTable& table) noexcept
    : add_(table.handlerFor(natives::kAddBlipForCoord)),
      removeBlip_(table.handlerFor(natives::kRemoveBlip)),
      exists_(table.handlerFor(natives::kDoesBlipExist)),
      setSprite_(table.handlerFor(natives::kSetBlipSprite)),
      setColour_(table.handlerFor(natives::kSetBlipColour)),
      setAlpha_(table.handlerFor(natives::kSetBlipAlpha)),
      setScale_(table.handlerFor(natives::kSetBlipScale)),
      setDisplay_(table.handlerFor(natives::kSetBlipDisplay)),
      setShortRange_(table.handlerFor(natives::kSetBlipAsShortRange)),
      setPriority_(table.handlerFor(natives::kSetBlipPriority)),
      beginName_(table.handlerFor(natives::kBeginTextCommandSetBlipName)),
      addNamePart_(table.handlerFor(natives::kAddTextComponentSubstringPlayerName)),
      endName_(table.handlerFor(natives::kEndTextCommandSetBlipName)) {}

bool Blips::ready() const noexcept {
    return add_ != nullptr && removeBlip_ != nullptr;
}

void Blips::rename(int blip, const std::string& name) const {
    if (beginName_ == nullptr || addNamePart_ == nullptr || endName_ == nullptr) {
        return;
    }

    // Строка собирается из кусков, как и всякий текст игры: сперва объявляется
    // вид записи, потом кладутся её части, потом всё это применяется. Здесь
    // часть одна, но порядок обязателен — пропустив начало, мы допишем свою
    // строку к чужой, начатой кем-то другим.
    invokeNative<void>(beginName_, "STRING");
    invokeNative<void>(addNamePart_, name.c_str());
    invokeNative<void>(endName_, blip);
}

void Blips::apply(const shared::BlipState& state) {
    if (!ready()) {
        return;
    }

    const auto known = shown_.find(state.id);

    // Уже заведённая метка не переставляется заново: описатель у неё остаётся
    // прежним, и переписываются только свойства. Убрать и завести снова было бы
    // проще, но метка при этом мигнула бы на карте, а маршрут к ней сбился.
    int blip = 0;

    if (known != shown_.end() &&
        (exists_ == nullptr || invokeNative<bool>(exists_, known->second))) {
        blip = known->second;
    } else {
        blip = invokeNative<int>(add_, state.position.x, state.position.y, state.position.z);

        if (blip == 0) {
            spdlog::warn("blip {} was not created", state.id);
            return;
        }

        shown_.insert_or_assign(state.id, blip);
    }

    if (setSprite_ != nullptr) {
        invokeNative<void>(setSprite_, blip, static_cast<int>(state.sprite));
    }
    if (setColour_ != nullptr) {
        invokeNative<void>(setColour_, blip, static_cast<int>(state.colour));
    }
    if (setAlpha_ != nullptr) {
        invokeNative<void>(setAlpha_, blip, static_cast<int>(state.alpha));
    }
    if (setScale_ != nullptr) {
        invokeNative<void>(setScale_, blip, state.scale);
    }
    if (setDisplay_ != nullptr) {
        invokeNative<void>(setDisplay_, blip, static_cast<int>(state.display));
    }
    if (setShortRange_ != nullptr) {
        invokeNative<void>(setShortRange_, blip, state.shortRange);
    }
    if (setPriority_ != nullptr) {
        invokeNative<void>(setPriority_, blip, static_cast<int>(state.priority));
    }

    // Пустую подпись не ставим вовсе: игра подпишет метку сама по её значку, и
    // затирать это пустой строкой значило бы оставить метку безымянной.
    if (!state.name.empty()) {
        rename(blip, state.name);
    }
}

void Blips::remove(shared::BlipId id) {
    const auto known = shown_.find(id);
    if (known == shown_.end()) {
        return;
    }

    if (removeBlip_ != nullptr) {
        // Натив принимает описатель по указателю и обнуляет его: так игра
        // отмечает у себя, что метки больше нет.
        int blip = known->second;
        invokeNative<void>(removeBlip_, &blip);
    }

    shown_.erase(known);
}

void Blips::clear() {
    if (removeBlip_ != nullptr) {
        for (auto& [id, handle] : shown_) {
            int blip = handle;
            invokeNative<void>(removeBlip_, &blip);
        }
    }

    shown_.clear();
}

} // namespace oxymp::client::game
