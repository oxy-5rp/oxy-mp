#include "world_clock.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace oxymp;
using namespace oxymp::server;

TEST_CASE("the world clock starts where it was told to", "[world]") {
    const WorldClock clock{"THUNDER", 21, 30};

    const shared::WorldState state = clock.snapshot();

    CHECK(state.weather == "THUNDER");
    CHECK(state.hour == 21);
    CHECK(state.minute == 30);
    CHECK(state.second == 0);
}

TEST_CASE("the world clock refuses impossible times", "[world]") {
    WorldClock clock{"CLEAR", 12, 0};

    CHECK_FALSE(clock.setTime(24, 0, 0));
    CHECK_FALSE(clock.setTime(12, 60, 0));
    CHECK_FALSE(clock.setTime(12, 0, 60));

    // Отвергнутое время не должно частично примениться: час из негодной тройки
    // так же негоден, как и она сама.
    CHECK(clock.snapshot().hour == 12);

    REQUIRE(clock.setTime(23, 59, 59));

    const shared::WorldState state = clock.snapshot();
    CHECK(state.hour == 23);
    CHECK(state.minute == 59);
    CHECK(state.second == 59);
}

TEST_CASE("the world clock refuses implausible weather", "[world]") {
    WorldClock clock{"CLEAR", 12, 0};

    // Названия у игры заглавными латинскими буквами и без пробелов. Строка,
    // собранная из чего угодно, уходит прямо в нативы, и лучше ей туда не
    // попадать.
    CHECK_FALSE(clock.setWeather(""));
    CHECK_FALSE(clock.setWeather("extrasunny"));
    CHECK_FALSE(clock.setWeather("EXTRA SUNNY"));
    CHECK_FALSE(clock.setWeather(std::string(shared::kMaxWeatherLength + 1, 'A')));

    CHECK(clock.snapshot().weather == "CLEAR");

    REQUIRE(clock.setWeather("EXTRASUNNY"));
    CHECK(clock.snapshot().weather == "EXTRASUNNY");
}

TEST_CASE("setting the time asks to tell everyone at once", "[world]") {
    WorldClock clock{"CLEAR", 12, 0};

    // Первый ход часов рассылки не требует: с прошлой прошло меньше положенного.
    CHECK_FALSE(clock.advance());

    // А вот перевод часов виден игроку сразу, и ждать до очередной рассылки —
    // значит показать ему, что распоряжение не сработало.
    REQUIRE(clock.setTime(3, 0, 0));
    CHECK(clock.advance());
}

TEST_CASE("the world clock wraps around midnight", "[world]") {
    WorldClock clock{"CLEAR", 23, 59};

    REQUIRE(clock.setTime(23, 59, 59));
    CHECK(clock.snapshot().hour == 23);

    // Сутки замкнуты: за полночью идёт ноль часов, а не двадцать четыре. Без
    // этого час вылез бы за границу байта и за границу того, что примет игра.
    for (int step = 0; step < 100; ++step) {
        clock.advance();
    }

    CHECK(clock.snapshot().hour <= 23);
}
