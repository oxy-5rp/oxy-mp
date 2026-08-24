#pragma once

#include <oxymp/script/core.hpp>
#include <oxymp/script/events.hpp>

#include <node.h>
#include <v8.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace oxymp::script::js {

/// Один поднятый ресурс: игровой режим или его часть.
///
/// Своя песочница целиком — свой изолят V8, своя куча, свой цикл событий libuv.
/// Дороже общей на несколько мегабайт, и взято намеренно: ресурсы пишут разные
/// люди, и уронивший свою кучу не должен уносить чужие. У alt:V устроено иначе —
/// один изолят на все ресурсы, — и там за это платят тем, что тяжёлая уборка
/// мусора в одном режиме останавливает все остальные.
///
/// Живёт ровно между start и разрушением. Ни копировать, ни переносить его
/// нельзя: внутри — указатели, которые V8 раздал наружу.
class Resource final {
public:
    Resource(std::string name, std::filesystem::path root, Core& core,
             node::MultiIsolatePlatform& platform);
    ~Resource();

    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    /// Заводит изолят, ставит привязки и исполняет точку входа.
    ///
    /// false — с объяснением в error. Объяснение содержательное: чаще всего это
    /// ошибка в самом скрипте, и хозяину сервера нужно увидеть её текст, а не
    /// «ресурс не поднялся».
    [[nodiscard]] bool start(const std::filesystem::path& main, std::string& error);

    /// Даёт движку поработать: таймеры, обещания, ввод-вывод.
    ///
    /// Без этого не сработает ни один setTimeout и не разрешится ни одно
    /// обещание. Не блокирует: цикл прокручивается ровно настолько, насколько
    /// готов прямо сейчас, — сервер не имеет права ждать чужой ввод-вывод.
    void pump();

    /// Отдаёт событие подписчикам ресурса.
    ///
    /// false — обработчик отменил происходящее. Считается это только у реплики
    /// в чате: остальное отменять нечему и незачем.
    [[nodiscard]] bool dispatch(const Event& event);

    /// Отдаёт подписчикам событие, названное скриптом.
    ///
    /// Тем, чем ресурсы говорят друг с другом: у alt:V это `alt.emit`, и там
    /// его слышат все ресурсы сразу. Доводы приходят уложенными в строку —
    /// иначе никак: у каждого ресурса свой изолят, и значение одного в чужом не
    /// живёт вовсе.
    void deliver(std::string_view name, std::string_view payload);

    /// Куда уходит то, что ресурс объявил сам.
    ///
    /// Ставит движок: только он знает про остальные ресурсы. Ресурс же про них
    /// не знает и знать не должен — он объявляет в пустоту, а разносит движок.
    using Announce = std::function<void(std::string_view name, std::string_view payload)>;

    void onAnnounce(Announce announce) { announce_ = std::move(announce); }

    /// Объявляет событие остальным. Зовётся из привязки oxymp.emit.
    void announce(std::string_view name, std::string_view payload) const {
        if (announce_) {
            announce_(name, payload);
        }
    }

    /// Кто ещё поднят: имя и корень каждого.
    ///
    /// Ставит движок, по той же причине, что и Announce: про остальные ресурсы
    /// знает только он. Ресурс же спрашивает — и получает список, а не доступ:
    /// чужой изолят ему по-прежнему недоступен, и это не ограничение, а
    /// устройство. Значение из одного изолята в другом не живёт вовсе.
    using Roster = std::function<std::vector<std::pair<std::string, std::string>>()>;

    void onRoster(Roster roster) { roster_ = std::move(roster); }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> roster() const {
        return roster_ ? roster_() : std::vector<std::pair<std::string, std::string>>{};
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] Core& core() const noexcept { return *core_; }

    /// Ресурс, которому принадлежит изолят.
    ///
    /// Так обработчики привязок добираются до ядра, не таская указатель на себя
    /// в каждом v8::External: изолят у ресурса свой, и путаницы быть не может.
    [[nodiscard]] static Resource* of(v8::Isolate* isolate) noexcept;

    /// Запоминает обработчик события. Зовётся из привязки oxymp.on.
    void subscribe(std::string name, v8::Local<v8::Function> handler);

