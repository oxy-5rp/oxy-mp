#pragma once

#include <oxymp/script/dimension.hpp>

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <array>
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

    /// Хеш модели персонажа. Ноль — игрок ещё не объявлял внешности.
    ///
    /// Живёт здесь, а не спрашивается отдельно, по той же причине, что и
    /// admin: это свойство игрока, а не отдельный вопрос к серверу.
    std::uint32_t model = 0;

    /// В какой машине сидит. kInvalidVehicleId — идёт пешком.
    shared::VehicleId vehicle = shared::kInvalidVehicleId;
    std::int8_t seat = shared::kNoSeat;

    /// В каком слое мира находится. См. dimension.hpp.
    std::int32_t dimension = kDefaultDimension;

    /// Откуда он подключился, точками.
    ///
    /// Здесь, а не отдельным вопросом к ядру, по той же причине, что и всё
    /// прочее в этом снимке: это свойство игрока. Нужен он всякому режиму —
    /// от списка входов до запрета второго подключения с одного адреса.
    std::string ip;

    /// Круговая задержка до него, в миллисекундах. Ноль — ещё не измерена.
    std::uint32_t ping = 0;

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

    /// В каком слое мира находится. См. dimension.hpp.
    std::int32_t dimension = kDefaultDimension;
};

/// Как машина выкрашена и что на ней навешано.
///
/// Отдельно от `VehicleInfo`, а не полем в нём, и это не придирка к составу.
/// `VehicleInfo` спрашивают в каждом обработчике — ради положения, ведущего,
/// слоя мира, — а внешность спрашивают редко и всю сразу. Возить полсотни байт
/// тюнинга вместе с ответом на вопрос «где машина стоит» значило бы платить за
/// них всякий раз, когда о тюнинге и речи не было.
///
/// Номера машины здесь нет: её называет довод, а не описание. У метки и маркера
/// номер в описании есть, потому что описание — это они сами; внешность же
/// принадлежит машине и сама по себе не живёт.
///
/// Состав взят у сообщения протокола поле в поле, но своей структурой — по той
/// же причине, что и у меток: слой не должен знать про устройство передачи.
struct VehicleAppearanceInfo {
    /// Цвета в нумерации игры: основной, второй, перламутр и колёса.
    std::uint8_t primaryColour = 0;
    std::uint8_t secondaryColour = 0;
    std::uint8_t pearlescentColour = 0;
    std::uint8_t wheelColour = 0;

    /// Своя краска поверх номера палитры. Признак нужен затем, что чёрная
    /// краска осмысленна, а «краски нет» — отдельное состояние.
    bool customPrimary = false;
    std::uint8_t customPrimaryRed = 0;
    std::uint8_t customPrimaryGreen = 0;
    std::uint8_t customPrimaryBlue = 0;

    bool customSecondary = false;
    std::uint8_t customSecondaryRed = 0;
    std::uint8_t customSecondaryGreen = 0;
    std::uint8_t customSecondaryBlue = 0;

    /// Что написано на номерном знаке и какого он вида.
    std::string plate;
    std::uint8_t plateStyle = 0;

    /// Раскраска, тип дисков и тонировка. shared::kStockMod — ничего не выбрано.
    std::int8_t livery = shared::kStockMod;
    std::int8_t wheelType = shared::kStockMod;
    std::int8_t windowTint = shared::kStockMod;

    /// Насколько машина грязная, от нуля до пятнадцати.
    float dirtLevel = 0.0F;

    /// Что стоит в каждом месте тюнинга. shared::kStockMod — заводское.
    std::array<std::int8_t, shared::kVehicleModSlots> mods = shared::stockMods();

    /// Какие места тюнинга включены переключателем, а не выбором из списка:
    /// турбина, дым из-под колёс, ксенон. По биту на место.
    std::uint32_t toggleMods = 0;

    /// Стоят ли на машине не заводские покрышки.
    bool customTyres = false;

    /// Цвет дыма из-под колёс. Виден только когда дым включён переключателем.
    std::uint8_t tyreSmokeRed = 255;
    std::uint8_t tyreSmokeGreen = 255;
    std::uint8_t tyreSmokeBlue = 255;

