#include <oxymp/shared/resource/source_kind.hpp>

#include <catch2/catch_test_macros.hpp>

using oxymp::shared::isGameDescription;
using oxymp::shared::isSourceOrMarkup;

TEST_CASE("the code of a mode is recognised wherever it lies") {
    CHECK(isSourceOrMarkup("client/index.js"));
    CHECK(isSourceOrMarkup("client/ui/app.mjs"));
    CHECK(isSourceOrMarkup("html/hud.html"));
    CHECK(isSourceOrMarkup("styles/main.css"));
    CHECK(isSourceOrMarkup("data/settings.json"));
}

TEST_CASE("a folder named stream does not turn code into a game asset") {
    // Ровно это и было поломкой: чужие режимы кладут свой интерфейс в
    // `stream/browsers/`, и прежнее правило оставляло его лежать у игрока
    // обычным текстом — три четверти всей клиентской логики режима.
    CHECK(isSourceOrMarkup("stream/browsers/accounts/main.js"));
    CHECK(isSourceOrMarkup("client/stream/browsers/anims/js/chiefslider.js"));
    CHECK(isSourceOrMarkup("ui/stream/browsers/shopclothesnew/index.html"));
}

TEST_CASE("what the game reads is not taken for source") {
    CHECK_FALSE(isSourceOrMarkup("stream/adder.yft"));
    CHECK_FALSE(isSourceOrMarkup("stream/adder.ytd"));
    CHECK_FALSE(isSourceOrMarkup("stream/props.ytyp"));
    CHECK_FALSE(isSourceOrMarkup("dlc.rpf"));
    CHECK_FALSE(isSourceOrMarkup("vehicles.meta"));
}

TEST_CASE("the suffix is read from the name, not from the path") {
    // Каталог с точкой в названии встречается чаще, чем кажется: так называют
    // версии. Прочитанное из пути окончание сделало бы исходником что угодно.
    CHECK(isSourceOrMarkup("assets/v1.2/app.js"));
    CHECK_FALSE(isSourceOrMarkup("assets/v1.js/adder.yft"));
}

TEST_CASE("a name without a suffix is not source") {
    CHECK_FALSE(isSourceOrMarkup("LICENSE"));
    CHECK_FALSE(isSourceOrMarkup("client/README"));
}

TEST_CASE("the suffix is read regardless of its case") {
    CHECK(isSourceOrMarkup("client/INDEX.JS"));
    CHECK(isGameDescription("VEHICLES.META"));
}

TEST_CASE("a game description names itself by its suffix") {
    CHECK(isGameDescription("vehicles.meta"));
    CHECK(isGameDescription("stream/handling.meta"));

    CHECK_FALSE(isGameDescription("client/index.js"));
    CHECK_FALSE(isGameDescription("meta"));
}
