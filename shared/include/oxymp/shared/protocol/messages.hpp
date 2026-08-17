#pragma once

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/message_id.hpp>
#include <oxymp/shared/protocol/protocol_version.hpp>
#include <oxymp/shared/protocol/serialization.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oxymp::shared {

/// Предел длины имени игрока.
inline constexpr std::size_t kMaxNicknameLength = 24;

/// Предел длины строки чата.
inline constexpr std::size_t kMaxChatLength = 160;

/// Предел длины пароля сервера.
///
/// Здесь по той же причине, по которой здесь предел имени: длину проверяет
/// сервер, и число, по которому он это делает, обязано быть общим — иначе
/// клиент отправит то, что сервер молча отвергнет.
inline constexpr std::size_t kMaxPasswordLength = 64;

/// Идентификатор игрока, выданный сервером.
///
/// Это номер места на сервере, а не порядковый номер подключения: первый
/// вошедший получает ноль, второй — единицу, а освободившееся место достаётся
/// следующему вошедшему. Так же устроены идентификаторы у RAGE MP, и на то есть
/// причина помимо привычки: игрок называет себя этим числом в чате, и оно должно
/// быть коротким и повторяемым, а не расти до бесконечности за сессию.
///
/// Отсюда и значение «игрока нет»: ноль занят настоящим игроком, поэтому пустым
/// служит наибольшее возможное число.
using PlayerId = std::uint32_t;

inline constexpr PlayerId kInvalidPlayerId = 0xFFFFFFFFU;

struct ClientHello {
    static constexpr MessageId kId = MessageId::ClientHello;

    std::uint16_t protocolVersion = kProtocolVersion;
    std::string nickname;

    /// Пароль сервера, если он его спрашивает.
    ///
    /// Пусто у подавляющего большинства, и это нормальное положение вещей:
    /// сервер без пароля не смотрит на это поле вовсе. Идёт вместе с
    /// представлением, а не отдельным сообщением, потому что решение «пускать
    /// или нет» сервер принимает разом: пароль, имя и версия проверяются в
    /// одном месте и отвечают одним отказом.
    std::string password;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ClientHello read(ByteReader& reader);
};

struct ServerWelcome {
    static constexpr MessageId kId = MessageId::ServerWelcome;

    PlayerId playerId = kInvalidPlayerId;
    Vec3 spawnPosition;
    std::uint16_t tickRate = kDefaultTickRate;

    /// Как сервер себя называет.
    ///
    /// Приходит в приветствии, а не спрашивается отдельно: имя нужно ровно
    /// тому, кто уже вошёл, и лишнего разговора ради одной строки не стоит.
    /// Меню показывает его вместо адреса — так же, как alt:V.
    std::string name;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ServerWelcome read(ByteReader& reader);
};

struct ServerReject {
    static constexpr MessageId kId = MessageId::ServerReject;

    RejectReason reason = RejectReason::ProtocolMismatch;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ServerReject read(ByteReader& reader);
};

struct Ping {
    static constexpr MessageId kId = MessageId::Ping;

    /// Отметка времени отправителя. Сервер возвращает её без изменений, поэтому
    /// её смысл известен только отправителю и часы сторон сверять не нужно.
    std::uint64_t timestampMs = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static Ping read(ByteReader& reader);
};

struct Pong {
    static constexpr MessageId kId = MessageId::Pong;

    std::uint64_t timestampMs = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static Pong read(ByteReader& reader);
};

/// Сколько у персонажа слотов одежды в нумерации игры.
///
/// Двенадцать: голова, маска, волосы, торс, ноги, сумка, обувь, шея, броня,
/// значок, рубашка и второй торс. Числа эти — не наши, а игры: она принимает их
/// как есть, и переставлять их местами нельзя.
inline constexpr std::size_t kPedComponentCount = 12;

/// Сколько у персонажа слотов аксессуаров.
///
/// В игре их восемь, но заняты только пять — шляпа, очки, серьги, часы и
/// браслет, — и передаются именно они. Пустые слоты между ними передавать
/// незачем: игра сама знает, что в них ничего нет.
inline constexpr std::size_t kPedPropCount = 5;

/// Сколько у лица слоёв: щетина, брови, старение, макияж, румяна, веснушки и
/// прочее, чем игрок правит внешность в редакторе.
inline constexpr std::size_t kPedOverlayCount = 13;

/// Один слот одежды.
struct PedComponent {
    /// Какая вещь надета. Ноль — то, что игра надевает по умолчанию.
    std::uint8_t drawable = 0;

    /// Какой её расцветки.
    std::uint8_t texture = 0;

    /// Из какого набора цветов взята расцветка. Почти всегда ноль.
    std::uint8_t palette = 0;

    [[nodiscard]] friend bool operator==(const PedComponent&, const PedComponent&) = default;
};

/// Один аксессуар. Минус единица означает «ничего не надето».
struct PedProp {
    std::int8_t drawable = -1;
    std::int8_t texture = 0;

    [[nodiscard]] friend bool operator==(const PedProp&, const PedProp&) = default;
};

/// Один слой лица.
struct PedOverlay {
    /// Какой именно слой выбран. 255 — ничего.
    std::uint8_t index = 255;