    /// Неон: какие полосы горят (биты shared::NeonSide) и какого они цвета.
    std::uint8_t neonSides = 0;
    std::uint8_t neonRed = 255;
    std::uint8_t neonGreen = 255;
    std::uint8_t neonBlue = 255;

    /// Какие «дополнения» кузова включены: по биту на номер от первого до
    /// четырнадцатого. Лестницы, багажники, антенны.
    std::uint16_t extras = 0;
};

/// Что скрипт знает о предмете.
struct ObjectInfo {
    shared::ObjectId id = shared::kInvalidObjectId;
    std::uint32_t model = 0;

    shared::Vec3 position;
    shared::Vec3 rotation;

    /// В каком слое мира находится. См. dimension.hpp.
    std::int32_t dimension = kDefaultDimension;
};

/// Что скрипт знает о прохожем.
///
/// Прохожий — кукла, которую ставит сервер: она стоит там, где её поставили, и
/// сама не делает ничего. Тем и отличается от игрока, у которого есть хозяин, и
/// от машины, у которой есть ведущий.
struct PedInfo {
    shared::PedId id = shared::kInvalidPedId;
    std::uint32_t model = 0;

    shared::Vec3 position;
    shared::Vec3 rotation;

    std::uint16_t health = 200;
    std::uint16_t maxHealth = 200;
    std::uint16_t armour = 0;

    /// Хеш оружия в руках. Ноль — безоружный.
    std::uint32_t weapon = 0;

    /// В каком слое мира он стоит. См. dimension.hpp.
    std::int32_t dimension = kDefaultDimension;
};

/// Что скрипт знает о метке на карте.
///
/// Состав взят у alt:V поле в поле: режим, написанный под него, задаёт ровно
/// это. Отдельной структурой, а не сообщением протокола, по той же причине, что
/// и остальные: слой не должен знать про устройство передачи.
struct BlipInfo {
    shared::BlipId id = shared::kInvalidBlipId;

    shared::Vec3 position;

    std::uint16_t sprite = 1;
    std::uint8_t colour = 0;
    std::uint8_t alpha = 255;
    std::uint8_t display = 2;

    bool shortRange = false;
    float scale = 1.0F;

    std::string name;

    /// В каком слое мира метка видна. См. dimension.hpp.
    std::int32_t dimension = kDefaultDimension;
};

/// Что скрипт знает о маркере — фигуре, нарисованной в мире.
///
/// Состав, как и у метки, взят у alt:V поле в поле. Цвет разложен на четыре
/// байта, а не сложен в число: именно так его задаёт режим.
struct MarkerInfo {
    shared::MarkerId id = shared::kInvalidMarkerId;

    /// Какая это фигура. Числа игры, у alt:V они те же.
    std::uint8_t type = 0;

    shared::Vec3 position;
    shared::Vec3 rotation;
    shared::Vec3 direction;
    shared::Vec3 scale{1.0F, 1.0F, 1.0F};

    std::uint8_t red = 255;
    std::uint8_t green = 255;
    std::uint8_t blue = 255;
    std::uint8_t alpha = 255;

    bool visible = true;
    bool bobUpAndDown = false;
    bool faceCamera = false;
    bool rotate = false;

    /// С какого расстояния фигуру видно. Ноль — с любого.
    float streamingDistance = 0.0F;

    /// В каком слое мира она видна. См. dimension.hpp.
    std::int32_t dimension = kDefaultDimension;
};

/// Что скрипт знает о контрольной точке.
///
/// Только её вид. Вход и выход в неё считаются как во всякую зону — там же, где
/// и все прочие зоны, и об этом описании не знают.
struct CheckpointInfo {
    shared::CheckpointId id = shared::kInvalidCheckpointId;

    std::uint8_t type = 0;

    shared::Vec3 position;

    /// Куда показывает стрелка внутри столба — обычно к следующей точке.
    shared::Vec3 nextPosition;

    float radius = 1.0F;
    float height = 2.0F;

    std::uint8_t red = 255;
    std::uint8_t green = 255;
    std::uint8_t blue = 255;
    std::uint8_t alpha = 255;

    std::uint8_t iconRed = 255;
    std::uint8_t iconGreen = 255;
    std::uint8_t iconBlue = 255;
    std::uint8_t iconAlpha = 255;