    /// Заготовки сущностей, заведённые для этого изолята.
    ///
    /// Заготовка, а не готовая функция-конструктор, и это не мелочь: заводить
    /// объект надо мимо конструктора. Конструктор у сущностей нарочно бросает —
    /// игроков заводит соединение, машины заводит ядро, — и Function::NewInstance
    /// его исправно вызывает. Заготовка же отдаёт объект с нужной цепочкой
    /// прототипов, никого не спрашивая.
    [[nodiscard]] v8::Local<v8::FunctionTemplate> playerShape() const;
    [[nodiscard]] v8::Local<v8::FunctionTemplate> vehicleShape() const;
    [[nodiscard]] v8::Local<v8::FunctionTemplate> objectShape() const;
    [[nodiscard]] v8::Local<v8::FunctionTemplate> pedShape() const;

    void setPlayerShape(v8::Local<v8::FunctionTemplate> value);
    void setVehicleShape(v8::Local<v8::FunctionTemplate> value);
    void setObjectShape(v8::Local<v8::FunctionTemplate> value);
    void setPedShape(v8::Local<v8::FunctionTemplate> value);

private:
    /// Слот в изоляте, в котором лежит указатель на ресурс.
    ///
    /// Node своими слотами изолята не пользуется вовсе — проверено по его
    /// исходникам, ни одного обращения к Isolate::SetData. Берётся всё же не
    /// нулевой, а первый: нулевой — первый кандидат для всякого, кто однажды
    /// решит слотами воспользоваться, и уступить его дешевле, чем однажды
    /// разбираться, чей указатель мы затёрли.
    static constexpr std::uint32_t kIsolateSlot = 1;

    /// Зовёт обработчики одного имени. false — кто-то отменил.
    [[nodiscard]] bool call(v8::Local<v8::Context> context, std::string_view name,
                            std::vector<v8::Local<v8::Value>>& arguments);

    /// Пишет в журнал сервера то, что скрипт не поймал сам.
    void report(v8::Local<v8::Context> context, const v8::TryCatch& caught) const;

    /// Дожидается, пока исполнится точка входа. false — она отказала.
    ///
    /// Точка входа грузится через import(), то есть обещанием, и к концу
    /// LoadEnvironment ещё не исполнена. Ждать приходится прокруткой цикла
    /// событий — другого способа сдвинуть обещание с места нет.
    [[nodiscard]] bool awaitStart(v8::Local<v8::Context> context, v8::Isolate* isolate,
                                  std::string& error);

    std::string name_;
    std::filesystem::path root_;
    Core* core_ = nullptr;
    node::MultiIsolatePlatform* platform_ = nullptr;

    /// Изолят, контекст и цикл событий разом. Пусто, пока не поднят.
    std::unique_ptr<node::CommonEnvironmentSetup> setup_;

    /// Обработчики по имени события. Порядок подписки сохраняется: первый
    /// подписавшийся первым и получит возможность отменить реплику.
    std::unordered_map<std::string, std::vector<v8::Global<v8::Function>>> handlers_;

    Announce announce_;
    Roster roster_;

    /// Заготовки классов сущностей, которые слой завёл в этом изоляте.
    ///
    /// **Массивом, а не четырьмя полями, и это не причёсывание.** Полями они и
    /// были, а уборка перечисляла их поимённо — в двух местах, в отказе запуска
    /// и в разборе. Когда к игроку и машине добавились предмет и кукла, в оба
    /// перечня их вписать забыли: две ссылки переживали разбор изолята, и их
    /// деструкторы срабатывали по мёртвой памяти. Сервер от этого падал молча —
    /// целиком, с `Check failed: node->IsInUse()`, — стоило одному ресурсу
    /// отказать при запуске. То есть опечатка в чужом режиме уносила сессию всех.
    ///
    /// Массив это исключает: уборка ходит по нему циклом, и новая заготовка
    /// попадает в неё сама, одной строкой в перечислении ниже.
    enum class Shape : std::size_t { Player, Vehicle, Object, Ped, kCount };

    [[nodiscard]] v8::Local<v8::FunctionTemplate> shapeOf(Shape which) const;
    void setShape(Shape which, v8::Local<v8::FunctionTemplate> value);

    /// Отпускает всё, что держит изолят.
    ///
    /// Зовётся из единственного места на каждый путь — и из отказа запуска, и из
    /// разбора. Звать её обязательно **до** того, как изолят разобран: отпустить
    /// ссылку можно только изнутри живого изолята.
    void releaseHandles();

    std::array<v8::Global<v8::FunctionTemplate>, static_cast<std::size_t>(Shape::kCount)> shapes_;
};

} // namespace oxymp::script::js
