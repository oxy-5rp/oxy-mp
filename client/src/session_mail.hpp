#pragma once

#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oxymp::client {

/// Почта между потоком игры и сетевым потоком.
///
/// Всё остальное, что ходит между ними, — это состояние: последнее значение
/// вытесняет предыдущее, и потерять промежуточное не жалко. Здесь наоборот:
/// реплика в чате и попадание — события, каждое из которых обязано дойти ровно
/// один раз. Состоянием их не передать.
///
/// Направления два и они не смешиваются: игра просит отправить, сеть приносит
/// пришедшее. Раскладывать их по разным объектам незачем — оба конца одинаковы,
/// а вместе видно, что именно эти два потока обмениваются событиями.
///
/// Блокировка короткая и не спорная: под ней происходит только обмен векторов.
class SessionMail {
public:
    // --- Игра просит отправить -------------------------------------------------

    /// Реплика игрока в чат.
    void postChat(std::string text) {
        if (text.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        outgoingChat_.push_back(std::move(text));
    }

    /// Попадание по чужому игроку: считает его тот, кто попал.
    void postDamage(shared::PlayerId victim, std::uint16_t amount, std::uint32_t weapon) {
        shared::DamageReport report;
        report.victim = victim;
        report.amount = amount;
        report.weapon = weapon;

        const std::lock_guard guard{mutex_};
        outgoingDamage_.push_back(report);
    }

    [[nodiscard]] std::vector<std::string> takeOutgoingChat() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingChat_, {});
    }

    [[nodiscard]] std::vector<shared::DamageReport> takeOutgoingDamage() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingDamage_, {});
    }

    /// Свой выстрел: замечает его игровой поток, отправляет сетевой.
    void postShot(std::uint32_t weapon, const shared::Vec3& target) {
        shared::WeaponFired fired;
        fired.weapon = weapon;
        fired.target = target;

        const std::lock_guard guard{mutex_};
        outgoingShots_.push_back(fired);
    }

    [[nodiscard]] std::vector<shared::WeaponFired> takeOutgoingShots() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingShots_, {});
    }

    /// Попадание по чужой машине.
    ///
    /// Тем же путём, что и попадание по человеку, и по той же причине: заметил
    /// его игровой поток, а отправляет сетевой.
    void postVehicleDamage(shared::VehicleId vehicle, const shared::VehicleHarm& harm,
                           std::uint32_t weapon) {
        shared::VehicleDamageReport report;
        report.vehicle = vehicle;
        report.harm = harm;
        report.weapon = weapon;

        const std::lock_guard guard{mutex_};
        outgoingVehicleDamage_.push_back(report);
    }

    [[nodiscard]] std::vector<shared::VehicleDamageReport> takeOutgoingVehicleDamage() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingVehicleDamage_, {});
    }

    /// Попадания по машинам, которые ведём мы. Пришли от сервера, применить их
    /// у себя обязаны мы: прочность машины живёт в игре у ведущего.
    void deliverVehicleDamage(std::vector<shared::VehicleDamaged> hits) {
        if (hits.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingVehicleDamage_.insert(incomingVehicleDamage_.end(), hits.begin(), hits.end());
    }

    [[nodiscard]] std::vector<shared::VehicleDamaged> takeIncomingVehicleDamage() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingVehicleDamage_, {});
    }

    /// Клавиша, нажатая или отпущенная игроком.
    ///
    /// Кладёт её перехват ввода — он живёт в потоке слоя интерфейса, — а
    /// забирает игровой поток. Через почту, а не прямым вызовом, и это не
    /// формальность: обработчик ресурса зовёт нативы, а нативы игра принимает
    /// только из своего потока. Позови мы скриптовую машину прямо из перехвата,
    /// первый же `alt.on('keydown')` с нативом внутри уронил бы игру.
    ///
    /// Событиями, а не состоянием: нажатие и отпускание — действия, и потерять
    /// их нельзя. Потерянное отпускание оставило бы клавишу «зажатой» навсегда,
    /// и `alt.isKeyDown` врал бы до конца сессии.
    struct KeyEvent {
        std::uint32_t code = 0;
        bool down = false;
    };

    void postKey(std::uint32_t code, bool down) {
        const std::lock_guard guard{mutex_};
        keys_.push_back(KeyEvent{.code = code, .down = down});
    }

    [[nodiscard]] std::vector<KeyEvent> takeKeys() {
        const std::lock_guard guard{mutex_};
        return std::exchange(keys_, {});
    }

    /// Именованное событие серверу: нажатие в интерфейсе.
    ///
    /// Единственное, чем игра теперь просит сервер что-либо сделать. Раньше
    /// здесь стояли просьбы завести машину, выдать оружие, сменить погоду — по
    /// очереди на каждую, — и все они выросли из админ-меню, жившего в клиенте.
    /// Меню уехало на сервер, а с ним и правила.
    void postEvent(std::string name, std::string payload) {
        if (name.empty()) {
            return;
        }

        shared::ClientEvent event;
        event.name = std::move(name);
        event.payload = std::move(payload);

        const std::lock_guard guard{mutex_};
        outgoingEvents_.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<shared::ClientEvent> takeOutgoingEvents() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingEvents_, {});
    }

    // --- Сеть приносит пришедшее -----------------------------------------------

    void deliverDamage(std::vector<shared::DamageTaken> taken) {
        if (taken.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingDamage_.insert(incomingDamage_.end(), taken.begin(), taken.end());
    }

    [[nodiscard]] std::vector<shared::DamageTaken> takeIncomingDamage() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingDamage_, {});
    }

    /// Снаряжение, выданное сервером.
    ///
    /// Состоянием, а не событием: список полный, и последний вытесняет
    /// предыдущий, ничего не теряя.
    void deliverLoadout(std::optional<shared::PlayerLoadout> loadout) {
        if (!loadout) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingLoadout_ = std::move(loadout);
    }

    [[nodiscard]] std::optional<shared::PlayerLoadout> takeIncomingLoadout() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingLoadout_, std::nullopt);
    }

    /// Здоровье, назначенное сервером.
    void deliverHealth(std::optional<shared::HealthChanged> health) {
        if (!health) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingHealth_ = health;
    }

    [[nodiscard]] std::optional<shared::HealthChanged> takeIncomingHealth() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingHealth_, std::nullopt);
    }

    /// Появившиеся и пропавшие предметы.
    ///
    /// Событиями, а не состоянием: каждое появление и каждая пропажа — это
    /// отдельное действие над миром, и потерять их нельзя.
    /// Точки, в которые сервер велел перенести игрока.
    ///
    /// Событиями: перенос — действие, и потерять его нельзя. Исполняет их поток
    /// игры, потому что переставить персонажа может только она.
    void deliverTeleports(std::vector<shared::Vec3> points) {
        if (points.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        teleports_.insert(teleports_.end(), points.begin(), points.end());
    }

    /// Кладёт распоряжение о теле. Заменяет прежнее целиком.
    void deliverControl(std::uint8_t flags) { control_ = flags; }

    void deliverVehicleControls(std::vector<shared::VehicleControl> controls) {
        vehicleControls_.insert(vehicleControls_.end(), controls.begin(), controls.end());
    }

    [[nodiscard]] std::vector<shared::VehicleControl> takeVehicleControls() {
        return std::exchange(vehicleControls_, {});
    }

    /// Чем сервер распорядился о теле. Спрашивается каждый кадр и потому не
    /// забирается: наложить признаки однажды нельзя — их сбрасывает подъём из
    /// мёртвых.
    [[nodiscard]] std::uint8_t control() const noexcept { return control_; }

    [[nodiscard]] std::vector<shared::Vec3> takeTeleports() {
        const std::lock_guard guard{mutex_};
        return std::exchange(teleports_, {});
    }

    /// Распоряжения о машинах: переставить и починить.
    ///
    /// Событиями, как и перенос игрока, и по той же причине: это действия, и
    /// потерять их нельзя. Приходят они только ведущему машины — у остальных
    /// её всё равно нет во власти.
    void deliverVehicleTeleports(std::vector<shared::VehicleTeleport> commands) {
        if (commands.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        vehicleTeleports_.insert(vehicleTeleports_.end(), commands.begin(), commands.end());
    }

    [[nodiscard]] std::vector<shared::VehicleTeleport> takeVehicleTeleports() {
        const std::lock_guard guard{mutex_};
        return std::exchange(vehicleTeleports_, {});
    }

    void deliverVehicleRepairs(std::vector<shared::VehicleRepair> commands) {
        if (commands.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        vehicleRepairs_.insert(vehicleRepairs_.end(), commands.begin(), commands.end());
    }

    [[nodiscard]] std::vector<shared::VehicleRepair> takeVehicleRepairs() {
        const std::lock_guard guard{mutex_};
        return std::exchange(vehicleRepairs_, {});
    }

    /// Метки на карте, назначенные сервером.
    void deliverBlips(std::vector<shared::BlipState> blips) {
        if (blips.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        blips_.insert(blips_.end(), std::make_move_iterator(blips.begin()),
                      std::make_move_iterator(blips.end()));
    }

    [[nodiscard]] std::vector<shared::BlipState> takeBlips() {
        const std::lock_guard guard{mutex_};
        return std::exchange(blips_, {});
    }

    void deliverRemovedBlips(std::vector<shared::BlipId> blips) {
        if (blips.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        removedBlips_.insert(removedBlips_.end(), blips.begin(), blips.end());
    }

    [[nodiscard]] std::vector<shared::BlipId> takeRemovedBlips() {
        const std::lock_guard guard{mutex_};
        return std::exchange(removedBlips_, {});
    }

    /// Движения, которые сервер велел сыграть.
    ///
    /// Событиями: движение — это действие, и потерять его нельзя. Исполняет их
    /// поток игры — телом распоряжается только она.
    void deliverAnimations(std::vector<shared::PlayerAnimation> animations) {
        if (animations.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        animations_.insert(animations_.end(), std::make_move_iterator(animations.begin()),
                           std::make_move_iterator(animations.end()));
    }

    [[nodiscard]] std::vector<shared::PlayerAnimation> takeAnimations() {
        const std::lock_guard guard{mutex_};
        return std::exchange(animations_, {});
    }

    /// Взрывы, которые устроил сервер.
    ///
    /// Событиями, как и движения, и по той же причине: взрыв случается один раз,
    /// и потерянный не повторится. Устраивает их поток игры — миром
    /// распоряжается только она.
    void deliverExplosions(std::vector<shared::Explosion> explosions) {
        if (explosions.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        explosions_.insert(explosions_.end(), explosions.begin(), explosions.end());
    }

    [[nodiscard]] std::vector<shared::Explosion> takeExplosions() {
        const std::lock_guard guard{mutex_};
        return std::exchange(explosions_, {});
    }

    /// Чужие выстрелы, о которых сообщил сервер.
    ///
    /// Событиями, как и взрывы: выстрел случается один раз, и потерянный не
    /// повторится. Показывает их поток игры — пулю заводит только она.
    void deliverShots(std::vector<shared::WeaponFired> shots) {
        if (shots.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        shots_.insert(shots_.end(), shots.begin(), shots.end());
    }

    [[nodiscard]] std::vector<shared::WeaponFired> takeShots() {
        const std::lock_guard guard{mutex_};
        return std::exchange(shots_, {});
    }

    /// Как собрано оружие у чужих игроков.
    ///
    /// Состоянием, а не событием, и в этом отличие от выстрела: собранное оружие
    /// держится, пока его не сменят, и куклы, которой оно предназначено, может
    /// ещё не быть. Помнит его RemotePlayers — так же, как помнит одежду.
    void deliverWeaponLooks(std::vector<shared::PlayerWeapon> looks) {
        if (looks.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        weaponLooks_.insert(weaponLooks_.end(), std::make_move_iterator(looks.begin()),
                            std::make_move_iterator(looks.end()));
    }

    [[nodiscard]] std::vector<shared::PlayerWeapon> takeWeaponLooks() {
        const std::lock_guard guard{mutex_};
        return std::exchange(weaponLooks_, {});
    }

    /// Маркеры и контрольные точки, назначенные сервером.
    ///
    /// Тем же порядком, что и метки: пришедшее либо заводится, либо
    /// поправляется, а убранное приходит отдельным списком.
    void deliverPeds(std::vector<shared::PedState> peds, std::vector<shared::PedId> removed) {
        if (peds.empty() && removed.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};

        peds_.insert(peds_.end(), std::make_move_iterator(peds.begin()),
                     std::make_move_iterator(peds.end()));
        removedPeds_.insert(removedPeds_.end(), removed.begin(), removed.end());
    }

    [[nodiscard]] std::vector<shared::PedState> takePeds() {
        const std::lock_guard guard{mutex_};
        return std::exchange(peds_, {});
    }

    [[nodiscard]] std::vector<shared::PedId> takeRemovedPeds() {
        const std::lock_guard guard{mutex_};
        return std::exchange(removedPeds_, {});
    }

    void deliverAttachments(std::vector<shared::EntityAttachment> attachments) {
        if (attachments.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        attachments_.insert(attachments_.end(), std::make_move_iterator(attachments.begin()),
                            std::make_move_iterator(attachments.end()));
    }

    [[nodiscard]] std::vector<shared::EntityAttachment> takeAttachments() {
        const std::lock_guard guard{mutex_};
        return std::exchange(attachments_, {});
    }

    void deliverMarkers(std::vector<shared::MarkerState> markers) {
        if (markers.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        markers_.insert(markers_.end(), std::make_move_iterator(markers.begin()),
                        std::make_move_iterator(markers.end()));
    }

    [[nodiscard]] std::vector<shared::MarkerState> takeMarkers() {
        const std::lock_guard guard{mutex_};
        return std::exchange(markers_, {});
    }

    void deliverRemovedMarkers(std::vector<shared::MarkerId> markers) {
        if (markers.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        removedMarkers_.insert(removedMarkers_.end(), markers.begin(), markers.end());
    }

    [[nodiscard]] std::vector<shared::MarkerId> takeRemovedMarkers() {
        const std::lock_guard guard{mutex_};
        return std::exchange(removedMarkers_, {});
    }

    void deliverCheckpoints(std::vector<shared::CheckpointState> checkpoints) {
        if (checkpoints.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        checkpoints_.insert(checkpoints_.end(), std::make_move_iterator(checkpoints.begin()),
                            std::make_move_iterator(checkpoints.end()));
    }

    [[nodiscard]] std::vector<shared::CheckpointState> takeCheckpoints() {
        const std::lock_guard guard{mutex_};
        return std::exchange(checkpoints_, {});
    }

    void deliverRemovedCheckpoints(std::vector<shared::CheckpointId> checkpoints) {
        if (checkpoints.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        removedCheckpoints_.insert(removedCheckpoints_.end(), checkpoints.begin(),
                                   checkpoints.end());
    }

    [[nodiscard]] std::vector<shared::CheckpointId> takeRemovedCheckpoints() {
        const std::lock_guard guard{mutex_};
        return std::exchange(removedCheckpoints_, {});
    }

    /// Куда сервер велел сесть.
    void deliverSeats(std::vector<shared::PlayerIntoVehicle> seats) {
        if (seats.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        seats_.insert(seats_.end(), seats.begin(), seats.end());
    }

    [[nodiscard]] std::vector<shared::PlayerIntoVehicle> takeSeats() {
        const std::lock_guard guard{mutex_};
        return std::exchange(seats_, {});
    }

    /// Именованные события от сервера.
    ///
    /// Клиент их не толкует: имя и нагрузку сочиняет ресурс сервера, а здесь они
    /// лишь передаются странице интерфейса.
    void deliverServerEvents(std::vector<shared::ServerEvent> events) {
        if (events.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingEvents_.insert(incomingEvents_.end(), events.begin(), events.end());
    }

    [[nodiscard]] std::vector<shared::ServerEvent> takeIncomingEvents() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingEvents_, {});
    }

    /// Клиентская половина ресурса, разложенная и готовая к запуску.
    struct ClientResource {
        /// Имя ресурса — то же, что и на сервере.
        std::string name;

        /// Корень его дерева в кеше клиента.
        std::filesystem::path root;

        /// Точка входа, путём от корня.
        std::string entry;
    };

    /// Сеть разложила ресурсы; поднимать их будет игровой поток.
    ///
    /// Через почту, а не напрямую, и это не формальность: скриптовая машина
    /// зовёт нативы, а нативы игра принимает только из своего потока. Заведи мы
    /// машину там, где качались файлы, — первый же вызов натива уронил бы игру.
    void deliverClientResources(std::vector<ClientResource> resources) {
        if (resources.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        clientResources_.insert(clientResources_.end(),
                                std::make_move_iterator(resources.begin()),
                                std::make_move_iterator(resources.end()));
    }

    [[nodiscard]] std::vector<ClientResource> takeClientResources() {
        const std::lock_guard guard{mutex_};
        return std::exchange(clientResources_, {});
    }

    // --- Расстановка чужой карты -----------------------------------------------

    /// Сеть узнала имена расстановки; просить игру будет игровой поток.
    ///
    /// Через почту, а не напрямую, по той же причине, что и всё остальное здесь:
    /// просьба идёт нативом, а нативы игра принимает только из своего потока.
    void deliverMapPlacements(std::vector<std::string> names) {
        if (names.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        mapPlacements_.insert(mapPlacements_.end(), std::make_move_iterator(names.begin()),
                              std::make_move_iterator(names.end()));
    }

    [[nodiscard]] std::vector<std::string> takeMapPlacements() {
        const std::lock_guard guard{mutex_};
        return std::exchange(mapPlacements_, {});
    }

    // --- Чтение файлов ресурса -------------------------------------------------

    /// Чем достают содержимое файла ресурса. false — такого файла нет.
    ///
    /// `resource` — имя ресурса, `file` — путь от его корня.
    using ResourceReader = std::function<bool(std::string_view resource, std::string_view file,
                                              std::vector<std::uint8_t>& contents)>;

    /// Ставит способ читать файлы ресурсов. Ставит сеть, спрашивают двое других.
    ///
    /// Через почту, а не прямой ссылкой, по той же причине, что и мостик к
    /// окнам: свёртки живут у кеша в сетевом потоке, а спрашивают из них
    /// скриптовая машина (кадр игры) и страницы интерфейса (поток Chromium).
    ///
    /// Пустой обработчик означает «читать неоткуда»: ресурс тогда не поднимется,
    /// и это честнее, чем поднять его с пустыми файлами.
    void setResourceReader(ResourceReader reader) {
        const std::lock_guard guard{mutex_};
        resourceReader_ = std::move(reader);
    }

    [[nodiscard]] ResourceReader resourceReader() const {
        const std::lock_guard guard{mutex_};
        return resourceReader_;
    }

    // --- Окна интерфейса, которыми распоряжается ресурс ------------------------

    /// Чем игровой поток дотягивается до слоя интерфейса.
    ///
    /// Через почту, а не прямой ссылкой, потому что живут они в разных местах и
    /// заводятся в разном порядке: слой поднимается вместе с кадром игры, сессия
    /// — вместе с соединением. Кто из них раньше, зависит от того, как быстро
    /// игрок выбрал сервер.
    ///
    /// Пустые обработчики означают «слоя ещё нет»: окно тогда не заведётся, и
    /// ресурс получит честный отказ вместо тишины.
    struct ViewBridge {
        std::function<std::uint32_t(std::string url)> create;
        std::function<void(std::uint32_t view)> destroy;
        std::function<void(std::uint32_t view, std::string name, std::string arguments)> emit;
        std::function<void(std::uint32_t view, bool visible)> show;
        std::function<void(std::uint32_t view, bool focused)> focus;
    };

    void setViewBridge(ViewBridge bridge) {
        const std::lock_guard guard{mutex_};
        viewBridge_ = std::move(bridge);
    }

    [[nodiscard]] ViewBridge viewBridge() const {
        const std::lock_guard guard{mutex_};
        return viewBridge_;
    }

    /// Событие от страницы ресурса. Кладёт поток CEF, забирает игровой.
    void deliverViewEvent(std::uint32_t view, std::string name, std::string arguments) {
        const std::lock_guard guard{mutex_};
        viewEvents_.push_back(ViewEvent{view, std::move(name), std::move(arguments)});
    }

    struct ViewEvent {
        std::uint32_t view = 0;
        std::string name;
        std::string arguments;
    };

    [[nodiscard]] std::vector<ViewEvent> takeViewEvents() {
        const std::lock_guard guard{mutex_};
        return std::exchange(viewEvents_, {});
    }

    void deliverObjects(std::vector<shared::ObjectAdded> added,
                        std::vector<shared::ObjectId> removed) {
        if (added.empty() && removed.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingObjects_.insert(incomingObjects_.end(), added.begin(), added.end());
        removedObjects_.insert(removedObjects_.end(), removed.begin(), removed.end());
    }

    [[nodiscard]] std::vector<shared::ObjectAdded> takeIncomingObjects() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingObjects_, {});
    }

    [[nodiscard]] std::vector<shared::ObjectId> takeRemovedObjects() {
        const std::lock_guard guard{mutex_};
        return std::exchange(removedObjects_, {});
    }

    /// Погода и время сессии.
    ///
    /// Состоянием, а не событием, и в этом отличие от соседей: промежуточные
    /// значения никому не нужны, важно последнее. Пропущенная минута ничего не
    /// значит — следующая придёт через две секунды.
    void deliverWorld(std::optional<shared::WorldState> state) {
        if (!state) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingWorld_ = std::move(state);
    }

    [[nodiscard]] std::optional<shared::WorldState> takeIncomingWorld() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingWorld_, std::nullopt);
    }

    /// Внешность чужой машины.
    ///
    /// Событием, а не состоянием, и это не мелочь: внешность приходит по одному
    /// сообщению на машину, и сложенная в «последнее значение» она вытеснила бы
    /// сама себя, стоило двум машинам объявиться в один сетевой тик.
    void deliverVehicleAppearances(std::vector<shared::VehicleAppearance> appearances) {
        if (appearances.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingAppearances_.insert(incomingAppearances_.end(), appearances.begin(),
                                    appearances.end());
    }

    [[nodiscard]] std::vector<shared::VehicleAppearance> takeIncomingVehicleAppearances() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingAppearances_, {});
    }

    /// Внешность чужого игрока. Тем же порядком и по той же причине, что и у
    /// машин: сообщение на игрока, и складывать их в «последнее значение»
    /// нельзя.
    void deliverPlayerAppearances(std::vector<shared::PlayerAppearance> appearances) {
        if (appearances.empty()) {
            return;
        }

        const std::lock_guard guard{mutex_};
        incomingPlayerAppearances_.insert(incomingPlayerAppearances_.end(), appearances.begin(),
                                          appearances.end());
    }

    [[nodiscard]] std::vector<shared::PlayerAppearance> takeIncomingPlayerAppearances() {
        const std::lock_guard guard{mutex_};
        return std::exchange(incomingPlayerAppearances_, {});
    }

    /// Своя внешность — наружу. Только когда изменилась: она уходит по надёжному
    /// каналу, и слать её каждый кадр значило бы забить его целиком.
    void postAppearance(shared::PlayerAppearance appearance) {
        const std::lock_guard guard{mutex_};
        outgoingAppearance_ = std::move(appearance);
    }

    [[nodiscard]] std::optional<shared::PlayerAppearance> takeOutgoingAppearance() {
        const std::lock_guard guard{mutex_};
        return std::exchange(outgoingAppearance_, std::nullopt);
    }

private:
    mutable std::mutex mutex_;

    std::vector<std::string> outgoingChat_;
    std::vector<shared::DamageReport> outgoingDamage_;
    std::vector<shared::ClientEvent> outgoingEvents_;

    std::vector<shared::DamageTaken> incomingDamage_;
    std::vector<shared::ServerEvent> incomingEvents_;
    std::vector<ClientResource> clientResources_;
    std::vector<ViewEvent> viewEvents_;
    std::vector<KeyEvent> keys_;
    ViewBridge viewBridge_;

    /// Чем читают файлы ресурсов. Пусто, пока сеть не разобрала ни одного.
    ResourceReader resourceReader_;

    /// Имена расстановки, о которых ещё не просили игру.
    std::vector<std::string> mapPlacements_;
    std::vector<shared::Vec3> teleports_;

    /// Чем сервер распорядился о нашем теле. Состояние, а не очередь.
    std::uint8_t control_ = 0;

    /// Замки машин, о которых сказал сервер и о которых ещё не рассказали игре.
    std::vector<shared::VehicleControl> vehicleControls_;
    std::vector<shared::VehicleTeleport> vehicleTeleports_;
    std::vector<shared::VehicleRepair> vehicleRepairs_;
    std::vector<shared::BlipState> blips_;
    std::vector<shared::BlipId> removedBlips_;
    std::vector<shared::PlayerAnimation> animations_;
    std::vector<shared::Explosion> explosions_;
    std::vector<shared::WeaponFired> shots_;
    std::vector<shared::WeaponFired> outgoingShots_;
    std::vector<shared::VehicleDamageReport> outgoingVehicleDamage_;
    std::vector<shared::VehicleDamaged> incomingVehicleDamage_;
    std::vector<shared::PlayerWeapon> weaponLooks_;
    std::vector<shared::PedState> peds_;
    std::vector<shared::PedId> removedPeds_;
    std::vector<shared::EntityAttachment> attachments_;
    std::vector<shared::MarkerState> markers_;
    std::vector<shared::MarkerId> removedMarkers_;
    std::vector<shared::CheckpointState> checkpoints_;
    std::vector<shared::CheckpointId> removedCheckpoints_;
    std::vector<shared::PlayerIntoVehicle> seats_;
    std::vector<shared::VehicleAppearance> incomingAppearances_;
    std::vector<shared::PlayerAppearance> incomingPlayerAppearances_;
    std::optional<shared::PlayerAppearance> outgoingAppearance_;
    std::optional<shared::WorldState> incomingWorld_;
    std::optional<shared::PlayerLoadout> incomingLoadout_;
    std::optional<shared::HealthChanged> incomingHealth_;
    std::vector<shared::ObjectAdded> incomingObjects_;
    std::vector<shared::ObjectId> removedObjects_;
};

} // namespace oxymp::client