    /// Откуда берётся цвет: 0 — из цветов волос, 1 — из цветов помады, 2 — цвета
    /// у слоя нет вовсе.
    std::uint8_t colourType = 2;

    std::uint8_t colour = 0;
    std::uint8_t secondColour = 0;

    /// Насколько слой заметен, от нуля до единицы.
    float opacity = 1.0F;

    [[nodiscard]] friend bool operator==(const PedOverlay&, const PedOverlay&) = default;
};

/// Как игрок выглядит.
///
/// Состав полей взят у alt:V поле в поле — иначе внешность, собранная в его
/// редакторе, у нас разошлась бы с оригиналом на мелочах, которые как раз и
/// делают лицо чужим.
///
/// Ходит по надёжному каналу и редко: внешность меняется раз в час, а дойти
/// обязана целиком. В снимке ей не место — снимок вправе потеряться.
struct PlayerAppearance {
    static constexpr MessageId kId = MessageId::PlayerAppearance;

    /// Чей это вид. В сообщении от клиента не заполняется: сервер знает, с
    /// какого соединения пришёл пакет, и проставляет идентификатор сам.
    PlayerId playerId = kInvalidPlayerId;

    /// Хеш модели персонажа. Ноль — оставить ту, что есть.
    ///
    /// Обычно это `mp_m_freemode_01` или `mp_f_freemode_01`, но не обязательно:
    /// сервер вправе выдать игроку любую модель игры, и тогда всё остальное к
    /// ней просто не применяется — у сюжетных моделей одежда своя.
    std::uint32_t model = 0;

    std::array<PedComponent, kPedComponentCount> components{};
    std::array<PedProp, kPedPropCount> props{};

    /// Лицо: смешение двух родителей и третьего вклада.
    ///
    /// Так устроен редактор внешности в GTA Online: лицо не выбирается из
    /// списка, а собирается из двух родительских и одного добавочного, и у
    /// формы с кожей доли смешения свои.
    std::uint8_t shapeFirst = 0;
    std::uint8_t shapeSecond = 0;
    std::uint8_t shapeThird = 0;
    std::uint8_t skinFirst = 0;
    std::uint8_t skinSecond = 0;
    std::uint8_t skinThird = 0;
    float shapeMix = 0.5F;
    float skinMix = 0.5F;
    float thirdMix = 0.0F;

    std::array<PedOverlay, kPedOverlayCount> overlays{};

    /// Цвет волос и цвет мелирования.
    std::uint8_t hairColour = 0;
    std::uint8_t hairHighlight = 0;

    /// Цвет глаз.
    std::uint8_t eyeColour = 0;

    /// Сравнение нужно затем, чтобы объявлять внешность только при изменении.
    /// Читать её у игры дёшево, а слать надёжным каналом двадцать раз в секунду
    /// — нет.
    [[nodiscard]] friend bool operator==(const PlayerAppearance&,
                                         const PlayerAppearance&) = default;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerAppearance read(ByteReader& reader);
};

struct PlayerJoined {
    static constexpr MessageId kId = MessageId::PlayerJoined;

    PlayerId playerId = kInvalidPlayerId;
    std::string nickname;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerJoined read(ByteReader& reader);
};

struct PlayerLeft {
    static constexpr MessageId kId = MessageId::PlayerLeft;

    PlayerId playerId = kInvalidPlayerId;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerLeft read(ByteReader& reader);
};

/// Чем игрок занят помимо перемещения.
///
/// Битами, а не отдельными полями: признаков много, каждый занимает один бит, и
/// снимок уходит по сети двадцать раз в секунду от каждого игрока.
///
/// Здесь перечислено то, что у получателя отыгрывается позой или задачей игры, а
/// не выводится из положения и скорости. Правило отбора именно такое: если
/// получатель может догадаться сам — признака здесь нет. Бег, например,
/// выводится из скорости, а плавание — нет: плывущий и идущий по грудь в воде
/// движутся одинаково, а выглядят по-разному.
///
/// Номера битов закреплены навсегда, как и номера сообщений: занятый однажды бит
/// не переиспользуется, даже если признак уберут.
enum class PlayerFlag : std::uint32_t {
    /// Игрок мёртв.
    Dead = 1U << 0U,

    /// Целится из оружия.
    Aiming = 1U << 1U,

    /// Стреляет прямо сейчас.
    Shooting = 1U << 2U,

    /// Тело обмякло: сбит машиной, упал, оглушён.
    Ragdoll = 1U << 3U,

    /// В прыжке.
    Jumping = 1U << 4U,

    /// Сидит в машине. Какая именно — в полях vehicleId и seat.
    InVehicle = 1U << 5U,

    /// Крадётся пригнувшись.
    Crouching = 1U << 6U,

    /// Лезет вверх по краю.
    Climbing = 1U << 7U,

    /// Перемахивает через препятствие.
    Vaulting = 1U << 8U,

    /// Плывёт по поверхности.
    Swimming = 1U << 9U,

    /// Под водой.
    Diving = 1U << 10U,

    /// Падает: шагнул с высоты и ещё не приземлился.
    Falling = 1U << 11U,

