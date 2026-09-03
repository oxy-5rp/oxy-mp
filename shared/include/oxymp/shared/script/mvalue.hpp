#pragma once

#include <oxymp/shared/math/vec3.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oxymp::shared {

/// Точка на плоскости.
///
/// Живёт здесь, а не рядом с Vec3, потому что нужна только скриптам: игре
/// двумерные точки ни к чему, а скрипт ими описывает положение на экране и на
/// карте.
struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;

    friend bool operator==(const Vec2&, const Vec2&) = default;
};

/// Цвет с прозрачностью, по байту на составляющую.
struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend bool operator==(const Rgba&, const Rgba&) = default;
};

/// Род сущности сессии, для ссылки внутри MValue.
///
/// Нужен затем, чтобы ссылка на сущность в значении оставалась однозначной:
/// номера игроков, машин и предметов считаются каждый от своего начала, и
/// «двадцать первый» без рода — это трое разных.
///
/// Названо не просто `EntityKind`, а с припиской: у протокола (messages.hpp)
/// есть свой род сущности, с иным составом (там же есть `None` и `Ped`) и для
/// другой надобности — привязки сущностей друг к другу. Оба живут в одном
/// пространстве имён `oxymp::shared`, и общее имя разошлось бы молча: файл,
/// включивший разом оба заголовка, получил бы ошибку переопределения, а до
/// этого дня — два одноимённых, но разных типа, отличимых только тем, откуда
/// взято значение.
enum class MValueEntityKind : std::uint8_t {
    Player = 1,
    Vehicle = 2,
    Object = 3,
};

/// Ссылка на сущность сессии внутри значения.
///
/// Номер, а не указатель, по той же причине, по какой номера держит и скриптовый
/// слой: сущность может исчезнуть между отправкой значения и его разбором.
/// Получатель разрешает номер заново и сам решает, что делать с пропавшим.
struct EntityRef {
    MValueEntityKind kind = MValueEntityKind::Player;
    std::uint32_t id = 0;

    friend bool operator==(const EntityRef&, const EntityRef&) = default;
};

/// Значение, которым скрипты обмениваются между собой и по сети.
///
/// Заведено ради событий. Своё событие протокол описывал одной строкой, и этого
/// хватало, пока страница интерфейса была единственным собеседником ресурса:
/// разметку она получала строкой, нажатие возвращала строкой. Скрипты же
/// передают друг другу числа, списки, словари и сущности, и укладывать их в
/// строку пришлось бы на каждом конце — то есть завести свой JSON и свои правила
/// его толкования в каждом ресурсе.
///
/// Устройство повторяет MValue из alt:V, и это не подражание: набор типов там
/// выведен из того, что действительно передают игровые режимы, и сходившийся
/// годами. Отличие одно, зато существенное — здесь это значение, а не
/// подсчитываемая ссылка за интерфейсом. Считать ссылки нужно тому, кто отдаёт
/// объекты через ABI чужому модулю; у нас модуль свой, и цена такого решения —
/// вечная возня со временем жизни — не покупает ничего.
///
/// Список и словарь держат значения прямо в себе, а не за указателем. Так
/// вложенность стоит одного выделения памяти на уровень, но и обязывает
/// разбирающего следить за глубиной: см. kMaxDepth.
class MValue {
public:
    /// Что именно лежит в значении.
    ///
    /// Числа заданы явно и повторно не выдаются: они уходят в сеть, и значение,
    /// однажды присвоенное типу, закрепляется за ним навсегда. Порядок взят у
    /// alt:V, чтобы человеку, читающему оба протокола, не приходилось держать в
    /// голове две таблицы.
    enum class Type : std::uint8_t {
        /// Значения нет вовсе — довод не был передан.
        ///
        /// Отличается от Nil так же, как в JS `undefined` отличается от `null`:
        /// первое означает «не сказано», второе — «сказано, что ничего».
        /// Ресурсы на эту разницу опираются, и сводить их в одно нельзя.
        None = 0,
        Nil = 1,
        Bool = 2,
        Int = 3,
        UInt = 4,
        Double = 5,
        String = 6,
        List = 7,
        Dict = 8,
        Entity = 9,
        Vector3 = 10,
        Vector2 = 11,
        Rgba = 12,
        ByteArray = 13,
    };

    /// Список значений.
    using List_ = std::vector<MValue>;

    /// Словарь: пары «ключ — значение» в том порядке, в котором их положили.
    ///
    /// Список пар, а не std::map, по двум причинам. Порядок ключей у объекта в
    /// JS сохраняется, и ресурс, собравший словарь для страницы интерфейса,
    /// вправе ожидать его назад в том же виде. И упорядоченный контейнер
    /// стандартной библиотеки с незавершённым типом — неопределённое поведение,
    /// а MValue на этой строке ещё не завершён.
    using Dict_ = std::vector<std::pair<std::string, MValue>>;

