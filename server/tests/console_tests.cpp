// Разбор строки, набранной в окне сервера.
//
// Проверяется здесь только разбор, а не чтение: чтение стоит внутри потока,
// ждущего перевода строки, и поднимать его в наборе значило бы завести проверку,
// которая ждёт человека. Разбор же — чистая работа над строкой, и ошибка в нём
// стоит того же, что и всякая ошибка в доводах: команда режима срабатывает не
// так, как её набрали, и не жалуется.

#include "../src/console.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

using oxymp::server::splitCommand;

} // namespace

TEST_CASE("a command is split into its name and arguments", "[server][console]") {
    const auto parts = splitCommand("give oxy weapon_pistol 50");

    REQUIRE(parts.size() == 4);
    CHECK(parts[0] == "give");
    CHECK(parts[1] == "oxy");
    CHECK(parts[2] == "weapon_pistol");
    CHECK(parts[3] == "50");
}

TEST_CASE("spaces typed by a human do not become arguments", "[server][console]") {
    // Набранное человеком редко бывает ровным, а пустой довод посреди команды
    // режим принял бы за настоящий — и выдал бы оружие никому.
    const auto parts = splitCommand("   kick    oxy   ");

    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "kick");
    CHECK(parts[1] == "oxy");
}

TEST_CASE("a command without arguments is still a command", "[server][console]") {
    const auto parts = splitCommand("stop");

    REQUIRE(parts.size() == 1);
    CHECK(parts[0] == "stop");
}

TEST_CASE("an empty line is no command at all", "[server][console]") {
    CHECK(splitCommand("").empty());
    CHECK(splitCommand("     ").empty());
    CHECK(splitCommand("\t").empty());
}

TEST_CASE("a carriage return from a foreign line ending is cut off",
          "[server][console]") {
    // Набранное в Windows и переданное через канал приходит с возвратом
    // каретки. Останься он — команда «stop\r» не совпала бы со «stop» ни разу,
    // и хозяин сервера искал бы причину в режиме.
    const auto parts = splitCommand("stop\r");

    REQUIRE(parts.size() == 1);
    CHECK(parts[0] == "stop");

    const auto withArguments = splitCommand("kick oxy\r");

    REQUIRE(withArguments.size() == 2);
    CHECK(withArguments[1] == "oxy");

    // Строка из одного возврата каретки — не команда: обрезав его, не остаётся
    // ничего.
    CHECK(splitCommand("\r").empty());
}

TEST_CASE("a byte order mark from a pipe is cut off", "[server][console]") {
    // Из живой консоли она не приходит никогда, а из канала — запросто: так
    // пишет файлы PowerShell. Замечено на живом сервере: первая команда за
    // запуск пришла с ней, и увидеть это в журнале нельзя — метка невидима.
    const auto parts = splitCommand("\xEF\xBB\xBF" "look");

    REQUIRE(parts.size() == 1);
    CHECK(parts[0] == "look");

    // Только в начале и только один раз: посреди строки это обычные байты, и
    // выкусывать их оттуда не наше дело.
    const auto withArguments = splitCommand("\xEF\xBB\xBF" "give oxy 100");

    REQUIRE(withArguments.size() == 3);
    CHECK(withArguments[0] == "give");
}

TEST_CASE("tabs separate arguments just like spaces", "[server][console]") {
    const auto parts = splitCommand("give\toxy\t100");

    REQUIRE(parts.size() == 3);
    CHECK(parts[0] == "give");
    CHECK(parts[2] == "100");
}