    /// Под куполом парашюта.
    Parachuting = 1U << 12U,

    /// Перезаряжает оружие.
    Reloading = 1U << 13U,

    /// Сидит в укрытии.
    InCover = 1U << 14U,

    /// Дерётся врукопашную.
    ///
    /// Признак говорит «дерётся», а какой именно удар — в поле action: удар
    /// длится доли секунды и мимо снимка проскочил бы незамеченным, если бы
    /// приходилось ловить его переход из нуля в единицу.
    Melee = 1U << 15U,

    /// Поднимается после падения.
    GettingUp = 1U << 16U,

    /// Стреляет из машины.
    DriveBy = 1U << 17U,

    /// Залезает в машину прямо сейчас.
    ///
    /// Отделено от InVehicle намеренно, и в этом весь смысл: пока признак стоит,
    /// получатель проигрывает вход — открывание двери и посадку, — а не
    /// переставляет персонажа на сиденье. Куда именно он лезет, сказано в тех же
    /// полях vehicleId и seat.
    EnteringVehicle = 1U << 18U,
};

[[nodiscard]] constexpr std::uint32_t operator|(PlayerFlag left, PlayerFlag right) noexcept {
    return static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right);
}

[[nodiscard]] constexpr bool has(std::uint32_t flags, PlayerFlag flag) noexcept {
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

/// Короткое движение, которое иначе проскочило бы между снимками.
///
/// Признаки в PlayerFlag описывают состояния — то, что длится и потому попадает
/// хотя бы в один снимок. Удар не таков: он длится доли секунды, и снимок застаёт
/// его в лучшем случае раз. Поэтому отправитель не спрашивает игру «бьёт ли он
/// сейчас», а замечает само начало удара и объявляет его здесь; получатель
/// проигрывает движение целиком, не дожидаясь следующих снимков.
enum class PedAction : std::uint8_t {
    /// Ничего сверх обычного.
    None = 0,

    /// Быстрый удар рукой.
    LightPunch = 1,

    /// Тяжёлый удар рукой.
    HeavyPunch = 2,

    /// Удар ногой.
    Kick = 3,
};

/// Место водителя. Пассажирские места нумеруются с нуля — так же, как в игре.
inline constexpr std::int8_t kDriverSeat = -1;

/// Место, которого нет. Нужно тому, кто ни в какой машине не сидит.
inline constexpr std::int8_t kNoSeat = -2;

/// Номер машины в сессии.
///
/// Номера выдаёт сервер, и это стоило двух перемен подряд — обе вынужденные.
///
/// Сперва машина числилась за своим водителем и называлась его именем. Стоило
/// водителю выйти — и машины не стало вместе с ним, вместе с сидящими в ней
/// пассажирами; пересесть с места на место было нельзя, потому что в промежутке
/// машина никому не принадлежала.
///
/// Тогда номер стали собирать из номера создателя и его счётчика: машина
/// зажила отдельно от седоков, и двое не могли выдать один и тот же номер. Но
/// выдавал его всё ещё клиент, а значит машина существовала ровно столько,
/// сколько её кто-то рассылал. Брошенная у обочины, она пропадала у всех через
/// несколько секунд, а вошедший позже не узнавал о ней вовсе: рассказать о
/// машине было некому — сервер её и не знал.
///
/// Теперь список машин ведёт сервер. Номер выдаёт он, машина живёт, пока он её
/// не уберёт, и вошедшему он пересказывает всё, что в сессии уже есть. Кто
/// именно считает физику машины — вопрос отдельный: это ведущий, и назначает
/// его тоже сервер (см. VehicleAuthority).
using VehicleId = std::uint32_t;

/// Машины нет. Ноль занят намеренно: счётчик сервера начинается с единицы.
inline constexpr VehicleId kInvalidVehicleId = 0;

/// Когда снимок был снят, по часам отправителя.
///
/// Миллисекунды, и только для сравнения с другой такой же отметкой того же
/// отправителя: часы сторон никто не сверяет, и сверять незачем. Получателю
/// нужна не дата, а промежуток между двумя снимками — а он одинаков на любых
/// монотонных часах.
///
/// Без этой отметки промежуток приходилось брать по времени прихода, и это было
/// главной причиной дрожания. Снимки уходят ровно двадцать раз в секунду, а
/// приходят как придётся: то через сорок миллисекунд, то через семьдесят. Взяв
/// разницу прихода за длину промежутка, получатель растягивал и сжимал само
/// движение — машина то замедлялась, то дёргалась вперёд, хотя ехала ровно.
///
/// Тридцати двух бит хватает на сорок девять суток непрерывной игры, после чего
/// счётчик пойдёт по кругу. Разница беззнаковых чисел переживает этот круг сама
/// собой, если считать её беззнаковой же, — см. elapsedSince.
using Timestamp = std::uint32_t;

/// Сколько прошло между двумя отметками, в миллисекундах.
///
/// Беззнаковое вычитание намеренно: на переходе счётчика через край обычное
/// вычитание дало бы четыре миллиарда, а это — единственный способ получить
/// верный ответ, не зная, был ли переход.
[[nodiscard]] constexpr std::uint32_t elapsedSince(Timestamp earlier, Timestamp later) noexcept {
    return later - earlier;
}

/// Снимок состояния игрока в мире.
///
/// Ходит по ненадёжному каналу и часто: потерянный снимок дешевле заменить
/// следующим, чем переотправлять устаревший.
struct PlayerState {
    static constexpr MessageId kId = MessageId::PlayerState;

    /// Когда снят, по часам отправителя. Проставляется при отправке.
    Timestamp sentAt = 0;

    /// Чей это снимок.
    ///
    /// В сообщении от клиента не заполняется: сервер знает, с какого соединения
    /// пришёл пакет, и проставляет идентификатор сам. Верить здесь клиенту
    /// нельзя — иначе он сможет выдать себя за другого.
    PlayerId playerId = kInvalidPlayerId;

    Vec3 position;

    /// Направление взгляда в градусах.
    float heading = 0.0F;

    /// Скорость. Нужна, чтобы получатель мог достроить движение между снимками,
    /// а не дёргать модель от точки к точке.
    Vec3 velocity;

    std::uint16_t health = 200;
    std::uint16_t armour = 0;

    /// Набор PlayerFlag.
    std::uint32_t flags = 0;

    /// Хеш оружия в руках. Ноль — безоружен.
    std::uint32_t weapon = 0;

    /// Сколько патронов осталось в этом оружии.
    ///
    /// Передаётся затем, что получатель выдаёт кукле то же самое оружие, и без
    /// числа патронов выдавал бы его с полным магазином. Разница видна: чужой
    /// игрок перезаряжался бы у себя, а у нас продолжал стрелять без остановки,
    /// и обмен выстрелами выглядел бы у двоих по-разному.
    std::uint16_t ammo = 0;

    /// Куда направлено оружие. Имеет смысл только при Aiming или Shooting.
    ///
    /// Точка, а не направление: получатель ставит по ней задачу прицеливания, а
    /// той нужны координаты в мире.
    Vec3 aimAt;

    /// В какой машине игрок сидит или в какую залезает.
    ///
    /// Номер машины, а не её водителя: машина живёт отдельно от того, кто ею
    /// правит, и пассажир не теряет её, когда водитель вышел.
    VehicleId vehicleId = kInvalidVehicleId;

    /// Место в машине: -1 водитель, 0 и дальше — пассажирские, -2 — ни в какой.
    std::int8_t seat = kNoSeat;

    /// Короткое движение, начавшееся к этому мгновению.
    PedAction action = PedAction::None;

    /// Порядковый номер движения.
    ///
    /// Нужен затем, что снимок с одним и тем же ударом приходит несколько раз
    /// подряд — снимки шлются чаще, чем длится удар. Без номера получатель не
    /// отличил бы «всё ещё тот же удар» от «ударил снова» и запускал бы движение
    /// с начала каждые пятьдесят миллисекунд.
    std::uint8_t actionSequence = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerState read(ByteReader& reader);
};

/// Сколько снимков влезает в одну связку.
///
/// Предел не от жадности: длина пишется одним байтом, и больше в неё просто не
/// поместится. Двести пятьдесят пять человек вокруг одного — цифра, до которой
/// не доходит ни один игровой режим, а если дойдёт, лишние уедут следующим
/// тактом.
inline constexpr std::size_t kMaxStatesInBundle = 255;

/// Снимки всех, кто рядом, одной посылкой.
///
/// Только от сервера к клиенту. Обратно клиент шлёт свой единственный снимок как
/// прежде: складывать ему нечего.
struct PlayerStates {
    static constexpr MessageId kId = MessageId::PlayerStates;

    std::vector<PlayerState> players;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerStates read(ByteReader& reader);
};

/// Что у машины включено и выключено.
enum class VehicleFlag : std::uint16_t {
    /// Двигатель заведён.
    EngineOn = 1U << 0U,

    /// Ручной тормоз затянут.
    Handbrake = 1U << 1U,

    /// Фары горят.
    LightsOn = 1U << 2U,

    /// Дальний свет.
    HighBeams = 1U << 3U,

    /// Сирена включена.
    SirenOn = 1U << 4U,
};

[[nodiscard]] constexpr std::uint16_t operator|(VehicleFlag left, VehicleFlag right) noexcept {
    return static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right);
}

