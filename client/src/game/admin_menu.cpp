#include "admin_menu.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace oxymp::client::game {
namespace {

/// Машины, которые меню предлагает готовым списком.
///
/// Список короткий и намеренно: это не каталог всей игры, а то, на чём удобно
/// проверять сессию — быстрое, вместительное, летающее и по воде. Всё остальное
/// берётся первым пунктом, по названию: моделей в игре больше семисот, и списком
/// их не показать.
struct Model {
    const char* name;
    const char* label;
};

/// Место пункта «по названию» в разделе транспорта.
///
/// Первым, а не последним: набрать название — это то, ради чего в раздел заходят
/// чаще всего, а список под ним — короткая подсказка на случай, если название не
/// вспоминается.
constexpr std::size_t kByName = 0;

constexpr std::array<Model, 14> kVehicles = {{
    {"adder", "Adder"},
    {"zentorno", "Zentorno"},
    {"t20", "T20"},
    {"sultanrs", "Sultan RS"},
    {"kuruma", "Kuruma"},
    {"bati", "Bati 801"},
    {"sanchez", "Sanchez"},
    {"insurgent", "Insurgent"},
    {"riot", "Riot"},
    {"buzzard", "Buzzard"},
    {"maverick", "Maverick"},
    {"lazer", "Lazer"},
    {"seashark", "Seashark"},
    {"dinghy", "Dinghy"},
}};

/// Внешности, которые меню умеет выдавать.
constexpr std::array<Model, 8> kSkins = {{
    {"mp_m_freemode_01", "Сетевой мужчина"},
    {"mp_f_freemode_01", "Сетевая женщина"},
    {"player_zero", "Майкл"},
    {"player_one", "Франклин"},
    {"player_two", "Тревор"},
    {"s_m_y_cop_01", "Полицейский"},
    {"s_m_m_paramedic_01", "Медик"},
    {"a_c_chop", "Чоп"},
}};

/// Места, куда меню умеет переносить.
struct Landmark {
    const char* label;
    shared::Vec3 position;
};

constexpr std::array<Landmark, 8> kLandmarks = {{
    {"Аэропорт", {-1037.7F, -2738.0F, 20.2F}},
    {"Центр Лос-Сантоса", {-268.0F, -956.0F, 31.2F}},
    {"Пляж Веспуччи", {-1223.0F, -1490.0F, 4.4F}},
    {"Обсерватория", {-438.8F, 1075.7F, 352.4F}},
    {"Гора Чилиад", {501.9F, 5604.4F, 797.9F}},
    {"Аэродром Сэнди-Шорс", {1728.0F, 3286.0F, 41.2F}},
    {"Тюрьма Болингброук", {1845.0F, 2585.0F, 45.7F}},
    {"Каюн-Секвойя", {-550.0F, 5330.0F, 74.2F}},
}};

/// Погода, которую меню умеет ставить.
struct Weather {
    const char* name;
    const char* label;
};

constexpr std::array<Weather, 6> kWeathers = {{
    {"EXTRASUNNY", "Ясно"},
    {"CLEAR", "Чисто"},
    {"CLOUDS", "Облачно"},
    {"RAIN", "Дождь"},
    {"THUNDER", "Гроза"},
    {"FOGGY", "Туман"},
}};

/// Время суток, которое меню умеет ставить.
struct TimeOfDay {
    const char* label;
    int hour;
};

constexpr std::array<TimeOfDay, 4> kTimes = {{
    {"Утро", 8},
    {"День", 13},
    {"Вечер", 19},
    {"Ночь", 1},
}};

/// Сколько кадров ждать заказанную модель.
///
/// Секунда с небольшим при обычной частоте кадров. Модели, которой нет в игре,
/// не дождаться никогда, и висеть в ожидании молча — худшее, что можно сделать.
constexpr int kModelWaitFrames = 90;

/// На сколько машина ставится впереди игрока, в метрах.
constexpr float kSpawnAhead = 4.0F;

/// На сколько переносимый ставится в стороне от того, к кому его переносят.
///
/// Ноль означал бы двоих в одной точке: игра растолкает их сама, но перед этим
/// покажет, как они друг в друге.
constexpr float kArrivalOffset = 2.0F;

constexpr int kFullHealth = 200;
constexpr int kFullArmour = 100;

} // namespace

