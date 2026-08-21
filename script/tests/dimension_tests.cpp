#include <oxymp/script/dimension.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace oxymp::script;

TEST_CASE("those in the same dimension see each other") {
    CHECK(dimensionsMeet(kDefaultDimension, kDefaultDimension));
    CHECK(dimensionsMeet(7, 7));
    CHECK(dimensionsMeet(-3, -3));
}

TEST_CASE("those in different dimensions do not see each other") {
    CHECK_FALSE(dimensionsMeet(kDefaultDimension, 1));
    CHECK_FALSE(dimensionsMeet(7, 8));

    // Отрицательные — такие же измерения, как и остальные: у alt:V они означают
    // «частное», но частность эта в видимости ничего не меняет.
    CHECK_FALSE(dimensionsMeet(-3, 3));
}

TEST_CASE("the global dimension sees everyone and is seen by everyone") {
    CHECK(dimensionsMeet(kGlobalDimension, kDefaultDimension));
    CHECK(dimensionsMeet(kDefaultDimension, kGlobalDimension));
    CHECK(dimensionsMeet(kGlobalDimension, 12345));
    CHECK(dimensionsMeet(-999, kGlobalDimension));
    CHECK(dimensionsMeet(kGlobalDimension, kGlobalDimension));
}

TEST_CASE("the numbers are the ones alt:V uses") {
    // Разойтись здесь нельзя: режим, написанный под alt:V, пишет эти числа
    // прямо в коде, и другое значение сломало бы его молча.
    CHECK(kDefaultDimension == 0);
    CHECK(kGlobalDimension == -2147483648LL);
}

TEST_CASE("visibility works both ways or not at all") {
    // Несимметричная видимость означает игрока, который стреляет в того, кто
    // его не видит. Проверяется прямо, потому что ошибиться здесь легко.
    constexpr std::int32_t kSpread[] = {kGlobalDimension, -5, kDefaultDimension, 5, 2147483647};

    for (const std::int32_t left : kSpread) {
        for (const std::int32_t right : kSpread) {
            CHECK(dimensionsMeet(left, right) == dimensionsMeet(right, left));
        }
    }
}
