#pragma once

#include "native_table.hpp"

namespace oxymp::client::game {

/// Подавление одиночного сюжета и сетевого режима Rockstar.
///
/// Полностью запретить стартовые скрипты игры нельзя: вместе с сюжетом отсюда
/// растут меню паузы, HUD и прочее, и без них игра падает при первом же
/// обращении к ним — проверено вылетом по 0xC0000005 на нажатии Esc. Поэтому
/// скрипты запускаются как обычно, а гасятся поимённо — те, что мешают.
///
/// Вызывать можно только изнутри скриптового тика.
class Story {
public:
    explicit Story(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Идёт ли сейчас кат-сцена.
    [[nodiscard]] bool cutsceneRunning() const;

    /// Обрывает идущую кат-сцену и выгружает её. Безвредно, если её нет.
    void stopCutscene() const;

    /// Возвращает игроку управление, если игра его отобрала.
    ///
    /// Проверка перед выдачей обязательна: выдавать управление каждый кадр без
    /// разбора значит спорить с игрой в те моменты, когда она отбирает его по
    /// делу — например на смерти персонажа.
    void restoreControl() const;

    /// Убирает оставшийся от сюжета текст на экране.
    ///
    /// Завершённый скрипт уносит с собой не всё: название миссии, подсказки и
    /// подписи внизу экрана остаются висеть, потому что рисует их не сам скрипт,
    /// а игра по его прежней просьбе. Убрать их можно только отдельно.
    void clearMessages() const;

    /// Снимает признак идущей миссии.
    ///
    /// Пока он выставлен, игра считает происходящее миссией и ведёт себя
    /// соответственно: показывает её мишуру и проваливает её при смерти игрока.
    void clearMissionFlag() const;

    /// Завершает сюжетные скрипты: и те, что заводят миссии, и сами миссии.
    ///
    /// ВАЖНО: у обоих завершающих методов два условия, и оба обязательны.
    ///
    /// Первое: игрок уже получил собственную модель. Сюжетный персонаж
    /// принадлежит сюжетным скриптам, и завершение скрипта уносит его с собой —
    /// вместе с игроком.
    ///
    /// Второе: игра уже дошла до обычной игры. На старте она распоряжается
    /// своими скриптами сама, и вмешательство в этот момент роняет её по
    /// 0xC0000005 через несколько секунд после запуска стартовых скриптов.
    /// Проверено дважды, оба раза по одному и тому же адресу.
    ///
    /// Отсутствующее имя безвредно: игра просто ничего не найдёт. Благодаря
    /// этому список можно вести с запасом, не выясняя заранее, какие скрипты
    /// запущены прямо сейчас.
    void terminateStory() const;

    /// Завершает скрипты сетевого режима Rockstar.
    ///
    /// Нужно не для удобства, а по существу: изменённому клиенту в GTA Online
    /// делать нечего, и попасть туда он не должен ни при каких обстоятельствах.
    /// Дешевле не пустить, чем объяснять потом блокировку учётной записи.
    ///
    /// Условия те же, что у terminateStory: до обычной игры вызывать нельзя.
    void terminateOnline() const;

    /// Завершает скрипты телефона.
    ///
    /// Телефон в GTA — не часть интерфейса, а собственные скрипты игры, и
    /// показывает он контакты сюжетных персонажей, которых в мультиплеере нет.
    ///
    /// Условия те же, что у terminateStory: до обычной игры вызывать нельзя.
    void terminatePhone() const;

private:
    void terminate(const char* scriptName) const;

    NativeHandler terminateByName_ = nullptr;
    NativeHandler playerId_ = nullptr;
    NativeHandler isCutsceneActive_ = nullptr;
    NativeHandler isCutscenePlaying_ = nullptr;
    NativeHandler stopCutscene_ = nullptr;
    NativeHandler removeCutscene_ = nullptr;
    NativeHandler isControlOn_ = nullptr;
    NativeHandler setControl_ = nullptr;
    NativeHandler setMissionFlag_ = nullptr;
    NativeHandler clearPrints_ = nullptr;
    NativeHandler clearBrief_ = nullptr;
    NativeHandler clearHelp_ = nullptr;
    NativeHandler clearSmallPrints_ = nullptr;
    NativeHandler clearFloatingHelp_ = nullptr;
    NativeHandler flushNotifications_ = nullptr;
};

} // namespace oxymp::client::game
