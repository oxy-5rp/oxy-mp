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

TEST_CASE("a resource is read from a resource.toml too", "[server][resources]") {
    const Sandbox sandbox;

    // Так описание называется у alt:V, и под этим именем лежат описания всех
    // готовых игровых режимов.
    sandbox.write("main/resource.toml", "type = 'js'\n"
                                        "main = 'server/index.cjs'\n"
                                        "client-main = 'client/index.cjs'\n"
                                        "deps = []\n");
    sandbox.write("main/server/index.cjs", "// сервер");
    sandbox.write("main/client/index.cjs", "// клиент");

    ResourceCatalog catalog;
    CHECK(catalog.load(sandbox.root(), {"main"}).empty());

    REQUIRE(catalog.all().size() == 1);
    CHECK(catalog.all().front().type == "js");
    CHECK(catalog.all().front().main == "server/index.cjs");
}

TEST_CASE("our own description wins over the alt:V one", "[server][resources]") {
    const Sandbox sandbox;

    // В каталоге, где по недосмотру оказались оба, побеждает написанное для нас,
    // а не притащенное вместе с чужим режимом.
    sandbox.write("both/resource.cfg", "type: js\nmain: наш.js\n");
    sandbox.write("both/resource.toml", "type = 'js'\nmain = 'чужой.js'\n");
    sandbox.write("both/наш.js", "// сервер");

    ResourceCatalog catalog;
    (void)catalog.load(sandbox.root(), {"both"});

    REQUIRE(catalog.all().size() == 1);
    CHECK(catalog.all().front().main == "наш.js");
}

TEST_CASE("a wildcard in client files takes the whole directory", "[server][resources]") {
    const Sandbox sandbox;

    // `client-files = ['client/*']` у alt:V означает весь каталог со всем, что в
    // нём лежит: собранная страница интерфейса — это полторы тысячи файлов по
    // десятку вложенных каталогов, и выписывать их руками пришлось бы заново
    // после каждой пересборки.
    sandbox.write("main/resource.toml", "type = 'js'\n"
                                        "main = 'server/index.cjs'\n"
                                        "client-files = [ 'client/*' ]\n");
    sandbox.write("main/server/index.cjs", "// сервер");
    sandbox.write("main/client/index.cjs", "// клиент");
    sandbox.write("main/client/ui/index.html", "<b>страница</b>");
    sandbox.write("main/client/ui/assets/app.js", "// сборка");

    ResourceCatalog catalog;
    CHECK(catalog.load(sandbox.root(), {"main"}).empty());

    REQUIRE(catalog.all().size() == 1);

    const std::vector<std::string>& files = catalog.all().front().clientFiles;

    // Звёздочка пересекает косую черту: иначе вложенные каталоги страницы не
    // попали бы в раздачу.
    REQUIRE(files.size() == 3);
    CHECK(std::ranges::find(files, "client/ui/assets/app.js") != files.end());
}

TEST_CASE("a wildcard does not reach outside the resource", "[server][resources]") {
    const Sandbox sandbox;

    sandbox.write("main/resource.cfg", "type: js\n"
                                       "main: index.js\n"
                                       "client-files: *\n");
    sandbox.write("main/index.js", "// сервер");
    sandbox.write("secret.txt", "чужое");

    ResourceCatalog catalog;
    CHECK(catalog.load(sandbox.root(), {"main"}).empty());

    REQUIRE(catalog.all().size() == 1);

    // Обход идёт от корня ресурса, и выйти за него звёздочке не по чему.
    for (const std::string& file : catalog.all().front().clientFiles) {
        CHECK(file.find("secret") == std::string::npos);
    }
}

TEST_CASE("a wildcard matching nothing is reported", "[server][resources]") {
    const Sandbox sandbox;

    // Промолчать нельзя: хозяин, ошибшийся в шаблоне, иначе получил бы сессию,
    // в которой страница интерфейса просто не показывается, и без единой строки
    // о том, почему.
    sandbox.write("main/resource.cfg", "type: js\n"
                                       "main: index.js\n"
                                       "client-files: html/*.html\n");
    sandbox.write("main/index.js", "// сервер");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"main"});

    REQUIRE(complaints.size() == 1);
    CHECK(complaints.front().find("не подошёл ни один файл") != std::string::npos);
}

