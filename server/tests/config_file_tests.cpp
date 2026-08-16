#include "config_file.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace oxymp;
using namespace oxymp::server;
using Catch::Approx;

namespace {

/// Разбирает текст и прикладывает его к настройкам.
///
/// Возвращает список непонятого — то же, что вернул бы сервер при запуске.
std::vector<std::string> settle(std::string_view text, Config& config) {
    config_file::Entries entries;

    std::string error;
    REQUIRE(config_file::parse(text, entries, error));

    return config_file::apply(entries, config);
}

} // namespace

TEST_CASE("the config file reads what it is told", "[config]") {
    Config config;

    const auto unknown = settle(
        "name: Тестовый\n"
        "port = 30120\n"
        "maxplayers: 64\n"
        "spawn: -1037.7, -2738.0, 20.2\n"
        "spawnheading: 328.5\n"
        "streamdistance: 400\n"
        "weather: THUNDER\n"
        "time: 21:30\n"
        "admins: 0, 3, 7\n",
        config);

    CHECK(unknown.empty());
    CHECK(config.name == "Тестовый");
    CHECK(config.port == 30120);
    CHECK(config.maxPlayers == 64);

    // Точка разбирается по классической локали, а не по местной. В русской
    // локали разделителем служит запятая, и «-1037.7» стало бы минус тысячей
    // тридцатью семью — то есть игрок появился бы в километре от места.
    CHECK(config.spawnPosition.x == Approx(-1037.7F));
    CHECK(config.spawnPosition.y == Approx(-2738.0F));
    CHECK(config.spawnPosition.z == Approx(20.2F));
    CHECK(config.spawnHeading == Approx(328.5F));

    CHECK(config.streamDistance == Approx(400.0F));
    CHECK(config.weather == "THUNDER");
    CHECK(config.startingHour == 21);
    CHECK(config.startingMinute == 30);

    REQUIRE(config.admins.size() == 3);
    CHECK(config.admins[0] == 0);
    CHECK(config.admins[2] == 7);
}

TEST_CASE("an empty password key leaves the server open", "[config]") {
    // Ключ без значения — то, что стоит в server.cfg по умолчанию, и он обязан
    // означать «пароля нет», а не «пароль из пустой строки»: сервер сверяет
    // пароль только у непустой настройки.
    Config config;

    const auto unknown = settle("password:\n", config);

    CHECK(unknown.empty());
    CHECK(config.password.empty());
}

TEST_CASE("the password key is read as it is written", "[config]") {
    Config config;

    const auto unknown = settle("password: под ёлкой\n", config);

    CHECK(unknown.empty());
    CHECK(config.password == "под ёлкой");
}

TEST_CASE("comments and blank lines are ignored", "[config]") {
    Config config;

    const auto unknown = settle(
        "# всё это примечание\n"
        "\n"
        "   port: 22006   # и это тоже\n"
        "\n",
        config);

    CHECK(unknown.empty());
    CHECK(config.port == 22006);
}

TEST_CASE("a hash inside quotes is not a comment", "[config]") {
    Config config;

    // Иначе имя сервера с решёткой обрезалось бы на ней, и хозяин долго
    // выяснял бы, куда делась половина названия.
    const auto unknown = settle("name: \"oxyMP #1\"\n", config);

    CHECK(unknown.empty());
    CHECK(config.name == "oxyMP #1");
}

TEST_CASE("the key name is case insensitive", "[config]") {
    Config config;

    // Человек, пишущий файл руками, не обязан помнить, как мы решили назвать
    // настройку.
    const auto unknown = settle("MaxPlayers: 16\nSTREAMDISTANCE: 250\n", config);

    CHECK(unknown.empty());
    CHECK(config.maxPlayers == 16);
    CHECK(config.streamDistance == Approx(250.0F));
}

TEST_CASE("a line without a separator is refused", "[config]") {
    config_file::Entries entries;
    std::string error;

    // Такую строку нельзя истолковать однозначно, и догадываться о ней хуже,
    // чем отказаться: хозяин узнает об опечатке при запуске, а не через неделю.
    CHECK_FALSE(config_file::parse("port 22005\n", entries, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("unknown keys are reported but do not stop the server", "[config]") {
    Config config;

    // Опечатка в имени ключа — самая частая беда таких файлов. Промолчать о ней
    // хуже, чем пожаловаться; отказываться запускаться — тоже хуже.
    const auto unknown = settle("maxplayer: 16\nport: 22005\n", config);

    REQUIRE(unknown.size() == 1);
    CHECK(unknown.front().find("maxplayer") != std::string::npos);

    // Соседняя настройка при этом прочиталась.
    CHECK(config.port == 22005);
}

TEST_CASE("a known key with a broken value is reported apart", "[config]") {
    Config config;
    const std::uint16_t before = config.port;

    // Не то же, что опечатка в имени: ключ понятен, непонятно значение. Сказать
    // об этом нужно иначе, чтобы хозяин искал ошибку там, где она есть.
    const auto unknown = settle("port: тридцать\ntime: 25:00\n", config);

    CHECK(unknown.size() == 2);
    CHECK(config.port == before);
}

TEST_CASE("the resources key is a list of names", "[config]") {
    Config config;

    const auto unknown = settle("resourcedirectory: ресурсы\n"
                                "resources: commands, freeroam\n"
                                "gamefiles: файлы/игры\n",
                                config);

    CHECK(unknown.empty());
    CHECK(config.resourceDirectory == "ресурсы");
    CHECK(config.gameFilesDirectory == "файлы/игры");

    // Порядок сохраняется, и он значим: подписавшийся первым первым и получит
    // возможность отменить реплику в чате.
    REQUIRE(config.resources.size() == 2);
    CHECK(config.resources.front() == "commands");
    CHECK(config.resources.back() == "freeroam");
}

TEST_CASE("the old meaning of the resources key is recognised", "[config]") {
    Config config;

    // Раньше здесь стоял путь к игровым файлам. Прочитай мы его как имя ресурса,
    // сервер пожаловался бы на пропавший каталог и перестал бы раздавать файлы —
    // а связать одно с другим хозяину было бы не по чему.
    const auto unknown = settle("resources: resources/dlcpacks\n", config);

    CHECK(config.gameFilesDirectory == "resources/dlcpacks");
    CHECK(config.resources.empty());

    REQUIRE(unknown.size() == 1);
    CHECK(unknown.front().find("gamefiles") != std::string::npos);
}

TEST_CASE("a file saved by Notepad is read the same", "[config]") {
    Config config;

    // Блокнот, сохраняя в UTF-8, ставит в начало метку порядка байтов. Оставь мы
    // её — она прилипнет к имени первой настройки, и та перестанет узнаваться:
    // сервер пожалуется на непонятный ключ, показав его в журнале ровно так же,
    // как он написан в файле. Искать эту разницу глазами можно долго.
    const auto unknown = settle("\xEF\xBB\xBF" "port: 30120\n", config);

    CHECK(unknown.empty());
    CHECK(config.port == 30120);
}
