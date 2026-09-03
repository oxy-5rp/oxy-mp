// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_test_macros.hpp>

#include <oxymp/shared/script/mvalue.hpp>
#include <oxymp/shared/script/mvalue_codec.hpp>

#include <string>

using namespace oxymp::shared;

namespace {

/// Прогоняет значение через байты и обратно.
[[nodiscard]] MValue roundTrip(const MValue& value) {
    ByteWriter writer;
    writeMValue(writer, value);

    ByteReader reader{ByteView{writer.bytes()}};
    MValue restored = readMValue(reader);

    CHECK(reader.ok());
    CHECK(reader.exhausted());

    return restored;
}

} // namespace

TEST_CASE("a missing value differs from an empty one", "[mvalue]") {
    // Та самая разница между `undefined` и `null`, на которую опираются ресурсы:
    // «довод не передан» и «передано ничто» — разные ответы.
    CHECK(MValue{}.type() == MValue::Type::None);
    CHECK(MValue::nil().type() == MValue::Type::Nil);
    CHECK(MValue{} != MValue::nil());
}

TEST_CASE("plain values survive a round trip", "[mvalue]") {
    CHECK(roundTrip(MValue::boolean(true)) == MValue::boolean(true));
    CHECK(roundTrip(MValue::integer(-42)) == MValue::integer(-42));
    CHECK(roundTrip(MValue::unsignedInteger(1ULL << 40U)) ==
          MValue::unsignedInteger(1ULL << 40U));
    CHECK(roundTrip(MValue::string("привет")) == MValue::string("привет"));
    CHECK(roundTrip(MValue{}) == MValue{});
    CHECK(roundTrip(MValue::nil()) == MValue::nil());
}

TEST_CASE("a fractional number keeps its fraction", "[mvalue]") {
    // Дробная часть — не мелочь: значения вида 0.5 приходят из скриптов
    // постоянно, а запись через целое потеряла бы их молча.
    const MValue restored = roundTrip(MValue::number(0.5));

    REQUIRE(restored.asDouble() != nullptr);
    CHECK(*restored.asDouble() == 0.5);
}

TEST_CASE("a vector and a colour survive a round trip", "[mvalue]") {
    const MValue point = roundTrip(MValue::vector3(Vec3{1.0F, -2.0F, 3.5F}));
    REQUIRE(point.asVector3() != nullptr);
    CHECK(*point.asVector3() == Vec3{1.0F, -2.0F, 3.5F});

    const MValue flat = roundTrip(MValue::vector2(Vec2{0.25F, 8.0F}));
    REQUIRE(flat.asVector2() != nullptr);
    CHECK(*flat.asVector2() == Vec2{0.25F, 8.0F});

    const MValue colour = roundTrip(MValue::rgba(Rgba{1U, 2U, 3U, 4U}));
    REQUIRE(colour.asRgba() != nullptr);
    CHECK(*colour.asRgba() == Rgba{1U, 2U, 3U, 4U});
}

TEST_CASE("an entity reference keeps its kind apart from its number", "[mvalue]") {
    // Номера игроков, машин и предметов считаются каждый от своего начала, и
    // ссылка без рода означала бы трёх разных.
    const MValue vehicle = roundTrip(MValue::entity(EntityRef{MValueEntityKind::Vehicle, 21U}));

    REQUIRE(vehicle.asEntity() != nullptr);
    CHECK(vehicle.asEntity()->kind == MValueEntityKind::Vehicle);
    CHECK(vehicle.asEntity()->id == 21U);
    CHECK(vehicle != MValue::entity(EntityRef{MValueEntityKind::Player, 21U}));
}

TEST_CASE("a list survives a round trip", "[mvalue]") {
    MValue::List_ items;
    items.push_back(MValue::integer(1));
    items.push_back(MValue::string("два"));
    items.push_back(MValue::nil());

    const MValue restored = roundTrip(MValue::list(std::move(items)));

    REQUIRE(restored.asList() != nullptr);
    REQUIRE(restored.asList()->size() == 3U);
    CHECK((*restored.asList())[1] == MValue::string("два"));
}

TEST_CASE("a dictionary keeps the order of its keys", "[mvalue]") {
    // Порядок ключей у объекта в JS сохраняется, и ресурс, собравший словарь для
    // страницы интерфейса, вправе получить его назад в том же виде.
    MValue::Dict_ pairs;
    pairs.emplace_back("z", MValue::integer(1));
    pairs.emplace_back("a", MValue::integer(2));

    const MValue restored = roundTrip(MValue::dict(std::move(pairs)));

    REQUIRE(restored.asDict() != nullptr);
    REQUIRE(restored.asDict()->size() == 2U);
    CHECK((*restored.asDict())[0].first == "z");
    CHECK((*restored.asDict())[1].first == "a");
}