[[nodiscard]] constexpr bool has(std::uint16_t flags, VehicleFlag flag) noexcept {
    return (flags & static_cast<std::uint16_t>(flag)) != 0;
}

/// Сколько у машины дверей в нумерации игры: две передние, две задние, капот и
/// багажник.
inline constexpr int kVehicleDoorCount = 6;

/// Сколько у машины стёкол и колёс в нумерации игры.
inline constexpr int kVehicleWindowCount = 8;
inline constexpr int kVehicleWheelCount = 8;

/// Снимок машины на текущее мгновение.
///
/// Рассылает его ведущий машины — один-единственный клиент, назначенный
/// сервером. Раньше ведущего вычисляли на месте, по правилу «кто занял младшее
/// место», и правило это было верным ровно до тех пор, пока в машине кто-то
/// сидел. Пустую машину вести было некому — и она пропадала. Теперь ведущего
/// назначает сервер, и пустая машина ведущего не теряет (см. VehicleAuthority).
///
/// Отправителя здесь нет намеренно, хотя раньше он был. Ведущий у машины один,
/// и сервер принимает снимок только от него; повторять его имя в каждом снимке
/// двадцать раз в секунду значило бы возить по сети то, что получателю и так
/// сказано отдельным сообщением — надёжным, в отличие от снимка.
///
/// Здесь только то, что меняется на ходу. Цвет, номер и тюнинг идут отдельным
/// сообщением: слать их двадцать раз в секунду значило бы возить одно и то же.
struct VehicleState {
    static constexpr MessageId kId = MessageId::VehicleState;

