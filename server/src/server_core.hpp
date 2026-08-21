#pragma once

#include "config.hpp"
#include "blip_directory.hpp"
#include "object_directory.hpp"
#include "player_registry.hpp"
#include "vehicle_directory.hpp"
#include "world_clock.hpp"

#include <oxymp/script/core.hpp>
#include <oxymp/script/events.hpp>

namespace oxymp::server {

/// Куда уходит то, что ядро изменило.
///
/// Ядру нужно не только записать перемену в реестр, но и рассказать о ней
/// клиентам, — а рассказывать умеет сервер, и только он: у него сокеты, у него
/// списки того, кто что уже видит. Прямая ссылка на сервер решала бы это в одну
/// строку и стоила бы проверяемости: ядро потянуло бы за собой ENet, и убедиться,
/// что скрипт не может вылечить вышедшего игрока, стало бы можно только подняв
/// сессию.
///
/// Поэтому здесь перечислено ровно то, чего ядро само не может. В проверках
/// вместо сервера подставляется список записей — тем же приёмом, каким проверялся
/// сам слой на подставном ядре.
class CoreSink {
public:
    virtual ~CoreSink() = default;

    CoreSink(const CoreSink&) = delete;
    CoreSink& operator=(const CoreSink&) = delete;

    /// Здоровье или броня игрока изменились не от чужой руки.
    virtual void healthChanged(const Player& player) = 0;

    /// Внешность игрока изменилась на сервере и должна уйти по сети.
    ///
    /// Всем, включая самого игрока, — в отличие от внешности, пришедшей от
    /// клиента. Ту он объявил сам и у себя её уже надел; эту ему назначили, и
    /// узнать о ней ему неоткуда.
    virtual void appearanceChanged(const Player& player) = 0;

    /// Игрока следует перенести в точку.
    ///
    /// Единственное, чего сервер не делает у себя: персонаж живёт в игре у
    /// своего хозяина, и переставить его может только она.
    virtual void teleported(const Player& player, const shared::Vec3& position) = 0;

    /// Игроку следует передать именованное событие.
    virtual void emitted(const Player& player, std::string_view name,
                         std::string_view payload) = 0;

    /// Игрока следует посадить в машину: просьба ему самому.
    virtual void seated(const Player& player, shared::VehicleId vehicle, std::int8_t seat) = 0;

    /// Игрока следует выгнать из сессии.
    virtual void kicked(const Player& player, std::string_view reason) = 0;

    /// Снаряжение игрока изменилось. replace — прежнее отобрать.
    virtual void loadoutChanged(const Player& player, bool replace) = 0;

    /// Машина заведена.
    ///
    /// Объявлять её клиентам сервер будет сам и не сразу: рассказывают о машине
    /// те, кто её увидит, и делает это раздача. Здесь — только повод её
    /// поторопить.
    virtual void vehicleAdded(shared::VehicleId id) = 0;

    /// Машину следует переставить: просьба её ведущему.
    ///
    /// Отдельно от самой машины и только ему: остальные узнают о новом месте
    /// обычным снимком — тем, который ведущий пришлёт следующим тактом.
    virtual void vehicleTeleported(shared::VehicleId id, const shared::Vec3& position,
                                   float heading) = 0;

    /// Машину следует починить: просьба её ведущему.
    virtual void vehicleRepaired(shared::VehicleId id) = 0;

    /// Машины больше нет. Об этом, в отличие от появления, нужно сказать сразу и
    /// всем: клиент, у которого она заведена, иначе оставит её стоять навсегда.
    virtual void vehicleRemoved(shared::VehicleId id) = 0;

    virtual void objectAdded(shared::ObjectId id) = 0;
    virtual void objectRemoved(shared::ObjectId id) = 0;

    /// Метка заведена или поправлена — рассказать о ней всем, кто её видит.
    ///
    /// Одним поводом на то и другое: получателю разницы нет, а нам не нужно
    /// помнить, кому о какой метке уже сказано.
    virtual void blipChanged(shared::BlipId id) = 0;

    virtual void blipRemoved(shared::BlipId id) = 0;

    /// Погода или время сменились и должны уйти немедленно.
    virtual void worldChanged() = 0;