AdminMenu::AdminMenu(const NativeTable& table) noexcept
    : hashKey_(table.handlerFor(natives::kGetHashKey)),
      requestModel_(table.handlerFor(natives::kRequestModel)),
      hasModelLoaded_(table.handlerFor(natives::kHasModelLoaded)),
      modelNoLongerNeeded_(table.handlerFor(natives::kSetModelAsNoLongerNeeded)),
      isModelInCdimage_(table.handlerFor(natives::kIsModelInCdimage)),
      isModelAVehicle_(table.handlerFor(natives::kIsModelAVehicle)),
      createVehicle_(table.handlerFor(natives::kCreateVehicle)),
      deleteVehicle_(table.handlerFor(natives::kDeleteVehicle)),
      setIntoVehicle_(table.handlerFor(natives::kSetPedIntoVehicle)),
      vehiclePedIsIn_(table.handlerFor(natives::kGetVehiclePedIsIn)),
      isPedInAnyVehicle_(table.handlerFor(natives::kIsPedInAnyVehicle)),
      vehicleFixed_(table.handlerFor(natives::kSetVehicleFixed)),
      vehicleDirt_(table.handlerFor(natives::kSetVehicleDirtLevel)),
      vehicleOnGround_(table.handlerFor(natives::kSetVehicleOnGroundProperly)),
      engineOn_(table.handlerFor(natives::kSetVehicleEngineOn)),
      setPlayerModel_(table.handlerFor(natives::kSetPlayerModel)),
      defaultVariation_(table.handlerFor(natives::kSetPedDefaultComponentVariation)),
      playerPedId_(table.handlerFor(natives::kPlayerPedId)),
      getCoords_(table.handlerFor(natives::kGetEntityCoords)),
      setCoords_(table.handlerFor(natives::kSetEntityCoords)),
      getHeading_(table.handlerFor(natives::kGetEntityHeading)),
      setHealth_(table.handlerFor(natives::kSetEntityHealth)),
      setArmour_(table.handlerFor(natives::kSetPedArmour)),
      clearBlood_(table.handlerFor(natives::kClearPedBloodDamage)),
      invincible_(table.handlerFor(natives::kSetEntityInvincible)),
      firstBlip_(table.handlerFor(natives::kGetFirstBlipInfoId)),
      blipCoord_(table.handlerFor(natives::kGetBlipInfoIdCoord)),
      blipExists_(table.handlerFor(natives::kDoesBlipExist)),
      setWeather_(table.handlerFor(natives::kSetWeatherTypeNow)),
      setClock_(table.handlerFor(natives::kOverrideClockTime)) {}

bool AdminMenu::ready() const noexcept {
    return hashKey_ != nullptr && requestModel_ != nullptr && hasModelLoaded_ != nullptr &&
           createVehicle_ != nullptr && setIntoVehicle_ != nullptr && setPlayerModel_ != nullptr &&
           setCoords_ != nullptr && getCoords_ != nullptr;
}

void AdminMenu::toggle() {
    open_ = !open_;

    if (open_) {
        page_ = Page::Root;
        selected_ = rootSelected_;
        note_.clear();
    }
}

void AdminMenu::close() {
    open_ = false;

    // Закрытое меню строку уже не ждёт: иначе ввод остался бы висеть без того,
    // кто его заказал.
    cancelText();
}

void AdminMenu::enter(Page page) {
    if (page_ == Page::Root) {
        rootSelected_ = selected_;
    }

    page_ = page;
    selected_ = 0;
}

std::size_t AdminMenu::itemCount() const {
    switch (page_) {
    case Page::Root:
        return 5;
    case Page::Vehicles:
        // Ввод названия, список машин, «убрать» и «починить».
        return kVehicles.size() + 3;
    case Page::Skins:
        return kSkins.size();
    case Page::Teleport:
        // Метка на карте плюс список мест.
        return kLandmarks.size() + 1;
    case Page::Players:
        // Список игроков плюс «собрать всех».
        return players_.size() + 1;
    case Page::World:
        return kWeathers.size() + kTimes.size() + 2;
    }

    return 0;
}

void AdminMenu::press(Key key, int player, int ped) {
    if (!open_) {
        return;
    }

    const int count = static_cast<int>(itemCount());

    switch (key) {
    case Key::Up:
        // По кругу: список короткий, и упираться в его край незачем.
        selected_ = count > 0 ? (selected_ + count - 1) % count : 0;
        return;

    case Key::Down:
        selected_ = count > 0 ? (selected_ + 1) % count : 0;
        return;

    case Key::Back:
        if (page_ == Page::Root) {
            close();
        } else {
            page_ = Page::Root;
            selected_ = rootSelected_;
        }
        return;

    case Key::Enter:
        activate(player, ped, false);
        return;

    case Key::Alternate:
        activate(player, ped, true);
        return;
    }
}