    /// Номер машины, выданный сервером.
    VehicleId id = kInvalidVehicleId;

    /// Когда снят, по часам ведущего. Проставляется при отправке.
    ///
    /// Часы ведущего, а не сервера, и это важно при смене ведущего: отметки
    /// двух разных клиентов между собой несравнимы. Получатель об этом знает и
    /// начинает отсчёт заново — см. SessionVehicle.
    Timestamp sentAt = 0;

    /// Хеш модели. По нему получатель закажет и создаст такую же.
    ///
    /// Лежит здесь, а не во внешности, хотя и не меняется: без модели машину не
    /// создать, а внешность приходит отдельно и может опоздать.
    std::uint32_t model = 0;

    Vec3 position;

    /// Поворот по трём осям в градусах, порядок игры (2).
    Vec3 rotation;

    Vec3 velocity;

    /// Угловая скорость, радианы в секунду по осям мира.
    ///
    /// Без неё поворот приходится доводить рывками между снимками: машина в
    /// заносе разворачивается за десятые доли секунды, и двадцати снимков в
    /// секунду на это не хватает.
    Vec3 angularVelocity;

    /// Куда повёрнут руль, от -1 до 1.
    ///
    /// Не угол колёс, а ввод водителя: угла колёс игра наружу не отдаёт, а вот
    /// поворот руля — ровно то число, из которого она этот угол и получает.
    float steer = 0.0F;

    /// Насколько выжат газ и насколько тормоз, от нуля до единицы.
    float throttle = 0.0F;
    float brake = 0.0F;

    /// Прочности: кузова, двигателя и бака. Тысяча — целое.
    std::uint16_t bodyHealth = 1000;
    std::uint16_t engineHealth = 1000;
    std::uint16_t tankHealth = 1000;

    /// Набор VehicleFlag.
    std::uint16_t flags = 0;

    /// Какие двери открыты и какие оторваны, по биту на дверь.
    std::uint8_t doorsOpen = 0;
    std::uint8_t doorsBroken = 0;

    /// Какие стёкла выбиты и какие колёса пробиты, по биту на каждое.
    std::uint8_t windowsBroken = 0;
    std::uint8_t tyresBurst = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static VehicleState read(ByteReader& reader);
};

/// Предел длины номерного знака. Восемь знаков — столько же, сколько у игры.
inline constexpr std::size_t kMaxPlateLength = 8;

/// Сколько у машины мест под тюнинг в нумерации игры.
///
/// Считаются все, включая те, которых у конкретной модели нет: получателю
/// достаточно поставить в такое место «как было», а разбираться, какие места
/// осмысленны для этой модели, — дело игры.
inline constexpr int kVehicleModSlots = 49;

/// Место тюнинга без изменений — то есть заводское.
inline constexpr std::int8_t kStockMod = -1;

/// Заводской набор тюнинга: во всех местах «как было».
///
/// Пустого набора здесь мало: нулём в нумерации игры обозначено первое
/// изменение, а не отсутствие изменений, и машина по умолчанию оказалась бы
/// собранной из первых попавшихся деталей.
[[nodiscard]] constexpr std::array<std::int8_t, kVehicleModSlots> stockMods() noexcept {
    std::array<std::int8_t, kVehicleModSlots> mods{};
    mods.fill(kStockMod);
    return mods;
}

/// Как выглядит машина.
///
/// Отдельным сообщением и по надёжному каналу, потому что живёт по другим
/// правилам: меняется редко, а дойти обязано — не дошедший цвет не исправится
/// следующим снимком, он останется неверным навсегда.
///
/// Сервер помнит последнюю внешность каждой машины и пересказывает её тем, кто
/// вошёл позже: иначе вошедший увидел бы чужую машину заводского вида и никогда
/// бы не узнал, что она другая.
struct VehicleAppearance {
    static constexpr MessageId kId = MessageId::VehicleAppearance;

    VehicleId id = kInvalidVehicleId;

    /// Цвета в нумерации игры: основной, второй, перламутр и колёса.
    std::uint8_t primaryColour = 0;
    std::uint8_t secondaryColour = 0;
    std::uint8_t pearlescentColour = 0;
    std::uint8_t wheelColour = 0;

    /// Что написано на номерном знаке и какого он вида.
    std::string plate;
    std::uint8_t plateStyle = 0;

    /// Раскраска, тип дисков и тонировка. Отрицательное — ничего не выбрано.
    std::int8_t livery = kStockMod;
    std::int8_t wheelType = kStockMod;
    std::int8_t windowTint = kStockMod;