    /// Двоичные данные.
    using Bytes = std::vector<std::uint8_t>;

    /// Предел вложенности списков и словарей при разборе.
    ///
    /// Существует не ради стройности, а ради того, чтобы присланное клиентом
    /// значение не уронило сервер. Разбор вложенного значения рекурсивен, и
    /// список из десяти тысяч открывающих скобок исчерпал бы стек — то есть
    /// один пакет от одного игрока прекратил бы сессию для всех.
    ///
    /// Тридцать два уровня — с запасом вдвое от всего, что встречается в живых
    /// игровых режимах: дерево страницы интерфейса редко глубже десяти.
    static constexpr std::size_t kMaxDepth = 32;

    /// Предел длины списка и числа ключей в словаре при разборе.
    static constexpr std::size_t kMaxListLength = 8192;

    /// Предел длины двоичных данных при разборе, в байтах.
    ///
    /// Мегабайт: этим полем ходят снимки экрана и небольшие файлы, а всё, что
    /// крупнее, раздаётся ресурсом, а не пересылается событием.
    static constexpr std::size_t kMaxByteArrayLength = 1024U * 1024U;

    /// Отсутствующее значение.
    MValue() = default;

    /// Явные заводы вместо конструкторов на каждый тип.
    ///
    /// Именно так, а не набором перегруженных конструкторов: bool, целые и
    /// double между собой неявно преобразуются, и `MValue{0}` выбирал бы тип по
    /// правилам, которых не помнит никто. Имя у каждого завода снимает вопрос.
    [[nodiscard]] static MValue nil();
    [[nodiscard]] static MValue boolean(bool value);
    [[nodiscard]] static MValue integer(std::int64_t value);
    [[nodiscard]] static MValue unsignedInteger(std::uint64_t value);
    [[nodiscard]] static MValue number(double value);
    [[nodiscard]] static MValue string(std::string value);
    [[nodiscard]] static MValue list(List_ value);
    [[nodiscard]] static MValue dict(Dict_ value);
    [[nodiscard]] static MValue entity(EntityRef value);
    [[nodiscard]] static MValue vector3(const Vec3& value);
    [[nodiscard]] static MValue vector2(const Vec2& value);
    [[nodiscard]] static MValue rgba(const Rgba& value);
    [[nodiscard]] static MValue byteArray(Bytes value);

    [[nodiscard]] Type type() const noexcept;

    [[nodiscard]] bool isNone() const noexcept { return type() == Type::None; }

    /// Чтение с проверкой типа: пусто, если внутри лежит не то.
    ///
    /// Возвращает указатель, а не бросает: событие приходит от того, кто мог
    /// собрать его как угодно, и несовпадение типа — обычный ход дела, а не
    /// поломка. Обработчик, читающий чужой ввод, обязан быть к этому готов, и
    /// проверка через `if` напоминает об этом лучше, чем перехват исключения.
    [[nodiscard]] const bool* asBool() const noexcept;
    [[nodiscard]] const std::int64_t* asInt() const noexcept;
    [[nodiscard]] const std::uint64_t* asUInt() const noexcept;
    [[nodiscard]] const double* asDouble() const noexcept;
    [[nodiscard]] const std::string* asString() const noexcept;
    [[nodiscard]] const List_* asList() const noexcept;
    [[nodiscard]] const Dict_* asDict() const noexcept;
    [[nodiscard]] const EntityRef* asEntity() const noexcept;
    [[nodiscard]] const Vec3* asVector3() const noexcept;
    [[nodiscard]] const Vec2* asVector2() const noexcept;
    [[nodiscard]] const Rgba* asRgba() const noexcept;
    [[nodiscard]] const Bytes* asByteArray() const noexcept;

    /// Значение по ключу словаря. Пусто — не словарь или ключа нет.
    [[nodiscard]] const MValue* find(std::string_view key) const noexcept;

    friend bool operator==(const MValue&, const MValue&) = default;

private:
    /// Пустая метка для Nil.
    ///
    /// Своим типом, а не std::monostate: monostate уже занят под None, а
    /// «ничего нет» и «есть ничто» — разные значения (см. Type::None).
    struct NilTag {
        friend bool operator==(const NilTag&, const NilTag&) = default;
    };

    using Body = std::variant<std::monostate, NilTag, bool, std::int64_t, std::uint64_t, double,
                              std::string, List_, Dict_, EntityRef, Vec3, Vec2, Rgba, Bytes>;

    explicit MValue(Body body) : body_(std::move(body)) {}

    Body body_;
};

/// Набор доводов события.
using MValueArgs = std::vector<MValue>;

} // namespace oxymp::shared