void AdminMenu::activate(int player, int ped, bool alternate) {
    const std::size_t index = static_cast<std::size_t>(std::max(selected_, 0));

    switch (page_) {
    case Page::Root:
        switch (index) {
        case 0:
            enter(Page::Vehicles);
            return;
        case 1:
            enter(Page::Skins);
            return;
        case 2:
            enter(Page::Teleport);
            return;
        case 3:
            enter(Page::Players);
            return;
        default:
            enter(Page::World);
            return;
        }

    case Page::Vehicles: {
        if (index == kByName) {
            asking_ = true;
            prompt_ = "модель:";
            note_ = "введите название модели и нажмите ввод";
            return;
        }

        if (const std::size_t listed = index - 1; listed < kVehicles.size()) {
            orderVehicle(invokeNative<std::uint32_t>(hashKey_, kVehicles[listed].name),
                         kVehicles[listed].label);
            return;
        }

        const int vehicle = isPedInAnyVehicle_ != nullptr &&
                                    invokeNative<bool>(isPedInAnyVehicle_, ped, false)
                                ? invokeNative<int>(vehiclePedIsIn_, ped, false)
                                : 0;

        if (vehicle == 0) {
            note_ = "вы не в машине";
            return;
        }

        if (index == kVehicles.size() + 1) {
            int handle = vehicle;

            NativeContext context;
            context.push(&handle);
            deleteVehicle_(context.address());

            note_ = "машина убрана";
            return;
        }

        if (vehicleFixed_ != nullptr) {
            invokeNative<void>(vehicleFixed_, vehicle);
        }
        if (vehicleDirt_ != nullptr) {
            invokeNative<void>(vehicleDirt_, vehicle, 0.0F);
        }

        note_ = "машина починена";
        return;
    }

    case Page::Skins:
        if (index < kSkins.size()) {
            pendingModel_ = invokeNative<std::uint32_t>(hashKey_, kSkins[index].name);
            pending_ = Pending::Skin;
            pendingFrames_ = 0;

            note_ = std::format("заказана внешность «{}»", kSkins[index].label);
        }
        return;

    case Page::Teleport: {
        if (index == 0) {
            shared::Vec3 destination;

            if (!waypoint(destination)) {
                note_ = "метки на карте нет";
                return;
            }

            teleportSelf(ped, destination);
            note_ = "перенесены к метке";
            return;
        }

        const std::size_t landmark = index - 1;
        if (landmark >= kLandmarks.size()) {
            return;
        }

        teleportSelf(ped, kLandmarks[landmark].position);
        note_ = std::format("перенесены: {}", kLandmarks[landmark].label);
        return;
    }

    case Page::Players: {
        if (!request_) {
            note_ = "сети нет";
            return;
        }

        NativeContext coords;
        coords.push(ped);
        coords.push(true);
        getCoords_(coords.address());

        const shared::Vec3 here{coords.result<float>(0) + kArrivalOffset,
                                coords.result<float>(1), coords.result<float>(2)};

        if (index == players_.size()) {
            request_(shared::AdminCommand::Summon, shared::kInvalidPlayerId, here);
            note_ = "все вызваны к вам";
            return;
        }

        if (index >= players_.size()) {
            return;
        }

        const Participant& participant = players_[index];

        if (alternate) {
            request_(shared::AdminCommand::Summon, participant.id, here);
            note_ = std::format("{} вызван к вам", participant.nickname);
            return;
        }

        teleportSelf(ped, participant.position);
        note_ = std::format("перенесены к {}", participant.nickname);
        return;
    }

    case Page::World: {
        if (index < kWeathers.size()) {
            if (setWeather_ != nullptr) {
                invokeNative<void>(setWeather_, kWeathers[index].name);
            }

            note_ = std::format("погода: {}", kWeathers[index].label);
            return;
        }

        const std::size_t after = index - kWeathers.size();

        if (after < kTimes.size()) {
            if (setClock_ != nullptr) {
                invokeNative<void>(setClock_, kTimes[after].hour, 0, 0);
            }

            note_ = std::format("время: {}", kTimes[after].label);
            return;
        }

        if (after == kTimes.size()) {
            if (setHealth_ != nullptr) {
                invokeNative<void>(setHealth_, ped, kFullHealth);
            }
            if (setArmour_ != nullptr) {
                invokeNative<void>(setArmour_, ped, kFullArmour);
            }
            if (clearBlood_ != nullptr) {
                invokeNative<void>(clearBlood_, ped);
            }

            note_ = "здоровье и броня восстановлены";
            return;
        }

        invincibleOn_ = !invincibleOn_;

        if (invincible_ != nullptr) {
            invokeNative<void>(invincible_, ped, invincibleOn_);
        }

        note_ = invincibleOn_ ? "неуязвимость включена" : "неуязвимость выключена";
        return;
    }
    }

    (void)player;
}