    /// Насколько машина грязная, от нуля до пятнадцати.
    float dirtLevel = 0.0F;

    /// Что стоит в каждом месте тюнинга. kStockMod — заводское.
    std::array<std::int8_t, kVehicleModSlots> mods = stockMods();

    /// Какие места тюнинга включены переключателем, а не выбором из списка:
    /// турбина, ксенон, дым из-под колёс. По биту на место.
    std::uint32_t toggleMods = 0;

    /// Сравнение по всем полям.
    ///
    /// Нужно отправителю: внешность уходит по надёжному каналу и только при
    /// изменении, а узнать об изменении можно только сравнив с тем, что ушло в
    /// прошлый раз.
    [[nodiscard]] bool operator==(const VehicleAppearance& other) const = default;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static VehicleAppearance read(ByteReader& reader);
};

/// В сессии появилась машина.
///
/// Несёт полное состояние, а не один номер, и это существенно: получатель
/// обязан суметь показать машину немедленно, не дожидаясь первого снимка.
/// Вошедшему в сессию таких сообщений приходит столько, сколько в ней машин, — и
/// он видит мир целиком с первого кадра, а не собирает его по мере того, как
/// мимо проезжают.
struct VehicleAdded {
    static constexpr MessageId kId = MessageId::VehicleAdded;

    /// Где машина стоит и что с ней происходит на это мгновение.
    VehicleState state;

    /// Кто её ведёт. kInvalidPlayerId — никто: машина стоит.
    PlayerId owner = kInvalidPlayerId;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static VehicleAdded read(ByteReader& reader);
};

/// Машины больше нет.
struct VehicleRemoved {
    static constexpr MessageId kId = MessageId::VehicleRemoved;

    VehicleId id = kInvalidVehicleId;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static VehicleRemoved read(ByteReader& reader);
};

/// Машину отныне ведёт другой.
///
/// Ведущий — тот, у кого машина живёт по-настоящему: считается её физика,
/// крутятся колёса, снимаются снимки. У остальных она кукла, которую двигают по
/// этим снимкам.
///
/// Смена ведущего — обычное дело, а не исключение. Сел за руль — забрал машину
/// себе; вышел из сессии — сервер отдал её тому, кто ближе; отошёл далеко от
/// брошенной машины — её подхватил сосед. Оттого сообщение и отдельное: оно
/// обязано дойти, а снимки идут по ненадёжному каналу.
struct VehicleAuthority {
    static constexpr MessageId kId = MessageId::VehicleAuthority;

    VehicleId id = kInvalidVehicleId;

    /// Новый ведущий. kInvalidPlayerId — машину не ведёт никто.
    ///
    /// «Никто» — не поломка, а нормальное положение вещей: машина, брошенная
    /// вдали от всех, стоит там, где её оставили, и считать её физику незачем.
    /// Её состояние помнит сервер и перескажет каждому, кто подъедет.
    PlayerId owner = kInvalidPlayerId;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static VehicleAuthority read(ByteReader& reader);
};

/// Предел длины названия погоды.
///
/// Названия у игры короткие и заданные — EXTRASUNNY, THUNDER и ещё десяток.
/// Предел здесь не от них, а от испорченного пакета: строка длиной в мегабайт
/// не должна заставлять получателя её выделять.
inline constexpr std::size_t kMaxWeatherLength = 24;

/// Какая в сессии погода и который час.
///
/// Названием погоды, а не её номером, и это не небрежность: игра принимает
/// именно название, а превратить название в номер умеет только она сама —
/// сервер игровых файлов не имеет и хешей считать не может.
///
/// Часы идут на сервере и рассылаются раз в игровую минуту. Между рассылками
/// они у клиента стоят, и это верно: игровые часы всё равно тикают минутами, а
/// идущие сами по себе разошлись бы у всех по-разному.
struct WorldState {
    static constexpr MessageId kId = MessageId::WorldState;

    /// Название погоды в нумерации игры, например EXTRASUNNY.
    std::string weather;

    std::uint8_t hour = 12;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static WorldState read(ByteReader& reader);
};

/// Одно оружие в снаряжении игрока.
struct WeaponSlot {
    std::uint32_t weapon = 0;
    std::uint16_t ammo = 0;

    [[nodiscard]] bool operator==(const WeaponSlot& other) const = default;
};

/// Сколько оружия сервер вправе выдать одному игроку.
///
/// Предел нужен разбору, а не хозяину сервера: без него испорченный пакет с
/// огромным числом в поле длины заставил бы клиент выделять память под список,
/// которого нет. Полусотни хватает с запасом — всего оружия в игре меньше.
inline constexpr std::uint16_t kMaxWeaponSlots = 64;

/// Чем игрок вооружён.
///
/// Уходит только владельцу и только от сервера. Остальным этот список не нужен:
/// они видят, что у человека в руках, из его же снимка, а знать, что лежит у
/// него в карманах, им незачем.
///
/// Список полный, а не разницей. Разница требует, чтобы обе стороны считали
/// одинаково и ничего не потеряли по дороге, — а снаряжение меняется считанные
/// разы за сессию, и возить его целиком не жалко.
///
/// Патроны в этом списке — те, что сервер выдал, а не те, что остались. Тратит
/// их игра у владельца, и сервер узнаёт об этом из его снимков; хранит он их для
/// другого — чтобы вернуть человеку то же снаряжение после смерти.
struct PlayerLoadout {
    static constexpr MessageId kId = MessageId::PlayerLoadout;