TEST_CASE("a wildcard picks files by extension", "[server][resources]") {
    const Sandbox sandbox;

    sandbox.write("main/resource.cfg", "type: js\n"
                                       "main: index.js\n"
                                       "client-files: html/*.html\n");
    sandbox.write("main/index.js", "// сервер");
    sandbox.write("main/html/hud.html", "<b>да</b>");
    sandbox.write("main/html/menu.html", "<b>да</b>");
    sandbox.write("main/html/style.css", "/* нет */");

    ResourceCatalog catalog;
    CHECK(catalog.load(sandbox.root(), {"main"}).empty());

    const std::vector<std::string>& files = catalog.all().front().clientFiles;

    REQUIRE(files.size() == 2);
    CHECK(std::ranges::find(files, "html/style.css") == files.end());
}

TEST_CASE("a file matched by two patterns is served once", "[server][resources]") {
    const Sandbox sandbox;

    sandbox.write("main/resource.cfg", "type: js\n"
                                       "main: index.js\n"
                                       "client-files: html/*, html/hud.html\n");
    sandbox.write("main/index.js", "// сервер");
    sandbox.write("main/html/hud.html", "<b>да</b>");

    ResourceCatalog catalog;
    CHECK(catalog.load(sandbox.root(), {"main"}).empty());

    REQUIRE(catalog.all().front().clientFiles.size() == 1);
}

TEST_CASE("the alt:V dependency list is read", "[server][resources]") {
    const Sandbox sandbox;

    sandbox.write("main/resource.toml", "type = 'js'\n"
                                        "main = 'index.js'\n"
                                        "deps = [ 'core', 'chat' ]\n");
    sandbox.write("main/index.js", "// сервер");

    ResourceCatalog catalog;
    CHECK(catalog.load(sandbox.root(), {"main"}).empty());

    REQUIRE(catalog.all().front().dependencies.size() == 2);
    CHECK(catalog.all().front().dependencies.front() == "core");
}

TEST_CASE("a resource that only hands out files needs no main") {
    Sandbox sandbox;

    // Так раздаются модели: каталог со `stream` внутри ничего не исполняет.
    sandbox.write("cars/resource.toml", "type = 'js'\nclient-files = [ 'stream/amcj.yft' ]\n");
    sandbox.write("cars/stream/amcj.yft", "RSC7");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"cars"});

    CHECK(complaints.empty());
    REQUIRE(catalog.all().size() == 1);

    const ScriptResource& resource = catalog.all().front();

    CHECK(resource.main.empty());
    CHECK(resource.clientFiles == std::vector<std::string>{"stream/amcj.yft"});
}

TEST_CASE("a dlc resource reads its file list from main") {
    Sandbox sandbox;

    // main у ресурса без кода указывает не на скрипт, а на список
    // раздаваемого (dlc.toml/stream.toml у alt:V) — читается он readManifest,
    // а не запускается.
    sandbox.write("cars/resource.toml", "type = 'dlc'\nmain = 'dlc.toml'\n");
    sandbox.write("cars/dlc.toml", "files = 'stream/amcj.yft'\n");
    sandbox.write("cars/stream/amcj.yft", "RSC7");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"cars"});

    CHECK(complaints.empty());
    REQUIRE(catalog.all().size() == 1);

    const ScriptResource& resource = catalog.all().front();

    // main прочитан и обнулён: дальше по коду он означал бы «что запускать»,
    // а исполнять здесь нечего.
    CHECK(resource.main.empty());
    CHECK(resource.clientFiles == std::vector<std::string>{"stream/amcj.yft"});
}

TEST_CASE("a dlc resource's main cannot read a file outside it", "[server][resources]") {
    // Тот же путь наружу, что уже запрещён для client-files, но с другой
    // стороны: main не раздаётся клиенту, а читается самим сервером — читать
    // чужое по указке из чужого resource.toml нельзя точно так же.
    const Sandbox sandbox;
    sandbox.write("server.cfg", "тайна");
    sandbox.write("beглец/resource.toml", "type = 'dlc'\nmain = '../server.cfg'\n");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"beглец"});

    // Ресурс с негодным main не поднимается вовсе: раздавать ему как dlc
    // больше нечего, а притворяться, что main просто пуст, значило бы молчать
    // о попытке выйти за пределы каталога.
    CHECK(catalog.all().empty());
    CHECK(complaints.size() == 1);
}

TEST_CASE("a resource with nothing to run and nothing to hand out is a mistake") {
    Sandbox sandbox;

    sandbox.write("empty/resource.toml", "type = 'js'\n");

    ResourceCatalog catalog;
    const std::vector<std::string> complaints = catalog.load(sandbox.root(), {"empty"});

    CHECK_FALSE(complaints.empty());
    CHECK(catalog.all().empty());
}
