// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_test_macros.hpp>

#include <oxymp/shared/protocol/serialization.hpp>

#include <limits>
#include <string>

using namespace oxymp::shared;

namespace {

ByteReader readerOver(const ByteWriter& writer) {
    return ByteReader{ByteView{writer.bytes()}};
}

} // namespace

TEST_CASE("integers survive a round trip", "[serialization]") {
    ByteWriter writer;
    writer.writeU8(0xAB);
    writer.writeU16(0xBEEF);
    writer.writeU32(0xDEADBEEF);
    writer.writeU64(0x0123456789ABCDEFULL);

    ByteReader reader = readerOver(writer);

    CHECK(reader.readU8() == 0xAB);
    CHECK(reader.readU16() == 0xBEEF);
    CHECK(reader.readU32() == 0xDEADBEEF);
    CHECK(reader.readU64() == 0x0123456789ABCDEFULL);
    CHECK(reader.ok());
    CHECK(reader.exhausted());
}

TEST_CASE("integers are written least significant byte first", "[serialization]") {
    // Порядок байт зафиксирован протоколом, а не платформой, поэтому проверяется
    // по фактическому содержимому буфера.
    ByteWriter writer;
    writer.writeU32(0x11223344);

    const auto& bytes = writer.bytes();

    REQUIRE(bytes.size() == 4);
    CHECK(bytes[0] == 0x44);
    CHECK(bytes[1] == 0x33);
    CHECK(bytes[2] == 0x22);
    CHECK(bytes[3] == 0x11);
}

TEST_CASE("floats and vectors survive a round trip", "[serialization]") {
    const Vec3 position{-1234.5F, 0.0F, 987.25F};

    ByteWriter writer;
    writer.writeFloat(3.14159F);
    writer.writeVec3(position);

    ByteReader reader = readerOver(writer);

    CHECK(reader.readFloat() == 3.14159F);
    CHECK(reader.readVec3() == position);
    CHECK(reader.ok());
}

TEST_CASE("strings survive a round trip", "[serialization]") {
    ByteWriter writer;
    writer.writeString("oxy");
    writer.writeString("");

    ByteReader reader = readerOver(writer);

    CHECK(reader.readString() == "oxy");
    CHECK(reader.readString().empty());
    CHECK(reader.ok());
    CHECK(reader.exhausted());
}

TEST_CASE("overlong strings are truncated on write", "[serialization]") {
    const std::string huge(kMaxStringLength * 4, 'x');

    ByteWriter writer;
    writer.writeString(huge);

    ByteReader reader = readerOver(writer);
    const std::string restored = reader.readString();

    CHECK(reader.ok());
    CHECK(restored.size() == kMaxStringLength);
}

TEST_CASE("reading past the end fails instead of crashing", "[serialization]") {
    ByteWriter writer;
    writer.writeU8(1);

    ByteReader reader = readerOver(writer);

    CHECK(reader.readU8() == 1);
    CHECK(reader.ok());

    CHECK(reader.readU32() == 0);
    CHECK_FALSE(reader.ok());
}

TEST_CASE("failure is sticky once raised", "[serialization]") {
    // Иначе разбор пришлось бы проверять после каждого поля, а не один раз в конце.
    ByteReader reader{ByteView{}};

    CHECK(reader.readU8() == 0);
    CHECK_FALSE(reader.ok());

    CHECK(reader.readU8() == 0);
    CHECK_FALSE(reader.ok());
}

TEST_CASE("a string claiming an impossible length is rejected", "[serialization]") {
    // Пакет собран вручную: длина больше предела, но данных за ней нет.
    ByteWriter writer;
    writer.writeU16(std::numeric_limits<std::uint16_t>::max());

    ByteReader reader = readerOver(writer);

    CHECK(reader.readString().empty());
    CHECK_FALSE(reader.ok());
}

TEST_CASE("a truncated string is rejected", "[serialization]") {
    ByteWriter writer;
    writer.writeU16(16); // обещаем 16 байт
    writer.writeU8('a'); // а даём один

    ByteReader reader = readerOver(writer);

    CHECK(reader.readString().empty());
    CHECK_FALSE(reader.ok());
}

TEST_CASE("exhausted reports leftover bytes", "[serialization]") {
    ByteWriter writer;
    writer.writeU16(0);
    writer.writeU8(0);

    ByteReader reader = readerOver(writer);

    CHECK(reader.readU16() == 0);
    CHECK_FALSE(reader.exhausted());
    CHECK(reader.remaining() == 1);

    CHECK(reader.readU8() == 0);
    CHECK(reader.exhausted());
}