    std::vector<WeaponSlot> weapons;

    /// Убрать ли имеющееся оружие перед выдачей.
    ///
    /// Нужно затем, что «выдать» и «выдать вместо всего» — разные действия, а
    /// список у них одинаковый. Без признака пришлось бы слать пустой список и
    /// следом настоящий, и между ними игрок оказывался бы безоружным.
    bool replace = false;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerLoadout read(ByteReader& reader);
};

/// Сколько у игрока здоровья и брони по мнению сервера.
///
/// По мнению сервера — единственному, которое считается. Раньше здоровье вёл сам
/// игрок: стрелявший сообщал об уроне, жертва применяла его к себе. Жертва при
/// этом была единственным источником правды о собственной жизни, и потерянный
/// по дороге урон просто не случался.
///
/// Уходит только владельцу: остальные видят его здоровье из его же снимка, а
/// снимок сервер и без того правит на своё значение перед рассылкой.
struct HealthChanged {
    static constexpr MessageId kId = MessageId::HealthChanged;

    std::uint16_t health = 200;
    std::uint16_t armour = 0;

    /// Кто это сделал. kInvalidPlayerId — никто: лечение, появление, падение.
    ///
    /// Нужен не для урона, а для строки на экране: молча потерявший половину
    /// здоровья игрок решит, что игра сломалась.
    PlayerId attacker = kInvalidPlayerId;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static HealthChanged read(ByteReader& reader);
};

/// Номер предмета в сессии.
///
/// Устроен и выдаётся так же, как номер машины, и по тем же причинам: список
/// ведёт сервер, номера не переиспользуются, ноль означает «предмета нет».
using ObjectId = std::uint32_t;

inline constexpr ObjectId kInvalidObjectId = 0;

/// В сессии появился предмет.
///
/// Предметы, в отличие от машин, ведущего не имеют. Они стоят там, где их
/// поставили, и физику им считать некому и незачем: подвинуть предмет может
/// только распорядитель, и подвинет он его у всех разом, через сервер.
///
/// Отсюда и состав: ни скорости, ни прочности, ни снимков на ходу. Всё, что о
/// предмете нужно знать, помещается в это одно сообщение и больше не меняется.
struct ObjectAdded {
    static constexpr MessageId kId = MessageId::ObjectAdded;

    ObjectId id = kInvalidObjectId;
    std::uint32_t model = 0;

    Vec3 position;

    /// Поворот по трём осям в градусах, порядок игры (2).
    Vec3 rotation;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ObjectAdded read(ByteReader& reader);
};

/// Предмета больше нет — или он ушёл из виду.
struct ObjectRemoved {
    static constexpr MessageId kId = MessageId::ObjectRemoved;

    ObjectId id = kInvalidObjectId;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ObjectRemoved read(ByteReader& reader);
};

/// Реплика игрока в чат.
struct ChatSay {
    static constexpr MessageId kId = MessageId::ChatSay;

    std::string text;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ChatSay read(ByteReader& reader);
};

/// Строка чата, разосланная сервером.
struct ChatLine {
    static constexpr MessageId kId = MessageId::ChatLine;

    ChatKind kind = ChatKind::Say;

    /// Кто это сказал. kInvalidPlayerId — сервер.
    PlayerId playerId = kInvalidPlayerId;

    /// Имя автора. Приходит вместе со строкой, а не берётся из списка игроков:
    /// сообщение о выходе иначе оказалось бы без имени — игрока уже нет.
    std::string nickname;

    std::string text;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ChatLine read(ByteReader& reader);
};

/// Клиент сообщает, что попал по чужому игроку.
struct DamageReport {
    static constexpr MessageId kId = MessageId::DamageReport;

    PlayerId victim = kInvalidPlayerId;
    std::uint16_t amount = 0;
    std::uint32_t weapon = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static DamageReport read(ByteReader& reader);
};

/// Сервер сообщает игроку, что по нему попали.
struct DamageTaken {
    static constexpr MessageId kId = MessageId::DamageTaken;

    PlayerId attacker = kInvalidPlayerId;
    std::uint16_t amount = 0;
    std::uint32_t weapon = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static DamageTaken read(ByteReader& reader);
};

/// Сервер переносит игрока в точку.
///
/// Единственное распоряжение, которое сервер вынужден передавать клиенту вместо
/// того, чтобы сделать самому: персонаж живёт в игре у своего хозяина, и
/// переставить его может только она. Всё прочее — здоровье, оружие, машины,
/// погода — сервер меняет у себя и рассылает готовым.
///
/// Распорядителя здесь нет намеренно. Раньше это было «распоряжение админ-меню»
/// и несло имя того, кто распорядился; теперь переносить игрока волен всякий
/// ресурс сервера, и объяснять игроку, кто именно, — дело того же ресурса, у
/// него для этого есть чат.
struct PlayerTeleport {
    static constexpr MessageId kId = MessageId::PlayerTeleport;

