// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include <catch2/catch_test_macros.hpp>

#include <oxymp/config/skin.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using oxymp::config::Skin;

namespace {

class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view contents) {
        path_ = std::filesystem::temp_directory_path() /
                ("oxymp-skin-" + std::to_string(++counter_) + ".bin");

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

/// Образец в точности того вида, в каком его кладёт alt:V.
constexpr std::string_view kSample = R"({
    "customUiGameAppName": "Santa Maria RP",
    "customUiLauncherAppName": "Santa Maria RP",
    "customUiImageAppIcon": "AAABAAEA",
    "customUiUrl": [],
    "installerBackground": "iVBORw0K",
    "isRssHidden": true,
    "launcherBackground": "iVBORw0K",
    "logo": "iVBORw0KGgo",
    "primaryColor": "#FF6F5F",
    "rss": "https://cdn.alt-mp.com/rss/news-short.rss",
    "servers": [
        {
            "id": "smrp",
            "name": "Santa Maria Role Play | 1",
            "vanityUrl": "smrp",
            "ip": "connect.santamaria-rp.com"
        }
    ],
    "uiBackground": "iVBORw0KGgoA"
})";

} // namespace

TEST_CASE("a missing skin file is not an error", "[skin]") {
    REQUIRE_FALSE(
        Skin::load(std::filesystem::temp_directory_path() / "oxymp-no-such-skin.bin").has_value());
}

TEST_CASE("a broken skin file is refused rather than half read", "[skin]") {
    const TemporaryFile file{"{ this is not json"};

    REQUIRE_FALSE(Skin::load(file.path()).has_value());
}

TEST_CASE("a skin file is read the way alt writes it", "[skin]") {
    const TemporaryFile file{kSample};

    const std::optional<Skin> skin = Skin::load(file.path());
    REQUIRE(skin.has_value());

    REQUIRE(skin->gameName == "Santa Maria RP");
    REQUIRE(skin->launcherName == "Santa Maria RP");
    REQUIRE(skin->logo == "iVBORw0KGgo");
    REQUIRE(skin->uiBackground == "iVBORw0KGgoA");
    REQUIRE(skin->rssHidden);

    REQUIRE(skin->servers.size() == 1);
    REQUIRE(skin->servers.front().id == "smrp");
    REQUIRE(skin->servers.front().address == "connect.santamaria-rp.com");
}

TEST_CASE("the hash is stripped from the primary color", "[skin]") {
    const TemporaryFile file{kSample};

    const std::optional<Skin> skin = Skin::load(file.path());
    REQUIRE(skin.has_value());

    // Страница отказывается от цвета, если знаков не ровно шесть, — а в файле их
    // семь вместе с решёткой.
    REQUIRE(skin->primaryColor == "FF6F5F");
    REQUIRE(skin->primaryColor.size() == 6);
}

TEST_CASE("a server without an address is left out", "[skin]") {
    const TemporaryFile file{R"({"servers":[{"id":"a","name":"A"},{"id":"b","ip":"host:1"}]})"};

    const std::optional<Skin> skin = Skin::load(file.path());
    REQUIRE(skin.has_value());

    REQUIRE(skin->servers.size() == 1);
    REQUIRE(skin->servers.front().id == "b");
}

TEST_CASE("the manifest carries what the page asks for and nothing else", "[skin]") {
    const TemporaryFile file{kSample};

    const std::optional<Skin> skin = Skin::load(file.path());
    REQUIRE(skin.has_value());

    const std::string manifest = skin->toManifestJson();

    REQUIRE(manifest.find(R"("name":"Santa Maria RP")") != std::string::npos);
    REQUIRE(manifest.find(R"("primaryColor":"FF6F5F")") != std::string::npos);
    REQUIRE(manifest.find(R"("url":"connect.santamaria-rp.com")") != std::string::npos);

    // Скрытая лента до страницы не доходит вовсе, а не приходит скрытой:
    // признака «скрыта» она не знает.
    REQUIRE(manifest.find("alt-mp.com") == std::string::npos);

    // Хозяйство лаунчера странице не нужно и не отдаётся.
    REQUIRE(manifest.find("launcherBackground") == std::string::npos);
    REQUIRE(manifest.find("customUiImageAppIcon") == std::string::npos);
}