void AdminMenu::teleportSelf(int ped, shared::Vec3 destination) const {
    if (ped == 0) {
        return;
    }

    // Признаки те же, что и при обычном переносе: не выключать столкновения, не
    // искать землю и считать перенос мгновенным.
    invokeNative<void>(setCoords_, ped, destination.x, destination.y, destination.z, false, false,
                       false, true);
}

bool AdminMenu::waypoint(shared::Vec3& destination) const {
    if (firstBlip_ == nullptr || blipCoord_ == nullptr || blipExists_ == nullptr) {
        return false;
    }

    // Восьмёрка — вид отметки «точка назначения игрока». Так метка на карте
    // числится у игры среди прочих отметок.
    constexpr int kWaypointBlip = 8;

    const int blip = invokeNative<int>(firstBlip_, kWaypointBlip);
    if (blip == 0 || !invokeNative<bool>(blipExists_, blip)) {
        return false;
    }

    NativeContext context;
    context.push(blip);
    blipCoord_(context.address());

    destination = shared::Vec3{context.result<float>(0), context.result<float>(1),
                               context.result<float>(2)};

    // Высоту метка не хранит — карта плоская. Точка ставится высоко, чтобы
    // игрок не оказался внутри горы, а на землю его опустит собственный вес.
    constexpr float kSafeHeight = 300.0F;
    destination.z = kSafeHeight;

    return true;
}

void AdminMenu::orderVehicle(std::uint32_t model, std::string_view label) {
    pendingModel_ = model;
    pending_ = Pending::Vehicle;
    pendingFrames_ = 0;

    note_ = std::format("заказана {}", label);
}

void AdminMenu::supplyText(std::string typed) {
    asking_ = false;
    prompt_.clear();

    // Пробелы по краям убираются молча: в названии модели их нет никогда, а
    // случайный пробел в конце превратил бы верное название в неизвестное.
    const std::size_t first = typed.find_first_not_of(" \t");
    const std::size_t last = typed.find_last_not_of(" \t");

    if (first == std::string::npos) {
        note_ = "название не введено";
        return;
    }

    const std::string name = typed.substr(first, last - first + 1);

    const std::uint32_t model = invokeNative<std::uint32_t>(hashKey_, name.c_str());

    // Проверка до заказа, а не после. Заказ несуществующей модели ничем себя не
    // выдаёт: игра просто никогда её не загрузит, и меню будет ждать её впустую
    // до конца отсчёта, показывая «заказана» вместо «такой нет».
    if (isModelInCdimage_ != nullptr && !invokeNative<bool>(isModelInCdimage_, model)) {
        note_ = std::format("модели «{}» в игре нет", name);
        return;
    }

    if (isModelAVehicle_ != nullptr && !invokeNative<bool>(isModelAVehicle_, model)) {
        note_ = std::format("«{}» — не машина", name);
        return;
    }

    orderVehicle(model, name);
}

void AdminMenu::cancelText() {
    asking_ = false;
    prompt_.clear();
    note_.clear();
}

void AdminMenu::spawnVehicle(int ped) {
    NativeContext coords;
    coords.push(ped);
    coords.push(true);
    getCoords_(coords.address());

    const float heading = getHeading_ != nullptr ? invokeNative<float>(getHeading_, ped) : 0.0F;

    // Впереди игрока, а не под ним: созданная в его точке машина выталкивает
    // его наружу, и это выглядит как сбой.
    constexpr float kDegreesToRadians = 3.14159265F / 180.0F;

    const float radians = heading * kDegreesToRadians;

    const float x = coords.result<float>(0) - std::sin(radians) * kSpawnAhead;
    const float y = coords.result<float>(1) + std::cos(radians) * kSpawnAhead;
    const float z = coords.result<float>(2);

    const int vehicle = invokeNative<int>(createVehicle_, pendingModel_, x, y, z, heading, false,
                                          false, false);

    if (vehicle == 0) {
        note_ = "машину создать не удалось";
        return;
    }

    if (vehicleOnGround_ != nullptr) {
        invokeNative<void>(vehicleOnGround_, vehicle);
    }
    if (engineOn_ != nullptr) {
        invokeNative<void>(engineOn_, vehicle, true, true, false);
    }

    invokeNative<void>(setIntoVehicle_, ped, vehicle, shared::kDriverSeat);

    if (modelNoLongerNeeded_ != nullptr) {
        invokeNative<void>(modelNoLongerNeeded_, pendingModel_);
    }

    note_ = "машина выдана";
}