    bool visible = true;

    float streamingDistance = 0.0F;

    std::int32_t dimension = kDefaultDimension;
};

/// Движение, которое скрипт велит сыграть персонажу.
///
/// Состав, как и у прочего, взят у alt:V поле в поле: режим зовёт
/// `player.playAnimation` ровно с этими доводами.
struct AnimationInfo {
    /// Набор движений игры и движение в нём.
    std::string dictionary;
    std::string name;

    float blendIn = 8.0F;
    float blendOut = 8.0F;

    /// Сколько движению отведено, в миллисекундах. Минус единица — до конца.
    std::int32_t duration = -1;

    /// Признаки проигрывания в нумерации игры.
    std::int32_t flags = 0;

    float playbackRate = 1.0F;

    /// По каким осям движению не позволено возить персонажа.
    bool lockX = false;
    bool lockY = false;
    bool lockZ = false;
};

/// Ссылка на сущность сессии любого рода.
///
/// Своей парой, а не номером, потому что номер без рода не указывает ни на что:
/// нумерация у игроков, машин и предметов своя, и третья машина с третьим
/// предметом по номеру не различаются. Понадобилось это привязке — единственному,
/// что связывает сущности разного рода между собой.
struct EntityRef {
    shared::EntityKind kind = shared::EntityKind::None;
    std::uint32_t id = 0;

    [[nodiscard]] bool operator==(const EntityRef& other) const noexcept = default;
};

/// К чему и как привязана сущность.
///
/// Состав взят у alt:V, но с двумя отличиями, и оба намеренные.
///
/// Поворот здесь в градусах — как везде у нас, — а у alt:V в радианах. Перевод
/// живёт в слое совместимости, там же, где и все прочие расхождения в единицах.
///
/// `fixedRotation` названо так, как его понимает игра: держать поворот намертво.
/// У alt:V оно названо наоборот, `noFixedRotation`, и переворачивает его тот же
/// слой. Своей кости у нас нет вовсе: натива, привязывающего кость к кости, в
/// нашей сборке игры не нашлось, а привязка к кости цели — есть.
struct AttachmentInfo {
    /// К кому привязано. Род None — ни к кому.
    EntityRef target;

    /// К какой кости цели. Минус единица — к самой сущности, а не к кости.
    std::int32_t bone = -1;

    /// Имя кости, если её назвали именем. Непустое старше номера.
    ///
    /// Перевести имя в номер может только игра: номера костей свои у каждой
    /// модели, и сервер их не знает. Поэтому имя доезжает до клиента как есть.
    std::string boneName;

    /// Смещение и поворот относительно точки привязки.
    shared::Vec3 position;
    shared::Vec3 rotation;

    bool collision = false;
    bool fixedRotation = true;
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

    /// Ставит на ствол насадку: прицел, глушитель, магазин, фонарик.
    ///
    /// Ствол должен быть у игрока: насадка без ствола — это насадка ни на чём,
    /// и молча запомнить её значило бы обещать то, чего не будет.
    ///
    /// Насадка, уже стоящая на этом стволе, вторично не заводится: игра держит
    /// по одной каждого вида, и список рос бы на каждую выдачу.
    ///
    /// false — игрока нет, ствола у него нет либо мест под насадки больше нет.
    virtual bool addWeaponComponent(shared::PlayerId id, std::uint32_t weapon,
                                    std::uint32_t component) = 0;

    /// Снимает насадку. false — игрока, ствола или насадки не было.
    virtual bool removeWeaponComponent(shared::PlayerId id, std::uint32_t weapon,
                                       std::uint32_t component) = 0;

    /// Красит ствол в один из заводских цветов игры.
    ///
    /// false — игрока нет либо ствола у него нет.
    virtual bool setWeaponTint(shared::PlayerId id, std::uint32_t weapon,
                               std::uint8_t tint) = 0;

    /// Сажает игрока в машину.
    ///
    /// Как перенос и перестановка машины — просьба, а не перемена: персонаж
    /// живёт в игре у своего хозяина, и посадить его может только она.
    /// Остальные узнают о посадке обычным снимком.
    ///
    /// seat — место: минус единица за руль, дальше по нумерации игры.
    ///
    /// false — игрока или машины уже нет.
    virtual bool setIntoVehicle(shared::PlayerId id, shared::VehicleId vehicle,
                                std::int8_t seat) = 0;

