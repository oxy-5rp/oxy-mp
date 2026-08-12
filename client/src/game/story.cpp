#include "story.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Признаки для SET_PLAYER_CONTROL. Ноль означает «без особых условий»:
/// управление просто возвращается игроку.
constexpr int kPlainControlFlags = 0;

/// Скрипты одиночного сюжета.
///
/// Первыми идут те, что заводят миссии: пока живы они, погашенная миссия
/// начнётся снова. Дальше — сами миссии начала игры, единственные, до которых
/// сюжет успевает дойти прежде, чем мы его останавливаем.
///
/// Список ведётся с запасом: несуществующее имя игра пропускает молча, а вот
/// пропущенное живое имя оборачивается внезапно начавшейся миссией.
constexpr const char* kStoryScripts[] = {
    "mission_triggerer_a",
    "mission_triggerer_b",
    "mission_triggerer_c",
    "mission_triggerer_d",
    "mission_repeat_controller",
    "mission_stat_watcher",
    "selector",
    "prologue1",
    "prologue2",
    "prologue3",
    "prologue4",
    "prologue5",
    "prologue6",
    "armenian1",
    "armenian2",
    "armenian3",
    "franklin0",
    "franklin1",
    "franklin2",
    "lamar1",
    "michael1",
    "michael2",
    "michael3",
    "michael4",
    "family1",
    "family2",
    "family3",
    "family4",
    "family5",
    "family6",
    "chinese1",
    "chinese2",
    "trevor1",
    "trevor2",
    "trevor3",
    "trevor4",
};

/// Скрипты телефона.
///
/// Телефон в GTA — не часть интерфейса, а собственные скрипты игры. Пока они
/// живы, он открывается по клавише, звонит и показывает контакты сюжетных
/// персонажей, которых в мультиплеере нет и быть не может.
///
/// Первым идёт распорядитель: он заводит остальные, и без него они не
/// возвращаются.
constexpr const char* kPhoneScripts[] = {
    "cellphone_controller",
    "cellphone_flashhand",
};

/// Скрипты сетевого режима Rockstar.
///
/// Первым — тот, через который идёт сам переход: пока он не запустился, до
/// остальных дело не доходит.
constexpr const char* kOnlineScripts[] = {
    "maintransition",
    "freemode",
    "fm_mission_controller",
    "fm_mission_controller_2020",
    "fmmc_launcher",
    "am_launcher",
    "gtao_intro",
    "creator",
};

} // namespace

Story::Story(const NativeTable& table) noexcept
    : terminateByName_(table.handlerFor(natives::kTerminateAllScriptsWithThisName)),
      playerId_(table.handlerFor(natives::kPlayerId)),
      isCutsceneActive_(table.handlerFor(natives::kIsCutsceneActive)),
      isCutscenePlaying_(table.handlerFor(natives::kIsCutscenePlaying)),
      stopCutscene_(table.handlerFor(natives::kStopCutsceneImmediately)),
      removeCutscene_(table.handlerFor(natives::kRemoveCutscene)),
      isControlOn_(table.handlerFor(natives::kIsPlayerControlOn)),
      setControl_(table.handlerFor(natives::kSetPlayerControl)),
      setMissionFlag_(table.handlerFor(natives::kSetMissionFlag)),
      clearPrints_(table.handlerFor(natives::kClearPrints)),
      clearBrief_(table.handlerFor(natives::kClearBrief)),
      clearHelp_(table.handlerFor(natives::kClearAllHelpMessages)),
      clearSmallPrints_(table.handlerFor(natives::kClearSmallPrints)),
      clearFloatingHelp_(table.handlerFor(natives::kClearFloatingHelp)),
      flushNotifications_(table.handlerFor(natives::kFlushNotifications)) {}

bool Story::ready() const noexcept {
    return terminateByName_ != nullptr && playerId_ != nullptr && isCutsceneActive_ != nullptr &&
           isCutscenePlaying_ != nullptr && stopCutscene_ != nullptr && removeCutscene_ != nullptr &&
           isControlOn_ != nullptr && setControl_ != nullptr && setMissionFlag_ != nullptr &&
           clearPrints_ != nullptr && clearBrief_ != nullptr && clearHelp_ != nullptr &&
           clearSmallPrints_ != nullptr && clearFloatingHelp_ != nullptr &&
           flushNotifications_ != nullptr;
}

void Story::clearMessages() const {
    if (!ready()) {
        return;
    }

    invokeNative<void>(clearPrints_);
    invokeNative<void>(clearBrief_);
    invokeNative<void>(clearHelp_, true);
    invokeNative<void>(clearSmallPrints_);
    invokeNative<void>(clearFloatingHelp_, true, true);
    invokeNative<void>(flushNotifications_);
}

void Story::terminate(const char* scriptName) const {
    if (terminateByName_ == nullptr || scriptName == nullptr) {
        return;
    }

    invokeNative<void>(terminateByName_, scriptName);
}

void Story::terminateStory() const {
    for (const char* name : kStoryScripts) {
        terminate(name);
    }
}

void Story::terminateOnline() const {
    for (const char* name : kOnlineScripts) {
        terminate(name);
    }
}

void Story::terminatePhone() const {
    for (const char* name : kPhoneScripts) {
        terminate(name);
    }
}

bool Story::cutsceneRunning() const {
    if (!ready()) {
        return false;
    }

    // Два разных состояния: одна кат-сцена загружена, другая уже проигрывается.
    // Сюжет успевает побывать в обоих, поэтому спрашиваем про оба.
    return invokeNative<bool>(isCutsceneActive_) || invokeNative<bool>(isCutscenePlaying_);
}

void Story::stopCutscene() const {
    if (!ready()) {
        return;
    }

    // Обрыв и выгрузка — разные вещи. Обрыв прекращает показ, но кат-сцена
    // остаётся заряженной, и скрипт, её заказавший, спокойно запустит её снова.
    invokeNative<void>(stopCutscene_, true);
    invokeNative<void>(removeCutscene_);
}

void Story::restoreControl() const {
    if (!ready()) {
        return;
    }

    const int player = invokeNative<int>(playerId_);

    if (!invokeNative<bool>(isControlOn_, player)) {
        invokeNative<void>(setControl_, player, true, kPlainControlFlags);
    }
}

void Story::clearMissionFlag() const {
    if (setMissionFlag_ == nullptr) {
        return;
    }

    invokeNative<void>(setMissionFlag_, false);
}

} // namespace oxymp::client::game
