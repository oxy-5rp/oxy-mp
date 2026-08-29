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
      setFlashes_(table.handlerFor(natives::kSetBlipFlashes)),
      setFlashesAlternate_(table.handlerFor(natives::kSetBlipFlashesAlternate)),
      setFlashInterval_(table.handlerFor(natives::kSetBlipFlashInterval)),
      setFlashTimer_(table.handlerFor(natives::kSetBlipFlashTimer)),
      setBright_(table.handlerFor(natives::kSetBlipBright)),
      setShowCone_(table.handlerFor(natives::kSetBlipShowCone)),
      setFriendly_(table.handlerFor(natives::kSetBlipAsFriendly)),
      setHighDetail_(table.handlerFor(natives::kSetBlipHighDetail)),
      setMissionCreator_(table.handlerFor(natives::kSetBlipAsMissionCreatorBlip)),
      setHeadingIndicator_(table.handlerFor(natives::kShowHeadingIndicatorOnBlip)),
      setTick_(table.handlerFor(natives::kShowTickOnBlip)),
      setNumber_(table.handlerFor(natives::kShowNumberOnBlip)),
      setSecondaryColour_(table.handlerFor(natives::kSetBlipSecondaryColour)),
      setGxtName_(table.handlerFor(natives::kSetBlipNameFromTextFile)),
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

void Blips::applyFlags(int blip, const shared::BlipState& state) const {
    const auto признак = [&](NativeHandler handler, shared::BlipFlag flag) {
        if (handler != nullptr) {
            invokeNative<void>(handler, blip, shared::has(state.flags, flag));
        }
    };

    признак(setFlashes_, shared::BlipFlag::Flashes);
    признак(setFlashesAlternate_, shared::BlipFlag::FlashesAlternate);
    признак(setBright_, shared::BlipFlag::Bright);
    признак(setFriendly_, shared::BlipFlag::Friendly);
    признак(setHighDetail_, shared::BlipFlag::HighDetail);
    признак(setMissionCreator_, shared::BlipFlag::MissionCreator);
    признак(setHeadingIndicator_, shared::BlipFlag::HeadingIndicator);
    признак(setTick_, shared::BlipFlag::Tick);

    // Конус берёт вторым доводом ещё и число — «сколько его видно». Ноль здесь
    // умолчание самой игры.
    if (setShowCone_ != nullptr) {
        invokeNative<void>(setShowCone_, blip, shared::has(state.flags, shared::BlipFlag::ShowCone),
                           0);
    }

    // Мигание: срок и промежуток. Нули означают «как решит игра», и звать с ними
    // натив незачем — он бы объявил метке мигание длиной в ноль.
    if (setFlashInterval_ != nullptr && state.flashInterval != 0) {
        invokeNative<void>(setFlashInterval_, blip, static_cast<int>(state.flashInterval));
    }

    if (setFlashTimer_ != nullptr && state.flashTimer != 0) {
        invokeNative<void>(setFlashTimer_, blip, static_cast<int>(state.flashTimer));
    }

    if (setNumber_ != nullptr) {
        invokeNative<void>(setNumber_, blip, static_cast<int>(state.number));
    }

    // Второй цвет — только если он назван: у натива нет «снять», и позвав его с
    // чёрным, мы объявили бы метке чёрную обводку вместо никакой.
    if (setSecondaryColour_ != nullptr && state.hasSecondaryColour) {
        invokeNative<void>(setSecondaryColour_, blip,
                           static_cast<float>(state.secondaryRed) / 255.0F,
                           static_cast<float>(state.secondaryGreen) / 255.0F,
                           static_cast<float>(state.secondaryBlue) / 255.0F);
    }

    // Имя из словаря игры сильнее своего, и потому накладывается после него:
    // у alt:V это два разных свойства, и назвавший `gxtName` ждёт именно его.
    if (setGxtName_ != nullptr && !state.gxtName.empty()) {
        invokeNative<void>(setGxtName_, blip, state.gxtName.c_str());
    }
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

    // Признаки накладываются все и каждый раз, а не по изменению.
    //
    // Так же, как заморозка игрока и запреты у кукол, и по той же причине: метку
    // игра вправе переписать сама — например, пересоздав её при смене
    // интерьера, — а нового снимка от сервера не придёт, потому что у него
    // ничего не менялось.
    applyFlags(blip, state);

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
