// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include "resource_catalog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string_view>

using namespace oxymp;
using namespace oxymp::server;

namespace {

/// Каталог ресурсов на диске, убирающий за собой.
///
/// Без диска здесь не обойтись, и это тот редкий случай, когда так и должно
/// быть: проверяется ровно чтение с диска — есть ли каталог, на месте ли файлы,
/// не уводит ли путь наружу. Подставить сюда нечего.
class Sandbox {
public:
    Sandbox() {
        root_ = std::filesystem::temp_directory_path() /
                std::format("oxymp-resources-{}", std::chrono::steady_clock::now()
                                                      .time_since_epoch()
                                                      .count());

        std::filesystem::create_directories(root_);
    }

    ~Sandbox() {
        std::error_code failure;
        std::filesystem::remove_all(root_, failure);
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    /// Кладёт файл, создавая каталоги по пути.
    void write(std::string_view relative, std::string_view text) const {
        const std::filesystem::path path = root_ / relative;

        std::filesystem::create_directories(path.parent_path());

        std::ofstream file{path, std::ios::binary};
        file << text;
    }

private:
    std::filesystem::path root_;
};

} // namespace

TEST_CASE("a resource is read from its description", "[server][resources]") {
    const Sandbox sandbox;
    sandbox.write("freeroam/resource.cfg", "type: js\n"
                                           "main: server/index.js\n"
                                           "client-main: client/index.js\n"
                                           "client-files: html/hud.html\n");
    sandbox.write("freeroam/server/index.js", "// сервер");
    sandbox.write("freeroam/client/index.js", "// клиент");
    sandbox.write("freeroam/html/hud.html", "<b>здравствуйте</b>");

    ResourceCatalog catalog;
    CHECK(catalog.load(sandbox.root(), {"freeroam"}).empty());

    REQUIRE(catalog.all().size() == 1);

    const ScriptResource& resource = catalog.all().front();
    CHECK(resource.name == "freeroam");
    CHECK(resource.type == "js");
    CHECK(resource.main == "server/index.js");

    // Клиентский вход попадает в раздачу сам собой: перечислять его ещё и в
    // списке файлов — лишняя работа для хозяина и лишний повод забыть.
    REQUIRE(resource.clientFiles.size() == 2);
    CHECK(std::ranges::find(resource.clientFiles, "client/index.js") !=
          resource.clientFiles.end());
}

TEST_CASE("a resource that names itself twice is taken once", "[server][resources]") {
    const Sandbox sandbox;
    sandbox.write("commands/resource.cfg", "type: native\nmain: commands\n");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints =
        catalog.load(sandbox.root(), {"commands", "commands"});

    // Дважды поднятый ресурс получал бы каждое событие по два раза, а остановка
    // убрала бы только одну из его половин.
    CHECK(catalog.all().size() == 1);
    CHECK(complaints.size() == 1);
}

TEST_CASE("a missing resource does not take the others down", "[server][resources]") {
    const Sandbox sandbox;
    sandbox.write("commands/resource.cfg", "type: native\nmain: commands\n");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints =
        catalog.load(sandbox.root(), {"нетутакого", "commands"});

    // Сервер, не поднявшийся из-за опечатки в одном имени, — худшее из
    // возможного: хозяин теряет сессию целиком там, где мог потерять её часть.
    REQUIRE(catalog.all().size() == 1);
    CHECK(catalog.all().front().name == "commands");
    CHECK(complaints.size() == 1);
}

TEST_CASE("a description without a runner is refused", "[server][resources]") {
    const Sandbox sandbox;
    sandbox.write("немой/resource.cfg", "main: commands\n");
    sandbox.write("пустой/resource.cfg", "type: native\n");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"немой", "пустой"});

    // Ни «чем запускать», ни «что запускать» без другого не значат ничего, и
    // поднимать такой ресурс наугад было бы хуже, чем отказаться.
    CHECK(catalog.all().empty());
    CHECK(complaints.size() == 2);
}

TEST_CASE("a name that is a path is refused", "[server][resources]") {
    const Sandbox sandbox;

    ResourceCatalog catalog;

    // Так выглядит старое значение ключа resources, оставшееся от прежнего
    // смысла, и так же выглядит перенесённый откуда-то пример с «..».
    const std::vector<std::string> complaints =
        catalog.load(sandbox.root(), {"resources/dlcpacks", ".."});

    CHECK(catalog.all().empty());
    CHECK(complaints.size() == 2);
}

TEST_CASE("a client file that leads outside stays home", "[server][resources]") {
    const Sandbox sandbox;
    sandbox.write("server.cfg", "тайна");
    sandbox.write("beглец/resource.cfg", "type: native\n"
                                         "main: commands\n"
                                         "client-files: ../server.cfg, свой.txt\n");
    sandbox.write("beглец/свой.txt", "можно");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"beглец"});

    REQUIRE(catalog.all().size() == 1);

    // Путь наружу отдал бы клиентам файл, который им не предназначался, — а
    // рядом с ресурсами лежит и server.cfg.
    REQUIRE(catalog.all().front().clientFiles.size() == 1);
    CHECK(catalog.all().front().clientFiles.front() == "свой.txt");
    CHECK(complaints.size() == 1);
}

TEST_CASE("a missing client file costs the file, not the resource", "[server][resources]") {
    const Sandbox sandbox;
    sandbox.write("freeroam/resource.cfg", "type: native\n"
                                           "main: commands\n"
                                           "client-files: есть.txt, нету.txt\n");
    sandbox.write("freeroam/есть.txt", "тут");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"freeroam"});

    // Раздать то, чего нет, всё равно нельзя, а отменять из-за одной картинки
    // весь игровой режим — наказание не по вине.
    REQUIRE(catalog.all().size() == 1);
    REQUIRE(catalog.all().front().clientFiles.size() == 1);
    CHECK(catalog.all().front().clientFiles.front() == "есть.txt");
    CHECK(complaints.size() == 1);
}