    /// Одевает игрока: слот одежды, вещь, расцветка, набор цветов.
    ///
    /// Уходит тем же каналом, что и вся внешность, и потому доходит и до самого
    /// игрока, и до всех вокруг. Надевает при этом не сервер, а игра владельца:
    /// тело живёт у неё.
    ///
    /// Слотов двенадцать, и что означает каждый, решает не сервер, а игра:
    /// одиннадцатый — верх, четвёртый — ноги, шестой — обувь. Номер вне этого
    /// предела — отказ: молча проглоченный, он выглядел бы как надетое, но
    /// невидимое.
    ///
    /// false — игрока уже нет либо слот назван неверно.
    virtual bool setClothes(shared::PlayerId id, std::uint8_t component, std::uint8_t drawable,
                            std::uint8_t texture, std::uint8_t palette) = 0;

    /// Надевает аксессуар: шляпу, очки, серьги, часы, браслет.
    ///
    /// drawable в минус единицу означает «снять»: так это и хранится в
    /// протоколе, и так же толкует его игра.
    virtual bool setProp(shared::PlayerId id, std::uint8_t index, std::int8_t drawable,
                         std::int8_t texture) = 0;

    /// Велит персонажу играть движение.
    ///
    /// Распоряжение, а не перемена состояния, и распоряжение это уходит не
    /// одному хозяину, а всем, кто игрока видит. Персонаж живёт в игре у
    /// хозяина, но показан он у каждого — куклой, которую ведут снимки; скажи
    /// мы одному хозяину, движение увидел бы он один.
    ///
    /// Движение спорит с задачами, которыми ведутся куклы: идущему оно
    /// достанется наполовину, потому что его тело в это время ведёт задача
    /// ходьбы. Стоящему — целиком, и это тот случай, ради которого движения и
    /// зовут.
    ///
    /// false — игрока уже нет.
    virtual bool playAnimation(shared::PlayerId id, const AnimationInfo& animation) = 0;

    /// Снимает с персонажа все задачи, включая начатое движение.
    virtual bool clearTasks(shared::PlayerId id) = 0;

    /// Переставляет игрока в другой слой мира.
    ///
    /// Из чужого слоя он пропадает у всех разом: сервер попросту перестаёт
    /// рассказывать о нём тем, кто его больше видеть не должен, и наоборот.
    /// false — игрока уже нет.
    virtual bool setDimension(shared::PlayerId id, std::int32_t dimension) = 0;

    /// Меняет модель персонажа.
    ///
    /// Уходит тем же каналом, что и остальная внешность, и потому доходит и до
    /// самого игрока, и до всех вокруг. Своего персонажа при этом переодевает
    /// не сервер, а его собственная игра: тело живёт у неё, и подменить модель
    /// может только она.
    virtual bool setModel(shared::PlayerId id, std::uint32_t model) = 0;

    /// Переносит игрока в точку.
    ///
    /// Единственное, чего сервер не делает у себя: персонаж живёт в игре у
    /// своего хозяина, и переставить его может только она. Отсюда и разница в
    /// поведении с остальным: здоровье меняется мгновенно, а перенос — просьба,
    /// исполняемая на той стороне.
    virtual bool teleport(shared::PlayerId id, const shared::Vec3& position) = 0;

    /// Выгоняет игрока из сессии.
    ///
    /// Причина уходит ему же: выгнанный без объяснения возвращается и пробует
    /// снова, а прочитавший «за оскорбления, на сутки» — не возвращается. Пустая
    /// причина допустима: не всякий отказ нужно объяснять.
    ///
    /// Разрыв не мгновенный, и рассчитывать на обратное нельзя. Сообщение о
    /// причине обязано уйти прежде, чем соединение закроется, поэтому закрытие
    /// откладывается до опустошения очереди отправки. До этого мига игрок
    /// остаётся в сессии и числится в реестре.
    virtual bool kick(shared::PlayerId id, std::string_view reason) = 0;

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

