// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_test_macros.hpp>

#include <oxymp/config/settings.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using oxymp::config::Settings;

namespace {

/// Кладёт файл во временный каталог и убирает его за собой.
class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view contents) {
        path_ = std::filesystem::temp_directory_path() /
                ("oxymp-settings-" + std::to_string(++counter_) + ".toml");

        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        out << contents;
    }

    ~TemporaryFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;

    static inline int counter_ = 0;
};

} // namespace

TEST_CASE("a missing file gives the defaults", "[settings]") {
    const Settings settings =
        Settings::load(std::filesystem::temp_directory_path() / "oxymp-no-such-file.toml");

    REQUIRE(settings.text("name") == "Player");
    REQUIRE(settings.number("uiVolume") == 100);
    REQUIRE(settings.flag("voiceEnabled"));
}

TEST_CASE("a broken file gives the defaults instead of failing", "[settings]") {
    const TemporaryFile file{"name = = = 'nonsense'"};

    const Settings settings = Settings::load(file.path());

    REQUIRE(settings.text("name") == "Player");
}

TEST_CASE("a file fills in over the defaults", "[settings]") {
    const TemporaryFile file{"name = 'oxy'\nuiVolume = 42\n"};

    const Settings settings = Settings::load(file.path());

    REQUIRE(settings.text("name") == "oxy");
    REQUIRE(settings.number("uiVolume") == 42);

    // Чего в файле не было, берётся из умолчаний, а не обнуляется.
    REQUIRE(settings.number("voiceVolume") == 200);
}

TEST_CASE("the game language is empty until it is chosen", "[settings]") {
    // Пусто означает «как решит лаунчер Rockstar»: не выбрав языка, игрок
    // получает ту же игру, что и без нас. Умолчание здесь — половина смысла
    // настройки, поэтому оно и проверяется.
    const Settings untouched;
    REQUIRE(untouched.text("gameLanguage").empty());

    const TemporaryFile file{"gameLanguage = 'en-US'\n"};

    const Settings chosen = Settings::load(file.path());
    REQUIRE(chosen.text("gameLanguage") == "en-US");
}

TEST_CASE("an unknown setting in the file is ignored", "[settings]") {
    const TemporaryFile file{"whatEvenIsThis = 'x'\nname = 'oxy'\n"};

    const Settings settings = Settings::load(file.path());

    REQUIRE(settings.text("name") == "oxy");
    REQUIRE(settings.text("whatEvenIsThis").empty());
}

TEST_CASE("a known setting of the wrong kind in the file keeps its default",
          "[settings]") {
    // Файл синтаксически верен — разбор его не отвергает целиком, — но
    // булево поле называет строкой. Молчаливое принятие подменило бы
    // умолчание значением, которое flag() потом читает всё равно как false,
    // — то самое «испорченный файл — не беда, получаются умолчания»,
    // которое здесь тихо переставало бы держаться.
    const TemporaryFile file{"crashReporterEnabled = 'yes'\nname = 'oxy'\n"};

    const Settings settings = Settings::load(file.path());

    REQUIRE(settings.flag("crashReporterEnabled"));
    REQUIRE(settings.text("name") == "oxy");
}

TEST_CASE("settings survive a trip through the file", "[settings]") {
    const TemporaryFile file{""};

    {
        Settings written;
        written.set("name", std::string_view{"oxy"});
        written.set("uiVolume", std::int64_t{7});
        written.set("consoleHeight", 0.25);
        written.set("voiceEnabled", false);

        REQUIRE(written.save(file.path()));
    }

    const Settings read = Settings::load(file.path());

    REQUIRE(read.text("name") == "oxy");
    REQUIRE(read.number("uiVolume") == 7);
    REQUIRE(read.fraction("consoleHeight") == 0.25);
    REQUIRE_FALSE(read.flag("voiceEnabled"));
}

TEST_CASE("the page sees its own names for the renamed settings", "[settings]") {
    Settings settings;
    settings.set("language", std::string_view{"ru"});

    // Страница зовёт эту настройку language, файл — lang. Читаться она должна
    // обоими именами, а записываться под тем, что в файле.
    REQUIRE(settings.text("language") == "ru");
    REQUIRE(settings.text("lang") == "ru");

    const std::string json = settings.toJson();

    REQUIRE(json.find("\"language\"") != std::string::npos);
    REQUIRE(json.find("\"lang\"") == std::string::npos);
}

TEST_CASE("a change from the page is accepted by its json spelling", "[settings]") {
    Settings settings;

    REQUIRE(settings.applyJson("name", "\"oxy\""));
    REQUIRE(settings.text("name") == "oxy");

    REQUIRE(settings.applyJson("voiceEnabled", "false"));
    REQUIRE_FALSE(settings.flag("voiceEnabled"));

    REQUIRE(settings.applyJson("uiVolume", "42"));
    REQUIRE(settings.number("uiVolume") == 42);

    REQUIRE(settings.applyJson("launcherSkinsDisabled", "[\"a\",\"b\"]"));
    REQUIRE(settings.list("launcherSkinsDisabled") == std::vector<std::string>{"a", "b"});
}

TEST_CASE("the page cannot change a setting's kind either", "[settings]") {
    Settings settings;

    // Та же защита, что и у чтения файла, с той же причиной: страница —
    // не единственный источник правки в проекте, а испорченный или
    // рассинхронизировавшийся код на ней не должен тихо подменять вид
    // булева поля строкой.
    REQUIRE_FALSE(settings.applyJson("voiceEnabled", "\"да\""));
    REQUIRE(settings.flag("voiceEnabled"));
}

TEST_CASE("a whole number for a fractional setting stays fractional", "[settings]") {
    Settings settings;

    // В JavaScript единица и единица с нулём — одно и то же число, и страница
    // шлёт именно `1`. Принять его целым значило бы испортить вид настройки.
    REQUIRE(settings.applyJson("consoleHeight", "1"));
    REQUIRE(settings.fraction("consoleHeight") == 1.0);

    const TemporaryFile file{""};
    REQUIRE(settings.save(file.path()));

    REQUIRE(Settings::load(file.path()).fraction("consoleHeight") == 1.0);
}

TEST_CASE("an unknown change from the page is refused", "[settings]") {
    Settings settings;

    REQUIRE_FALSE(settings.applyJson("whatEvenIsThis", "1"));
    REQUIRE_FALSE(settings.applyJson("name", "{ this is not json"));
}
