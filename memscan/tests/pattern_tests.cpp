// Имена тестов намеренно латиницей: ctest передаёт их обратно в исполняемый файл
// как фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_test_macros.hpp>

#include <oxymp/memscan/scanner.hpp>

#include <cstdint>
#include <vector>

using namespace oxymp::memscan;

namespace {

ByteView view(const std::vector<std::uint8_t>& bytes) {
    return ByteView{bytes.data(), bytes.size()};
}

Pattern require(std::string_view text) {
    auto parsed = Pattern::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

} // namespace

TEST_CASE("parse accepts a plain signature", "[pattern]") {
    const Pattern pattern = require("48 8B C8");

    CHECK(pattern.size() == 3);
    CHECK(pattern.anchorIndex() == 0);
    CHECK(pattern.anchorByte() == 0x48);
}

TEST_CASE("parse is case insensitive", "[pattern]") {
    const std::vector<std::uint8_t> data{0xAB, 0xCD};

    CHECK(findFirst(view(data), require("ab cd")).has_value());
    CHECK(findFirst(view(data), require("AB CD")).has_value());
}

TEST_CASE("parse accepts both wildcard spellings", "[pattern]") {
    CHECK(require("48 ? C8").size() == 3);
    CHECK(require("48 ?? C8").size() == 3);
}

TEST_CASE("anchor is the first known byte", "[pattern]") {
    const Pattern pattern = require("? ? 8B C8");

    CHECK(pattern.anchorIndex() == 2);
    CHECK(pattern.anchorByte() == 0x8B);
}

TEST_CASE("parse rejects malformed signatures", "[pattern]") {
    SECTION("empty string") {
        CHECK_FALSE(Pattern::parse("").has_value());
    }
    SECTION("whitespace only") {
        CHECK_FALSE(Pattern::parse("   \t ").has_value());
    }
    SECTION("wildcards only leave nothing to search for") {
        CHECK_FALSE(Pattern::parse("? ?? ?").has_value());
    }
    SECTION("non-hex token") {
        CHECK_FALSE(Pattern::parse("48 ZZ").has_value());
    }
    SECTION("token that is not a byte pair is most likely a typo") {
        CHECK_FALSE(Pattern::parse("48 8BC8").has_value());
        CHECK_FALSE(Pattern::parse("4").has_value());
    }
}

TEST_CASE("scanner finds a match in the middle of a block", "[scanner]") {
    const std::vector<std::uint8_t> data{0x90, 0x90, 0x48, 0x8B, 0xC8, 0x90};

    const auto hits = findAll(view(data), require("48 8B C8"));

    REQUIRE(hits.size() == 1);
    CHECK(hits.front() == 2);
}

TEST_CASE("scanner handles matches at both block edges", "[scanner]") {
    const Pattern pattern = require("AA BB");

    SECTION("at the very beginning") {
        const std::vector<std::uint8_t> data{0xAA, 0xBB, 0x00, 0x00};
        CHECK(findFirst(view(data), pattern) == 0u);
    }
    SECTION("at the very end") {
        const std::vector<std::uint8_t> data{0x00, 0x00, 0xAA, 0xBB};
        CHECK(findFirst(view(data), pattern) == 2u);
    }
}

TEST_CASE("wildcard matches any byte", "[scanner]") {
    const std::vector<std::uint8_t> data{0x48, 0x11, 0xC8, 0x48, 0xFF, 0xC8};

    const auto hits = findAll(view(data), require("48 ? C8"));

    REQUIRE(hits.size() == 2);
    CHECK(hits[0] == 0);
    CHECK(hits[1] == 3);
}

TEST_CASE("signature starting with a wildcard is searched correctly", "[scanner]") {
    // Якорь смещён внутрь сигнатуры, поэтому совпадение в нулевой позиции —
    // отдельный краевой случай: наивная реализация уходит за начало блока.
    const std::vector<std::uint8_t> data{0x11, 0x8B, 0xC8};

    CHECK(findFirst(view(data), require("? 8B C8")) == 0u);
}

TEST_CASE("overlapping matches are all reported", "[scanner]") {
    const std::vector<std::uint8_t> data{0xAA, 0xAA, 0xAA};

    const auto hits = findAll(view(data), require("AA AA"));

    REQUIRE(hits.size() == 2);
    CHECK(hits[0] == 0);
    CHECK(hits[1] == 1);
}

TEST_CASE("limit stops the search early", "[scanner]") {
    const std::vector<std::uint8_t> data{0xAA, 0xAA, 0xAA, 0xAA};
    const Pattern pattern = require("AA");

    CHECK(findAll(view(data), pattern, 2).size() == 2);
    CHECK(findAll(view(data), pattern, 0).size() == 4);
}

TEST_CASE("no match yields an empty result", "[scanner]") {
    const std::vector<std::uint8_t> data{0x00, 0x01, 0x02};

    CHECK(findAll(view(data), require("48 8B")).empty());
    CHECK_FALSE(findFirst(view(data), require("48 8B")).has_value());
}

TEST_CASE("signature longer than the block never reads out of bounds", "[scanner]") {
    const std::vector<std::uint8_t> data{0x48, 0x8B};

    CHECK(findAll(view(data), require("48 8B C8")).empty());
}

TEST_CASE("empty block is handled without crashing", "[scanner]") {
    CHECK(findAll(ByteView{}, require("48")).empty());
}