    /// Переставляет машину в другой слой мира. false — машины уже нет.
    virtual bool setVehicleDimension(shared::VehicleId id, std::int32_t dimension) = 0;

    /// Переставляет машину в точку.
    ///
    /// Как и перенос игрока, это просьба, а не перемена: машина живёт в игре у
    /// своего ведущего, и переставить её может только она. Поставь сервер новое
    /// положение у себя — ведущий вернул бы машину обратно ближайшим снимком,
    /// потому что у него она никуда не двигалась.
    ///
    /// У машины без ведущего просить некого, и тогда сервер ставит её у себя
    /// сам: снимков о ней никто не шлёт, и спорить с этим некому.
    ///
    /// false — машины уже нет.
    virtual bool teleportVehicle(shared::VehicleId id, const shared::Vec3& position,
                                 float heading) = 0;

    /// Чинит машину: кузов, двигатель, бак, стёкла, двери, колёса.
    ///
    /// Тоже просьба к ведущему, и по той же причине: вмятины и оторванные двери
    /// живут у него в игре. Машине без ведущего чинить нечего — её прочности
    /// сервер поправит у себя, а мять её было некому.
    ///
    /// false — машины уже нет.
    virtual bool repairVehicle(shared::VehicleId id) = 0;

    /// Как машина выглядит. Пусто — машины уже нет.
    ///
    /// Машина, о внешности которой никто ещё не говорил, описывается заводской:
    /// другого сервер о ней не знает, а отказать вопросу было бы неверно — вопрос
    /// законный, и ответ на него есть.
    [[nodiscard]] virtual std::optional<VehicleAppearanceInfo> vehicleAppearance(
        shared::VehicleId id) const = 0;

    /// Задаёт внешность машины целиком.
    ///
    /// Целиком, а не по полю, по той же причине, что и у метки: тюнинг ставят
    /// сразу помногу — покрасил, обул, навесил, — а поле за полем означало бы по
    /// сообщению на каждое, и получатель увидел бы машину собранной наполовину.
    ///
    /// С этого мгновения внешность машины принадлежит серверу, и объявления
    /// ведущего о ней больше не принимаются. Иначе вышло бы вот что: сервер
    /// назначил тюнинг, ведущий тем же тактом прислал снятое со своей игры — то
    /// есть ещё без тюнинга, — и назначенное пропало бы, не успев дойти. Терять
    /// при этом нечего: своей волей внешность машины игрок изменить не может, у
    /// клиента для этого нет правил игры.
    ///
    /// false — машины уже нет.
    virtual bool setVehicleAppearance(shared::VehicleId id,
                                      const VehicleAppearanceInfo& appearance) = 0;

    // --- Предметы --------------------------------------------------------------

    [[nodiscard]] virtual std::vector<ObjectInfo> objects() const = 0;
    [[nodiscard]] virtual std::optional<ObjectInfo> object(shared::ObjectId id) const = 0;

    [[nodiscard]] virtual shared::ObjectId createObject(std::uint32_t model,
                                                        const shared::Vec3& position,
                                                        const shared::Vec3& rotation) = 0;

    virtual bool removeObject(shared::ObjectId id) = 0;

    /// Переставляет предмет в другой слой мира. false — предмета уже нет.
    virtual bool setObjectDimension(shared::ObjectId id, std::int32_t dimension) = 0;

    // --- Прохожие --------------------------------------------------------------

    [[nodiscard]] virtual std::vector<PedInfo> peds() const = 0;
    [[nodiscard]] virtual std::optional<PedInfo> ped(shared::PedId id) const = 0;

    /// Ставит прохожего. kInvalidPedId — отказ: предел исчерпан или модель
    /// негодная.
    ///
    /// Номер в переданном описании не читается: его назначает сервер.
    [[nodiscard]] virtual shared::PedId createPed(const PedInfo& ped) = 0;

    /// Правит прохожего целиком: место, поворот, здоровье, броню, оружие.
    ///
    /// Целиком, а не по полю, по той же причине, что и у метки: ресурс правит
    /// куклу редко и сразу помногу, а поле за полем означало бы по сообщению на
    /// каждое.
    ///
    /// Модель здесь не меняется: смена модели — не правка, а новое тело.
    /// Названная другой, она молча не примется.
    ///
    /// false — прохожего уже нет.
    virtual bool updatePed(shared::PedId id, const PedInfo& ped) = 0;

