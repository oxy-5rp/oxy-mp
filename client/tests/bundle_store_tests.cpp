#include "../src/bundle_store.hpp"

#include <oxymp/shared/resource/bundle.hpp>
#include <oxymp/shared/resource/vault.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using oxymp::client::BundleStore;
using oxymp::shared::Bundle;

namespace {

/// Свой каталог на проверку и уборка за собой.
///
/// Каталог общий на весь набор был бы источником взаимных помех: проверки идут
/// каждая своим процессом, но кеш у них один, и свёрток от соседней остался бы
/// лежать.
class Scratch {
public:
    Scratch() {
        path_ = std::filesystem::temp_directory_path() /
                ("oxymp-bundle-" + std::to_string(token()) + "-" + std::to_string(counter()));

        std::filesystem::remove_all(path_);
    }

    ~Scratch() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    [[nodiscard]] static int counter() {
        static int next = 0;
        return ++next;
    }

    /// Одно число на запуск: наборы идут разными процессами, а каталог у них
    /// общий, и одинаковое имя означало бы уборку за соседом посреди его работы.
    [[nodiscard]] static unsigned token() {
        static const unsigned value = std::random_device{}();
        return value;
    }

    std::filesystem::path path_;
};

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string text(const std::vector<std::uint8_t>& data) {
    return {data.begin(), data.end()};
}

/// Свёрток из трёх файлов: точка входа, её сосед и страница.
std::vector<std::uint8_t> sampleBundle() {
    const std::vector<Bundle::File> files{
        Bundle::File{.path = "client/index.js", .contents = bytes("import './utils.js';")},
        Bundle::File{.path = "client/utils.js", .contents = bytes("export const a = 1;")},
        Bundle::File{.path = "html/hud.html", .contents = bytes("<html>привет</html>")},
    };

    return Bundle::pack(files, oxymp::shared::Vault::builtInKey());
}

} // namespace

TEST_CASE("a bundle kept in the store is read back file by file") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> packed = sampleBundle();
    const std::string hash = oxymp::shared::fingerprint(packed);

    REQUIRE(store.keep(hash, packed));
    REQUIRE(store.has(hash));
    REQUIRE(store.bind("mode", hash, oxymp::shared::Vault::builtInKey()));

    std::vector<std::uint8_t> contents;

    REQUIRE(store.read("mode", "client/index.js", contents));
    CHECK(text(contents) == "import './utils.js';");

    REQUIRE(store.read("mode", "client/utils.js", contents));
    CHECK(text(contents) == "export const a = 1;");

    REQUIRE(store.read("mode", "html/hud.html", contents));
    CHECK(text(contents) == "<html>привет</html>");
}

TEST_CASE("the bundle on disk never holds the sources in the clear") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> packed = sampleBundle();
    const std::string hash = oxymp::shared::fingerprint(packed);

    REQUIRE(store.keep(hash, packed));

    std::ifstream file{store.pathOf(hash), std::ios::binary};
    REQUIRE(file);

    const std::string raw{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    // Ни содержимого, ни даже имён: оглавление зашифровано вместе с телом. Это и
    // есть то, ради чего свёрток заведён, — поэтому проверяется, а не
    // подразумевается.
    CHECK(raw.find("export const a = 1;") == std::string::npos);
    CHECK(raw.find("client/index.js") == std::string::npos);
}

TEST_CASE("a path written the Windows way finds the same file") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> packed = sampleBundle();
    const std::string hash = oxymp::shared::fingerprint(packed);

    REQUIRE(store.keep(hash, packed));
    REQUIRE(store.bind("mode", hash, oxymp::shared::Vault::builtInKey()));

    std::vector<std::uint8_t> contents;

    CHECK(store.read("mode", "client\\index.js", contents));
    CHECK(store.read("mode", "./client/index.js", contents));
    CHECK(store.read("mode", "/client/index.js", contents));
}

TEST_CASE("a file that is not in the bundle refuses instead of returning nothing") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> packed = sampleBundle();
    const std::string hash = oxymp::shared::fingerprint(packed);

    REQUIRE(store.keep(hash, packed));
    REQUIRE(store.bind("mode", hash, oxymp::shared::Vault::builtInKey()));

    std::vector<std::uint8_t> contents;

    CHECK_FALSE(store.read("mode", "client/missing.js", contents));
    CHECK_FALSE(store.read("other", "client/index.js", contents));
}

TEST_CASE("a bundle cut short is not taken for a whole one") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> packed = sampleBundle();
    const std::string hash = oxymp::shared::fingerprint(packed);

    REQUIRE(store.keep(hash, packed));

    // Оборванная закачка оставляет файл верного имени и неверной длины. Имя
    // сходится, и без проверки длины клиент считал бы его готовым навсегда.
    std::filesystem::resize_file(store.pathOf(hash), packed.size() / 2);

    CHECK_FALSE(store.has(hash));
    CHECK_FALSE(store.bind("mode", hash, oxymp::shared::Vault::builtInKey()));
}

TEST_CASE("the store names what a resource brought with it") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> packed = sampleBundle();
    const std::string hash = oxymp::shared::fingerprint(packed);

    REQUIRE(store.keep(hash, packed));

    CHECK_FALSE(store.knows("mode"));

    REQUIRE(store.bind("mode", hash, oxymp::shared::Vault::builtInKey()));

    CHECK(store.knows("mode"));

    const std::vector<std::string> paths = store.list("mode");

    REQUIRE(paths.size() == 3);
    CHECK(paths[0] == "client/index.js");
    CHECK(paths[1] == "client/utils.js");
    CHECK(paths[2] == "html/hud.html");
}

TEST_CASE("a bundle nobody has used for a long time leaves the cache") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> mine = sampleBundle();
    const std::string used = oxymp::shared::fingerprint(mine);

    // Второй свёрток — другого содержимого, а значит и другого имени: старый
    // свёрток брошенного режима выглядит ровно так.
    const std::vector<Bundle::File> other{
        Bundle::File{.path = "client/index.js", .contents = bytes("// прошлая сборка")},
    };

    const std::vector<std::uint8_t> abandoned =
        Bundle::pack(other, oxymp::shared::Vault::builtInKey());
    const std::string forgotten = oxymp::shared::fingerprint(abandoned);

    REQUIRE(store.keep(used, mine));
    REQUIRE(store.keep(forgotten, abandoned));

    // Оба состарены, и это существенно: без этого проверка сказала бы лишь, что
    // свежее не трогают, — а спрашивается здесь другое.
    const auto ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours{24 * 30};

    std::filesystem::last_write_time(store.pathOf(used), ago);
    std::filesystem::last_write_time(store.pathOf(forgotten), ago);

    // Привязка — это и есть пользование: она обновляет время записи.
    REQUIRE(store.bind("mode", used, oxymp::shared::Vault::builtInKey()));

    store.prune(std::chrono::hours{24 * 14});

    CHECK(std::filesystem::exists(store.pathOf(used)));
    CHECK_FALSE(std::filesystem::exists(store.pathOf(forgotten)));
}

TEST_CASE("a bundle used the other day stays where it is") {
    const Scratch scratch;
    BundleStore store{scratch.path()};

    const std::vector<std::uint8_t> packed = sampleBundle();
    const std::string hash = oxymp::shared::fingerprint(packed);

    REQUIRE(store.keep(hash, packed));

    // Ни к чему не привязан, но и не стар: человек играет на нескольких серверах
    // и возвращается на прежний. Стереть здесь значило бы качать заново.
    store.prune(std::chrono::hours{24 * 14});

    CHECK(std::filesystem::exists(store.pathOf(hash)));
}