    Vec3 position;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static PlayerTeleport read(ByteReader& reader);
};

/// Предел длины имени события.
///
/// Нужен разбору, а не хозяину сервера: без него испорченный пакет с огромным
/// числом в поле длины заставил бы получателя выделять память под строку,
/// которой нет.
inline constexpr std::size_t kMaxEventNameLength = 64;

/// Предел длины полезной нагрузки. Четырёх килобайт хватает описанию меню
/// целиком; ею ходит разметка, и обычные двести пятьдесят шесть байт ей малы.
inline constexpr std::size_t kMaxEventPayloadLength = kMaxTextLength;

/// Именованное событие от клиента серверу.
///
/// Всё, что не описано протоколом: нажат пункт меню, отправлена форма, случилось
/// что-то в странице интерфейса. Имя и нагрузка — строки, и толкует их не
/// клиент, а ресурс сервера.
///
/// Нагрузка не разобрана намеренно. Заведи мы здесь поля — их пришлось бы
/// заводить под каждый игровой режим, то есть менять протокол всякий раз, когда
/// у кого-то в меню появляется новый пункт. Строкой обходится и alt:V, и
/// RAGE MP, и по той же причине.
struct ClientEvent {
    static constexpr MessageId kId = MessageId::ClientEvent;

    std::string name;
    std::string payload;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ClientEvent read(ByteReader& reader);
};

/// Именованное событие от сервера клиенту.
struct ServerEvent {
    static constexpr MessageId kId = MessageId::ServerEvent;

    std::string name;
    std::string payload;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ServerEvent read(ByteReader& reader);
};

/// Сколько у игрока денег.
///
/// Шлётся при входе и при каждом изменении. Целиком, а не разницей: разница
/// требует, чтобы обе стороны считали одинаково и ничего не потеряли по дороге,
/// а деньги — не то, где стоит на это полагаться.
///
/// Со знаком: долг в игре — обычное дело, а беззнаковое число превратило бы его
/// в неправдоподобное богатство.
struct MoneyChanged {
    static constexpr MessageId kId = MessageId::MoneyChanged;

    std::int64_t amount = 0;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static MoneyChanged read(ByteReader& reader);
};

/// Сколько ресурсов сервер вправе объявить за раз.
///
/// Предел нужен разбору, а не хозяину сервера: без него испорченный пакет с
/// огромным числом в поле длины заставил бы клиент выделять память под список,
/// которого нет.
inline constexpr std::uint16_t kMaxResources = 512;

/// Один раздаваемый ресурс.
struct ResourceEntry {
    /// Исходное имя — то, как файл называется у хозяина сервера.
    std::string name;

    /// Отпечаток содержимого. По нему ресурс качается и им же называется в кеше.
    std::string hash;

    /// Размер, чтобы показать человеку, сколько качать.
    std::uint64_t size = 0;

    /// Этот файл — страница интерфейса, которую клиент грузит в CEF.
    ///
    /// Так альтв и рейджмп раздают интерфейс: страница живёт на сервере, а не в
    /// сборке клиента. Клиенту нужно знать, какой из скачанных файлов открыть, —
    /// по имени он этого не поймёт, а угадывать нельзя: сегодня страница
    /// называется panel.html, завтра иначе. Помечает её сервер по client-main из
    /// описания ресурса.
    bool page = false;
};

/// Список того, что сервер раздаёт сверх самой игры.
struct ResourceList {
    static constexpr MessageId kId = MessageId::ResourceList;

    std::vector<ResourceEntry> entries;

    void write(ByteWriter& writer) const;
    [[nodiscard]] static ResourceList read(ByteReader& reader);
};

// --- Упаковка сообщений в пакеты ----------------------------------------------
//
// Пакет — это байт с типом сообщения и следом его поля. Границы пакетов
// обеспечивает транспорт, поэтому длину сообщения передавать не нужно.

/// Собирает пакет из сообщения.
template<typename Message>
[[nodiscard]] std::vector<std::uint8_t> encode(const Message& message) {
    ByteWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(Message::kId));
    message.write(writer);
    return std::move(writer).take();
}

/// Тип сообщения в пакете, не разбирая остального. nullopt на пустом пакете
/// или на неизвестном значении.
[[nodiscard]] std::optional<MessageId> peekMessageId(ByteView packet) noexcept;

/// Разбирает пакет в сообщение ожидаемого типа.
///
/// Возвращает nullopt, если тип не тот, данных не хватило или, наоборот,
/// остались лишние байты: и то и другое означает, что стороны разошлись в
/// понимании протокола, и продолжать разбор нельзя.
template<typename Message>
[[nodiscard]] std::optional<Message> decode(ByteView packet) {
    ByteReader reader{packet};

    if (reader.readU8() != static_cast<std::uint8_t>(Message::kId)) {
        return std::nullopt;
    }

    Message message = Message::read(reader);
    if (!reader.ok() || !reader.exhausted()) {
        return std::nullopt;
    }

    return message;
}

} // namespace oxymp::shared