TEST_CASE("a dictionary finds a value by key", "[mvalue]") {
    MValue::Dict_ pairs;
    pairs.emplace_back("health", MValue::integer(200));

    const MValue dict = MValue::dict(std::move(pairs));

    REQUIRE(dict.find("health") != nullptr);
    CHECK(*dict.find("health") == MValue::integer(200));
    CHECK(dict.find("armour") == nullptr);
}

TEST_CASE("reading the wrong type answers nothing instead of throwing", "[mvalue]") {
    // Событие приходит от того, кто мог собрать его как угодно, поэтому
    // несовпадение типа — обычный ход дела, а не поломка.
    const MValue text = MValue::string("не число");

    CHECK(text.asInt() == nullptr);
    CHECK(text.asList() == nullptr);
    REQUIRE(text.asString() != nullptr);
}

TEST_CASE("nested values survive a round trip", "[mvalue]") {
    MValue::List_ inner;
    inner.push_back(MValue::integer(7));

    MValue::Dict_ pairs;
    pairs.emplace_back("items", MValue::list(std::move(inner)));

    const MValue restored = roundTrip(MValue::dict(std::move(pairs)));

    REQUIRE(restored.find("items") != nullptr);
    REQUIRE(restored.find("items")->asList() != nullptr);
    CHECK((*restored.find("items")->asList())[0] == MValue::integer(7));
}

TEST_CASE("event arguments survive a round trip", "[mvalue]") {
    MValueArgs args;
    args.push_back(MValue::string("openWindow"));
    args.push_back(MValue::integer(3));

    ByteWriter writer;
    writeMValueArgs(writer, args);

    ByteReader reader{ByteView{writer.bytes()}};
    const MValueArgs restored = readMValueArgs(reader);

    CHECK(reader.ok());
    CHECK(reader.exhausted());
    REQUIRE(restored.size() == 2U);
    CHECK(restored[0] == MValue::string("openWindow"));
    CHECK(restored[1] == MValue::integer(3));
}

TEST_CASE("an unknown value type fails the read", "[mvalue]") {
    // Единственный способ отличить пакет от новой сборки: она пришлёт тип,
    // которого здесь ещё нет.
    ByteWriter writer;
    writer.writeU8(200U);

    ByteReader reader{ByteView{writer.bytes()}};
    (void)readMValue(reader);

    CHECK_FALSE(reader.ok());
}

TEST_CASE("an overlong list fails the read instead of allocating", "[mvalue]") {
    // Присланная длина — чужая, и верить ей нельзя: список из миллиарда
    // элементов исчерпал бы память сервера одним пакетом.
    ByteWriter writer;
    writer.writeU8(static_cast<std::uint8_t>(MValue::Type::List));
    writer.writeU32(0xFFFFFFFFU);

    ByteReader reader{ByteView{writer.bytes()}};
    (void)readMValue(reader);

    CHECK_FALSE(reader.ok());
}

TEST_CASE("nesting deeper than allowed fails the read", "[mvalue]") {
    // Разбор вложенного значения рекурсивен, и список из десяти тысяч
    // открывающих скобок исчерпал бы стек — то есть один пакет от одного игрока
    // прекратил бы сессию для всех.
    ByteWriter writer;

    for (std::size_t depth = 0; depth <= MValue::kMaxDepth + 1U; ++depth) {
        writer.writeU8(static_cast<std::uint8_t>(MValue::Type::List));
        writer.writeU32(1U);
    }
    writer.writeU8(static_cast<std::uint8_t>(MValue::Type::Nil));

    ByteReader reader{ByteView{writer.bytes()}};
    (void)readMValue(reader);

    CHECK_FALSE(reader.ok());
}

TEST_CASE("nesting up to the limit still reads", "[mvalue]") {
    // Обратная сторона предыдущего: предел обязан быть достижимым, иначе он
    // запрещает не только нападение, но и обычное дерево страницы.
    MValue value = MValue::nil();

    for (std::size_t depth = 0; depth < MValue::kMaxDepth; ++depth) {
        MValue::List_ wrapper;
        wrapper.push_back(std::move(value));
        value = MValue::list(std::move(wrapper));
    }

    ByteWriter writer;
    writeMValue(writer, value);

    ByteReader reader{ByteView{writer.bytes()}};
    (void)readMValue(reader);

    CHECK(reader.ok());
}

TEST_CASE("a truncated value fails the read", "[mvalue]") {
    ByteWriter writer;
    writeMValue(writer, MValue::vector3(Vec3{1.0F, 2.0F, 3.0F}));

    // Обрезаем на середине: так выглядит потерянный по дороге хвост пакета.
    const std::vector<std::uint8_t>& bytes = writer.bytes();
    ByteReader reader{ByteView{bytes.data(), bytes.size() / 2U}};
    (void)readMValue(reader);

    CHECK_FALSE(reader.ok());
}
