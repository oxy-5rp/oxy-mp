#pragma once

#include "native_table.hpp"

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <functional>
#include <string>
#include <vector>

namespace oxymp::client::game {

/// Меню отладки и распорядителя сессии.
///
/// Открывается клавишей над Tab — той, что в русской раскладке «ё», а в
/// латинской обратный апостроф. Так же устроено во всех знакомых играх, и
/// придумывать своё сочетание незачем.
///
/// Рисует его не игра, а страница интерфейса: списки, разделы и подсветка
/// нужного пункта средствами игрового вывода собираются мучительно, а в
/// разметке получаются сами собой. Сюда же попадает и живой список игроков.
///
/// Клавиши читает клиент, а не страница: страница живёт в чужом браузерном
/// движке и до клавиатуры игры не дотягивается — ровно как и чат.
///
/// Вызывать можно только изнутри скриптового тика: почти каждый пункт — это
/// нативы.
class AdminMenu {
public:
    /// Кого меню показывает в разделе «Игроки».
    struct Participant {
        shared::PlayerId id = shared::kInvalidPlayerId;
        std::string nickname;
        shared::Vec3 position;
    };

    /// Пункт меню, каким его видит страница.
    struct Item {
        std::string label;

        /// Пояснение справа: значение, подсказка или пусто.
        std::string value;
    };

    /// Как меню выглядит сейчас.
    struct View {
        bool open = false;
        std::string title;
        std::vector<Item> items;
        int selected = 0;

        /// Строка внизу: что было сделано последним и чем это кончилось.
        std::string note;
    };

    /// Нажатия, которые меню понимает.
    enum class Key {
        Up,
        Down,
        Enter,

        /// Второе действие над выбранным пунктом. Там, где оно есть, о нём
        /// написано прямо в пункте.
        Alternate,

        Back,
    };

    /// Куда уходят распоряжения, касающиеся чужих игроков.
    ///
    /// Всё остальное меню делает у себя: спавн машины, смена внешности и
    /// телепорт сервера не касаются вовсе. А вот перенести к себе чужого игрока
    /// может только он сам — по просьбе, переданной сервером.
    using Request = std::function<void(shared::AdminCommand command, shared::PlayerId target,
                                       shared::Vec3 position)>;

    explicit AdminMenu(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    void requestThrough(Request request) { request_ = std::move(request); }

    [[nodiscard]] bool open() const noexcept { return open_; }

    void toggle();
    void close();

    /// Обрабатывает нажатие. Ничего не делает, пока меню закрыто.
    void press(Key key, int player, int ped);

    /// Обновляет список игроков и доводит до конца отложенное.
    ///
    /// Отложенное — это всё, что требует загруженной модели: заказанная модель
    /// появляется не в том же кадре, в котором её попросили.
    void update(std::vector<Participant> players, int player, int ped);

    [[nodiscard]] View view() const;

private:
    /// Разделы меню.
    enum class Page {
        Root,
        Vehicles,
        Skins,
        Teleport,
        Players,
        World,
    };

    /// Что делать, когда заказанная модель загрузится.
    enum class Pending {
        Nothing,
        Vehicle,
        Skin,
    };

    void enter(Page page);
    void activate(int player, int ped, bool alternate);

    void spawnVehicle(int ped);
    void applySkin(int player, int ped);

    void teleportSelf(int ped, shared::Vec3 destination) const;

    /// Точка метки, поставленной игроком на карте. Пусто — метки нет.
    [[nodiscard]] bool waypoint(shared::Vec3& destination) const;

    [[nodiscard]] std::size_t itemCount() const;

    Request request_;

    NativeHandler hashKey_ = nullptr;
    NativeHandler requestModel_ = nullptr;
    NativeHandler hasModelLoaded_ = nullptr;
    NativeHandler modelNoLongerNeeded_ = nullptr;
    NativeHandler isModelInCdimage_ = nullptr;
    NativeHandler createVehicle_ = nullptr;
    NativeHandler deleteVehicle_ = nullptr;
    NativeHandler setIntoVehicle_ = nullptr;
    NativeHandler vehiclePedIsIn_ = nullptr;
    NativeHandler isPedInAnyVehicle_ = nullptr;
    NativeHandler vehicleFixed_ = nullptr;
    NativeHandler vehicleDirt_ = nullptr;
    NativeHandler vehicleOnGround_ = nullptr;
    NativeHandler engineOn_ = nullptr;
    NativeHandler setPlayerModel_ = nullptr;
    NativeHandler defaultVariation_ = nullptr;
    NativeHandler playerPedId_ = nullptr;
    NativeHandler getCoords_ = nullptr;
    NativeHandler setCoords_ = nullptr;
    NativeHandler getHeading_ = nullptr;
    NativeHandler setHealth_ = nullptr;
    NativeHandler setArmour_ = nullptr;
    NativeHandler clearBlood_ = nullptr;
    NativeHandler invincible_ = nullptr;
    NativeHandler firstBlip_ = nullptr;
    NativeHandler blipCoord_ = nullptr;
    NativeHandler blipExists_ = nullptr;
    NativeHandler setWeather_ = nullptr;
    NativeHandler setClock_ = nullptr;

    bool open_ = false;
    Page page_ = Page::Root;
    int selected_ = 0;

    /// Отметки выбора по разделам: вернувшись, игрок оказывается там же, где был.
    int rootSelected_ = 0;

    std::string note_;

    Pending pending_ = Pending::Nothing;
    std::uint32_t pendingModel_ = 0;

    /// Сколько кадров ждём заказанную модель. Ждать вечно нельзя: модели,
    /// которой нет, не дождаться никогда.
    int pendingFrames_ = 0;

    /// Неуязвимость — единственное, что меню включает надолго, а не разово.
    bool invincibleOn_ = false;

    std::vector<Participant> players_;
};

} // namespace oxymp::client::game