    /// Строка в чат. kInvalidPlayerId — всем.
    virtual void chatLine(shared::PlayerId to, std::string text) = 0;

protected:
    CoreSink() = default;
};

/// Ядро скриптового слоя, собранное на реестрах сервера.
///
/// Ничего не хранит, и это главное правило слоя: правда о сессии лежит в
/// PlayerRegistry, VehicleDirectory, ObjectDirectory и WorldClock, а ядро только
/// показывает её и меняет. Заведи оно свои списки — они разошлись бы с реестрами
/// в первый же день, и разошлись бы молча.
///
/// Оттого и снимки наружу: скрипт получает PlayerInfo — слепок мгновения, — а не
/// ссылку на живую запись, которая успеет исчезнуть вместе с вышедшим игроком.
class ServerCore final : public script::Core {
public:
    ServerCore(PlayerRegistry& players, VehicleDirectory& vehicles, ObjectDirectory& objects,
               BlipDirectory& blips, WorldClock& world, const Config& config,
               script::Events& events, CoreSink& sink) noexcept;

    // --- Игроки ----------------------------------------------------------------

    [[nodiscard]] std::vector<script::PlayerInfo> players() const override;
    [[nodiscard]] std::optional<script::PlayerInfo> player(shared::PlayerId id) const override;

    bool setHealth(shared::PlayerId id, std::uint16_t health, std::uint16_t armour) override;
    bool giveWeapon(shared::PlayerId id, std::uint32_t weapon, std::uint16_t ammo) override;
    bool clearWeapons(shared::PlayerId id) override;
    bool setModel(shared::PlayerId id, std::uint32_t model) override;
    bool setIntoVehicle(shared::PlayerId id, shared::VehicleId vehicle,
                        std::int8_t seat) override;
    bool setClothes(shared::PlayerId id, std::uint8_t component, std::uint8_t drawable,
                    std::uint8_t texture, std::uint8_t palette) override;
    bool setProp(shared::PlayerId id, std::uint8_t index, std::int8_t drawable,
                 std::int8_t texture) override;
    bool setDimension(shared::PlayerId id, std::int32_t dimension) override;
    bool teleport(shared::PlayerId id, const shared::Vec3& position) override;
    bool kick(shared::PlayerId id, std::string_view reason) override;
    bool emit(shared::PlayerId id, std::string_view name, std::string_view payload) override;

    // --- Машины ----------------------------------------------------------------

    [[nodiscard]] std::vector<script::VehicleInfo> vehicles() const override;
    [[nodiscard]] std::optional<script::VehicleInfo> vehicle(shared::VehicleId id) const override;

    [[nodiscard]] shared::VehicleId createVehicle(std::uint32_t model,
                                                  const shared::Vec3& position,
                                                  float heading) override;

    bool removeVehicle(shared::VehicleId id) override;
    bool setVehicleDimension(shared::VehicleId id, std::int32_t dimension) override;
    bool teleportVehicle(shared::VehicleId id, const shared::Vec3& position,
                         float heading) override;
    bool repairVehicle(shared::VehicleId id) override;

    // --- Предметы --------------------------------------------------------------

    [[nodiscard]] std::vector<script::ObjectInfo> objects() const override;
    [[nodiscard]] std::optional<script::ObjectInfo> object(shared::ObjectId id) const override;

    [[nodiscard]] shared::ObjectId createObject(std::uint32_t model, const shared::Vec3& position,
                                                const shared::Vec3& rotation) override;

    bool removeObject(shared::ObjectId id) override;
    bool setObjectDimension(shared::ObjectId id, std::int32_t dimension) override;

    [[nodiscard]] std::vector<script::BlipInfo> blips() const override;
    [[nodiscard]] std::optional<script::BlipInfo> blip(shared::BlipId id) const override;
    [[nodiscard]] shared::BlipId createBlip(const script::BlipInfo& blip) override;
    bool updateBlip(shared::BlipId id, const script::BlipInfo& blip) override;
    bool removeBlip(shared::BlipId id) override;

    // --- Мир и общение ---------------------------------------------------------

    void broadcast(std::string_view text) override;
    bool tell(shared::PlayerId id, std::string_view text) override;

    bool setWeather(std::string_view weather) override;
    bool setTime(std::uint8_t hour, std::uint8_t minute) override;

private:
    PlayerRegistry* players_ = nullptr;
    VehicleDirectory* vehicles_ = nullptr;
    ObjectDirectory* objects_ = nullptr;
    BlipDirectory* blips_ = nullptr;
    WorldClock* world_ = nullptr;
    const Config* config_ = nullptr;
    script::Events* events_ = nullptr;
    CoreSink* sink_ = nullptr;
};

} // namespace oxymp::server