void AdminMenu::applySkin(int player, int ped) {
    invokeNative<void>(setPlayerModel_, player, pendingModel_);

    // Персонаж после смены модели — уже другой, и старый номер к нему не ведёт.
    const int fresh = playerPedId_ != nullptr ? invokeNative<int>(playerPedId_) : ped;

    if (defaultVariation_ != nullptr && fresh != 0) {
        invokeNative<void>(defaultVariation_, fresh, false);
    }

    if (modelNoLongerNeeded_ != nullptr) {
        invokeNative<void>(modelNoLongerNeeded_, pendingModel_);
    }

    note_ = "внешность заменена";
}

void AdminMenu::update(std::vector<Participant> players, int player, int ped) {
    players_ = std::move(players);

    // Выбор мог указывать на игрока, который вышел.
    const int count = static_cast<int>(itemCount());
    if (selected_ >= count) {
        selected_ = count > 0 ? count - 1 : 0;
    }

    if (pending_ == Pending::Nothing || !ready() || ped == 0) {
        return;
    }

    if (pendingModel_ == 0 ||
        (isModelInCdimage_ != nullptr && !invokeNative<bool>(isModelInCdimage_, pendingModel_))) {
        pending_ = Pending::Nothing;
        note_ = "такой модели в игре нет";
        return;
    }

    invokeNative<void>(requestModel_, pendingModel_);

    if (!invokeNative<bool>(hasModelLoaded_, pendingModel_)) {
        if (++pendingFrames_ < kModelWaitFrames) {
            return;
        }

        pending_ = Pending::Nothing;
        note_ = "модель не загрузилась";
        return;
    }

    const Pending what = pending_;
    pending_ = Pending::Nothing;

    if (what == Pending::Vehicle) {
        spawnVehicle(ped);
    } else {
        applySkin(player, ped);
    }
}

AdminMenu::View AdminMenu::view() const {
    View view;
    view.open = open_;
    view.selected = selected_;
    view.note = note_;

    if (!open_) {
        return view;
    }

    switch (page_) {
    case Page::Root:
        view.title = "oxyMP";
        view.items = {
            {"Транспорт", ""}, {"Внешность", ""}, {"Телепорт", ""},
            {"Игроки", ""},    {"Мир", ""},
        };
        return view;

    case Page::Vehicles:
        view.title = "Транспорт";
        view.items.reserve(kVehicles.size() + 3);

        view.items.push_back(Item{.label = "Выдать по названию", .value = "ввод"});

        for (const Model& model : kVehicles) {
            view.items.push_back(Item{.label = model.label, .value = model.name});
        }

        view.items.push_back(Item{.label = "Убрать машину", .value = ""});
        view.items.push_back(Item{.label = "Починить машину", .value = ""});
        return view;

    case Page::Skins:
        view.title = "Внешность";
        view.items.reserve(kSkins.size());

        for (const Model& model : kSkins) {
            view.items.push_back(Item{.label = model.label, .value = model.name});
        }
        return view;

    case Page::Teleport:
        view.title = "Телепорт";
        view.items.reserve(kLandmarks.size() + 1);

        view.items.push_back(Item{.label = "К метке на карте", .value = ""});

        for (const Landmark& landmark : kLandmarks) {
            view.items.push_back(Item{.label = landmark.label, .value = ""});
        }
        return view;

    case Page::Players:
        view.title = "Игроки";
        view.items.reserve(players_.size() + 1);

        for (const Participant& participant : players_) {
            view.items.push_back(Item{
                .label = std::format("{} [{}]", participant.nickname, participant.id),
                .value = "ввод — к нему, → — сюда",
            });
        }

        view.items.push_back(Item{.label = "Собрать всех к себе", .value = ""});
        return view;

    case Page::World:
        view.title = "Мир";
        view.items.reserve(kWeathers.size() + kTimes.size() + 2);

        for (const Weather& weather : kWeathers) {
            view.items.push_back(Item{.label = weather.label, .value = "погода"});
        }

        for (const TimeOfDay& time : kTimes) {
            view.items.push_back(
                Item{.label = time.label, .value = std::format("{}:00", time.hour)});
        }

        view.items.push_back(Item{.label = "Вылечить себя", .value = ""});
        view.items.push_back(
            Item{.label = "Неуязвимость", .value = invincibleOn_ ? "вкл" : "выкл"});
        return view;
    }

    return view;
}

} // namespace oxymp::client::game