    virtual bool removePed(shared::PedId id) = 0;

    /// Переставляет прохожего в другой слой мира. false — его уже нет.
    virtual bool setPedDimension(shared::PedId id, std::int32_t dimension) = 0;

    // --- Метки на карте --------------------------------------------------------

    [[nodiscard]] virtual std::vector<BlipInfo> blips() const = 0;
    [[nodiscard]] virtual std::optional<BlipInfo> blip(shared::BlipId id) const = 0;

    /// Ставит метку. kInvalidBlipId — отказ: предел исчерпан.
    ///
    /// Номер в переданном описании не читается: его назначает сервер.
    [[nodiscard]] virtual shared::BlipId createBlip(const BlipInfo& blip) = 0;

    /// Правит метку целиком: цвет, значок, подпись, место, слой мира.
    ///
    /// Целиком, а не по полю, и это не грубость. Ресурс правит метку редко и
    /// сразу помногу — покрасил, переименовал, подвинул, — а поле за полем
    /// означало бы по сообщению на каждое, и получатель увидел бы метку
    /// поправленной наполовину.
    ///
    /// false — метки уже нет.
    virtual bool updateBlip(shared::BlipId id, const BlipInfo& blip) = 0;

    virtual bool removeBlip(shared::BlipId id) = 0;

    // --- Нарисованное в мире ---------------------------------------------------
    //
    // Маркер и контрольная точка живут по тем же правилам, что и метка на
    // карте: заводятся, правятся целиком и убираются. Отличие одно, и оно на
    // стороне рисующего: у обоих есть своё поле видимости, и отбирает их по
    // расстоянию не сервер, а клиент.

    [[nodiscard]] virtual std::vector<MarkerInfo> markers() const = 0;
    [[nodiscard]] virtual std::optional<MarkerInfo> marker(shared::MarkerId id) const = 0;

    /// Ставит маркер. kInvalidMarkerId — отказ: предел исчерпан.
    [[nodiscard]] virtual shared::MarkerId createMarker(const MarkerInfo& marker) = 0;

    virtual bool updateMarker(shared::MarkerId id, const MarkerInfo& marker) = 0;
    virtual bool removeMarker(shared::MarkerId id) = 0;

    [[nodiscard]] virtual std::vector<CheckpointInfo> checkpoints() const = 0;
    [[nodiscard]] virtual std::optional<CheckpointInfo> checkpoint(
        shared::CheckpointId id) const = 0;

    /// Ставит контрольную точку. kInvalidCheckpointId — отказ: предел исчерпан.
    [[nodiscard]] virtual shared::CheckpointId createCheckpoint(
        const CheckpointInfo& checkpoint) = 0;

    virtual bool updateCheckpoint(shared::CheckpointId id, const CheckpointInfo& checkpoint) = 0;
    virtual bool removeCheckpoint(shared::CheckpointId id) = 0;

    // --- Привязка сущностей ----------------------------------------------------
    //
    // Привязка не принадлежит ни одному роду сущностей и потому живёт здесь, а
    // не среди машин или предметов: предмет вешают на человека, человека сажают
    // на предмет, предмет цепляют к машине.

    /// Привязывает сущность к другой.
    ///
    /// Состояние, а не распоряжение: сервер помнит его и рассказывает вошедшим
    /// позже — иначе шляпа, надетая час назад, лежала бы у них на земле.
    ///
    /// false — одной из двух сущностей нет, либо привязка бессмысленна: сущность
    /// вешают на саму себя или цепь замкнулась бы в кольцо. Кольцо игра ведёт
    /// пересчётом от родителя, а у замкнувшихся друг на друга родитель есть у
    /// каждого, и пересчёт не кончается никогда.
    virtual bool attachEntity(EntityRef entity, const AttachmentInfo& attachment) = 0;

    /// Отвязывает сущность. false — сущности нет либо она и не была привязана.
    virtual bool detachEntity(EntityRef entity) = 0;

    /// К чему сущность привязана. Пусто — ни к чему.
    [[nodiscard]] virtual std::optional<AttachmentInfo> attachment(EntityRef entity) const = 0;

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
