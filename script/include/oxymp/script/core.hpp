#pragma once

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oxymp::script {

/// Что скрипт знает об игроке.
///
/// Снимок, а не ссылка на живую запись, и это намеренно. Скрипт волен подержать
/// его у себя, положить в список, сравнить с прежним — и ничего от этого не
/// сломается: снимок описывает мгновение и устаревает молча. Живая запись за то
/// же время успела бы исчезнуть вместе с вышедшим игроком.
struct PlayerInfo {
    shared::PlayerId id = shared::kInvalidPlayerId;
    std::string nickname;

    shared::Vec3 position;
    float heading = 0.0F;

    std::uint16_t health = 0;
    std::uint16_t armour = 0;

    /// В какой машине сидит. kInvalidVehicleId — идёт пешком.
    shared::VehicleId vehicle = shared::kInvalidVehicleId;
    std::int8_t seat = shared::kNoSeat;

    /// Позволено ли ему распоряжаться сессией.
    ///
    /// Здесь, а не отдельным вопросом к ядру, и это существенно: право — такое
    /// же свойство игрока, как имя, и спрашивать о нём отдельно пришлось бы в
    /// каждом обработчике команды. Решает при этом по-прежнему сервер: скрипт
    /// видит ответ, но не назначает его.
    ///
    /// Без этого поля ресурсу нельзя доверить ничего, что меняет сессию, — и
    /// первые встроенные команды оттого умели только рассказывать.
    bool admin = false;
};

/// Что скрипт знает о машине.
struct VehicleInfo {
    shared::VehicleId id = shared::kInvalidVehicleId;
    std::uint32_t model = 0;

    shared::Vec3 position;
    shared::Vec3 rotation;

    /// Кто её ведёт. kInvalidPlayerId — никто, машина стоит.
    shared::PlayerId owner = shared::kInvalidPlayerId;
};

/// Что скрипт знает о предмете.
struct ObjectInfo {
    shared::ObjectId id = shared::kInvalidObjectId;
    std::uint32_t model = 0;

    shared::Vec3 position;
    shared::Vec3 rotation;
};

/// Всё, до чего дотягивается скрипт.
///
/// Объявлено интерфейсом, а не написано здесь же, и причина не в отвлечённой
/// чистоте. Слой не хранит правду о мире — она лежит в реестрах сервера, — а
/// значит ему нужен способ до них дотянуться. Сделай он это напрямую, включив
/// заголовки сервера, — и проверить его стало бы нельзя: пришлось бы поднимать
/// сеть, чтобы убедиться, что событие о входе игрока дошло до обработчика.
///
/// Здесь же лежит и граница дозволенного: всё, чего в этом перечне нет, скрипту
/// недоступно. Перечень будет расти, и это нормально — но расти он должен
/// осознанно, а не оттого, что кому-то оказалось удобно дотянуться до реестра
/// напрямую.
class Core {
public:
    virtual ~Core() = default;

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // --- Игроки ----------------------------------------------------------------

    [[nodiscard]] virtual std::vector<PlayerInfo> players() const = 0;

    /// Пусто, если такого игрока в сессии нет.
    [[nodiscard]] virtual std::optional<PlayerInfo> player(shared::PlayerId id) const = 0;

    /// Ставит здоровье и броню. false — игрока уже нет.
    virtual bool setHealth(shared::PlayerId id, std::uint16_t health, std::uint16_t armour) = 0;

    /// Выдаёт оружие с боезапасом.
    virtual bool giveWeapon(shared::PlayerId id, std::uint32_t weapon, std::uint16_t ammo) = 0;

    /// Отбирает всё оружие.
    virtual bool clearWeapons(shared::PlayerId id) = 0;

    /// Переносит игрока в точку.
    ///
    /// Единственное, чего сервер не делает у себя: персонаж живёт в игре у
    /// своего хозяина, и переставить его может только она. Отсюда и разница в
    /// поведении с остальным: здоровье меняется мгновенно, а перенос — просьба,
    /// исполняемая на той стороне.
    virtual bool teleport(shared::PlayerId id, const shared::Vec3& position) = 0;

    /// Отправляет игроку именованное событие.
    ///
    /// Так ресурс говорит со страницей интерфейса: посылает ей то, что
    /// показывать, и получает обратно нажатия. Нагрузка — строка, и толкует её
    /// только страница; клиент передаёт её не читая.
    virtual bool emit(shared::PlayerId id, std::string_view name,
                      std::string_view payload) = 0;

    // --- Машины ----------------------------------------------------------------

    [[nodiscard]] virtual std::vector<VehicleInfo> vehicles() const = 0;
    [[nodiscard]] virtual std::optional<VehicleInfo> vehicle(shared::VehicleId id) const = 0;

    /// Заводит машину. kInvalidVehicleId — отказ: предел исчерпан или модель
    /// негодная.
    [[nodiscard]] virtual shared::VehicleId createVehicle(std::uint32_t model,
                                                          const shared::Vec3& position,
                                                          float heading) = 0;

    virtual bool removeVehicle(shared::VehicleId id) = 0;

    // --- Предметы --------------------------------------------------------------

    [[nodiscard]] virtual std::vector<ObjectInfo> objects() const = 0;
    [[nodiscard]] virtual std::optional<ObjectInfo> object(shared::ObjectId id) const = 0;

    [[nodiscard]] virtual shared::ObjectId createObject(std::uint32_t model,
                                                        const shared::Vec3& position,
                                                        const shared::Vec3& rotation) = 0;

    virtual bool removeObject(shared::ObjectId id) = 0;

    // --- Мир и общение ---------------------------------------------------------

    /// Строка в чат всем. Пустая не отправляется.
    virtual void broadcast(std::string_view text) = 0;

    /// Строка в чат одному игроку.
    virtual bool tell(shared::PlayerId id, std::string_view text) = 0;

    virtual bool setWeather(std::string_view weather) = 0;
    virtual bool setTime(std::uint8_t hour, std::uint8_t minute) = 0;

protected:
    Core() = default;
};

} // namespace oxymp::script
