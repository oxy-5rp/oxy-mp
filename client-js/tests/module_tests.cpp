// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.
//
// Машина проверяется так же, как её будет заводить клиент: библиотека грузится
// по имени, точка входа спрашивается по имени, граница сверяется по версии. Игра
// для этого не нужна вовсе — и это не удобство, а условие: проверка, требующая
// GTA, не запускается никогда.

#include <oxymp/client/js/abi.h>

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

/// Что машина сказала в журнал за время жизни проверки.
struct Recorder {
    std::vector<std::string> lines;
    std::vector<std::string> toServer;

    /// Последний вызванный натив и его доводы.
    std::uint64_t nativeHash = 0;
    std::vector<std::uint64_t> nativeArguments;

    /// Чем отвечать на вызов натива.
    std::uint64_t nativeAnswer = 0;
    bool nativeKnown = true;

    std::vector<std::string> createdViews;
    std::uint32_t nextView = 1;

    /// Каким номером клиент отвечает на вопрос машины.
    std::int32_t selfId = -1;

    /// Сущности сессии, какими их видит подставной клиент: номер и тело.
    ///
    /// Тем же порядком, что и настоящий клиент: сперва мы сами, потом
    /// остальные. Ноль телом означает «в сессии есть, а здесь его ещё нет» — на
    /// этом и проверяется разница между `all` и `streamedIn`.
    std::vector<std::pair<std::int32_t, std::int32_t>> players;
    std::vector<std::pair<std::int32_t, std::int32_t>> vehicles;

    /// Имена игроков сессии по их номеру.
    std::map<std::int32_t, std::string> names;

    /// Строка, отданная границе последней. Граница обещает, что она жива до
    /// следующего вызова, — значит, хранить её обязан клиент, а не вызов.
    std::string lastName;
};

Recorder& recorder() {
    static Recorder instance;
    return instance;
}

[[nodiscard]] std::string toText(OxympJsText text) {
    return text.data == nullptr ? std::string{}
                                : std::string{text.data, static_cast<std::size_t>(text.length)};
}

void onLog(void*, OxympJsLogLevel, OxympJsText resource, OxympJsText line) {
    recorder().lines.push_back(toText(resource) + ": " + toText(line));
}

void onEmitServer(void*, OxympJsText name, OxympJsBytes payload) {
    recorder().toServer.push_back(
        toText(name) + " " +
        std::string{reinterpret_cast<const char*>(payload.data),
                    static_cast<std::size_t>(payload.length)});
}

std::int32_t onCallNative(void*, std::uint64_t hash, const std::uint64_t* arguments,
                          std::uint32_t argumentCount, std::uint64_t* results, std::uint32_t) {
    Recorder& kept = recorder();

    kept.nativeHash = hash;
    kept.nativeArguments.assign(arguments, arguments + argumentCount);

    if (!kept.nativeKnown) {
        return 0;
    }

    results[0] = kept.nativeAnswer;
    return 1;
}

std::int32_t onLocalPlayerId(void*) {
    return recorder().selfId;
}

OxympJsBytes onReadResourceFile(void*, OxympJsText, OxympJsText) {
    return OxympJsBytes{nullptr, 0};
}

const std::vector<std::pair<std::int32_t, std::int32_t>>& listFor(OxympJsEntityKind kind) {
    return kind == kOxympJsEntityVehicle ? recorder().vehicles : recorder().players;
}

std::uint32_t onListEntities(void*, OxympJsEntityKind kind, OxympJsEntity* entities,
                             std::uint32_t capacity) {
    const auto& source = listFor(kind);

    for (std::size_t i = 0; i < source.size() && i < capacity; ++i) {
        entities[i] = OxympJsEntity{.id = source[i].first, .handle = source[i].second};
    }

    // Сколько есть, а не сколько влезло: обрезать молча нельзя.
    return static_cast<std::uint32_t>(source.size());
}

std::int32_t onEntityHandle(void*, OxympJsEntityKind kind, std::int32_t id) {
    for (const auto& [number, handle] : listFor(kind)) {
        if (number == id) {
            return handle;
        }
    }

    return 0;
}

std::int32_t onEntityId(void*, OxympJsEntityKind kind, std::int32_t handle) {
    if (handle == 0) {
        return -1;
    }

    for (const auto& [number, body] : listFor(kind)) {
        if (body == handle) {
            return number;
        }
    }

    return -1;
}

OxympJsText onPlayerName(void*, std::int32_t id) {
    Recorder& kept = recorder();

    const auto found = kept.names.find(id);
    kept.lastName = found == kept.names.end() ? std::string{} : found->second;

    return OxympJsText{kept.lastName.data(),
                       static_cast<std::uint32_t>(kept.lastName.size())};
}

std::uint32_t onCreateWebView(void*, OxympJsText, OxympJsText url) {
    recorder().createdViews.push_back(toText(url));
    return recorder().nextView++;
}

void onDestroyWebView(void*, std::uint32_t) {}
void onEmitWebView(void*, std::uint32_t, OxympJsText, OxympJsBytes) {}
void onSetVisible(void*, std::uint32_t, std::int32_t) {}
void onSetFocused(void*, std::uint32_t, std::int32_t) {}

/// Загруженная машина. Одна на весь прогон.
///
/// Одна, а не по одной на проверку, и это не лень: Node поднимается один раз на
/// процесс — это его правило, не наше. Выгрузить и поднять его заново в том же
/// процессе нельзя.
class Engine {
public:
    static Engine& instance() {
        static Engine only;
        return only;
    }

    [[nodiscard]] const OxympJsEngine* operator->() const { return engine_; }
    [[nodiscard]] bool ready() const { return engine_ != nullptr; }

private:
    Engine() {
        const std::filesystem::path path = std::filesystem::path{OXYMP_CLIENT_JS_LIBRARY};

        library_ = ::LoadLibraryW(path.wstring().c_str());
        if (library_ == nullptr) {
            return;
        }

        const auto entry = reinterpret_cast<OxympJsEntry>(
            reinterpret_cast<void*>(::GetProcAddress(library_, OXYMP_CLIENT_JS_ENTRY_NAME)));

        if (entry == nullptr) {
            return;
        }

        host_.context = nullptr;
        host_.log = &onLog;
        host_.emitServer = &onEmitServer;
        host_.callNative = &onCallNative;
        host_.localPlayerId = &onLocalPlayerId;
        host_.readResourceFile = &onReadResourceFile;
        host_.listEntities = &onListEntities;
        host_.entityHandle = &onEntityHandle;
        host_.entityId = &onEntityId;
        host_.playerName = &onPlayerName;
        host_.createWebView = &onCreateWebView;
        host_.destroyWebView = &onDestroyWebView;
        host_.emitWebView = &onEmitWebView;
        host_.setWebViewVisible = &onSetVisible;
        host_.setWebViewFocused = &onSetFocused;

        engine_ = entry(OXYMP_CLIENT_JS_ABI_VERSION, &host_);

        if (engine_ != nullptr) {
            (void)engine_->setUp();
        }
    }

    HMODULE library_ = nullptr;
    OxympJsHost host_{};
    const OxympJsEngine* engine_ = nullptr;
};

[[nodiscard]] OxympJsText text(std::string_view value) {
    return OxympJsText{value.data(), static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] OxympJsBytes bytes(std::string_view value) {
    return OxympJsBytes{reinterpret_cast<const std::uint8_t*>(value.data()),
                        static_cast<std::uint32_t>(value.size())};
}

/// Каталог с ресурсом на диске, убирающий за собой.
class Sandbox {
public:
    explicit Sandbox(std::string_view script, std::string_view name = "index.js") {
        root_ = std::filesystem::temp_directory_path() /
                ("oxymp-client-js-" + std::to_string(::GetTickCount64()) + "-" +
                 std::to_string(counter()++));

        std::filesystem::create_directories(root_);

        std::ofstream file{root_ / name, std::ios::binary};
        file << script;
    }

    ~Sandbox() {
        std::error_code failure;
        std::filesystem::remove_all(root_, failure);
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] std::string root() const { return root_.string(); }

private:
    static int& counter() {
        static int value = 0;
        return value;
    }

    std::filesystem::path root_;
};

/// Поднимает ресурс с этим текстом и прокручивает движок.
///
/// Прокрутка обязательна: без неё не сработает ни один таймер и не разрешится ни
/// одно обещание — ровно как в игре.
[[nodiscard]] bool run(std::string_view name, std::string_view script,
                       std::string_view entry = "index.js") {
    static std::vector<std::unique_ptr<Sandbox>> kept;

    kept.push_back(std::make_unique<Sandbox>(script, entry));

    const std::string root = kept.back()->root();

    if (Engine::instance()->startResource(text(name), text(root), text(entry)) == 0) {
        return false;
    }

    Engine::instance()->tick();
    return true;
}

/// Есть ли среди сказанного строка с этим куском.
[[nodiscard]] bool said(std::string_view piece) {
    for (const std::string& line : recorder().lines) {
        if (line.find(piece) != std::string::npos) {
            return true;
        }
    }

    return false;
}

} // namespace

TEST_CASE("the engine refuses a boundary of another version", "[client][js]") {
    // Клиент и машина раздаются вместе, но на диске может оказаться DLL от
    // прошлой сборки. Отказ здесь честнее падения при первом же вызове по
    // указателю, который указывает не туда.
    const std::filesystem::path path{OXYMP_CLIENT_JS_LIBRARY};

    const HMODULE library = ::LoadLibraryW(path.wstring().c_str());
    REQUIRE(library != nullptr);

    const auto entry = reinterpret_cast<OxympJsEntry>(
        reinterpret_cast<void*>(::GetProcAddress(library, OXYMP_CLIENT_JS_ENTRY_NAME)));
    REQUIRE(entry != nullptr);

    OxympJsHost host{};
    CHECK(entry(OXYMP_CLIENT_JS_ABI_VERSION + 1000u, &host) == nullptr);
}

TEST_CASE("the engine loads and answers", "[client][js]") {
    REQUIRE(Engine::instance().ready());
}

TEST_CASE("a client resource runs and reaches the log", "[client][js]") {
    REQUIRE(run("greeting", "const alt = require('alt-client');\n"
                            "alt.log('здравствуйте с клиента');\n"));

    CHECK(said("здравствуйте с клиента"));
}

TEST_CASE("the shared layer is the same one the server has", "[client][js]") {
    // Тот же файл, а не похожий: разойдись hash хоть в разряде, ресурс,
    // считающий его на клиенте и на сервере, получал бы два разных ответа.
    REQUIRE(run("shared", "const alt = require('alt-client');\n"
                          "alt.log('hash=' + alt.hash('adder').toString(16));\n"
                          "alt.log('len=' + new alt.Vector3(3, 4, 0).length);\n"
                          "alt.log('key=' + alt.KeyCode.E);\n"));

    CHECK(said("hash=b779a091"));
    CHECK(said("len=5"));
    CHECK(said("key=69"));
}

TEST_CASE("alt-shared is reachable as its own module", "[client][js]") {
    REQUIRE(run("sharedmodule", "const { Vector3 } = require('alt-shared');\n"
                                "require('alt-client').log('v=' + new Vector3(1, 2, 2).length);\n"));

    CHECK(said("v=3"));
}

TEST_CASE("an event from the server reaches the resource", "[client][js]") {
    REQUIRE(run("fromserver", "const alt = require('alt-client');\n"
                              "alt.onServer('привет', (кто, сколько) =>\n"
                              "    alt.log(`сервер сказал ${кто} ${сколько}`));\n"));

    Engine::instance()->dispatchServerEvent(text("привет"), bytes("[\"миру\",7]"));

    CHECK(said("сервер сказал миру 7"));
}

TEST_CASE("an event to the server leaves the resource", "[client][js]") {
    recorder().toServer.clear();

    REQUIRE(run("toserver", "const alt = require('alt-client');\n"
                            "alt.emitServer('готов', 1, 'да');\n"));

    REQUIRE(recorder().toServer.size() == 1);
    CHECK(recorder().toServer.front() == "готов [1,\"да\"]");
}

TEST_CASE("a key press reaches the resource once, not every frame", "[client][js]") {
    // Повтор от удержания событием не считается: иначе меню по клавише
    // открывалось бы и закрывалось тридцать раз в секунду.
    REQUIRE(run("keys", "const alt = require('alt-client');\n"
                        "let счёт = 0;\n"
                        "alt.on('keydown', (код) => alt.log(`нажато ${код}, раз ${++счёт}`));\n"
                        "alt.on('keyup', () => alt.log(`держим: ${alt.isKeyDown(69)}`));\n"));

    Engine::instance()->dispatchKey(69, 1);
    Engine::instance()->dispatchKey(69, 1);
    Engine::instance()->dispatchKey(69, 0);

    CHECK(said("нажато 69, раз 1"));
    CHECK_FALSE(said("раз 2"));
    CHECK(said("держим: false"));
}

TEST_CASE("a web view is opened through the client", "[client][js]") {
    recorder().createdViews.clear();

    REQUIRE(run("view", "const alt = require('alt-client');\n"
                        "const окно = new alt.WebView('http://resource/ui/index.html');\n"
                        "alt.log('окно ' + окно.id + ', годно: ' + окно.valid);\n"));

    REQUIRE(recorder().createdViews.size() == 1);
    CHECK(recorder().createdViews.front() == "http://resource/ui/index.html");
    CHECK(said("годно: true"));
}

TEST_CASE("an event from a web view reaches its own handler", "[client][js]") {
    REQUIRE(run("viewevent", "const alt = require('alt-client');\n"
                             "const окно = new alt.WebView('http://resource/a.html');\n"
                             "окно.on('нажали', (что) => alt.log(`страница: ${что}`));\n"
                             "globalThis.номерОкна = окно.id;\n"));

    // Номер берётся тот же, что клиент выдал последним: страница отвечает
    // именно своему окну.
    Engine::instance()->dispatchWebViewEvent(recorder().nextView - 1, text("нажали"),
                                             bytes("[\"кнопку\"]"));

    CHECK(said("страница: кнопку"));
}

TEST_CASE("a native is called by name from the generated table", "[client][js]") {
    // Так его и зовёт всякий клиентский ресурс alt:V.
    recorder().nativeKnown = true;
    recorder().nativeAnswer = 4242;

    REQUIRE(run("native", "const natives = require('natives');\n"
                          "require('alt-client').log('время ' + natives.getGameTimer());\n"));

    // GET_GAME_TIMER: хеш выверен по живой игре (client/src/game/native_hashes.hpp).
    CHECK(recorder().nativeHash == 0x1DD05E817C89C737ULL);
    CHECK(said("время 4242"));
}

TEST_CASE("native arguments are laid out by the signature", "[client][js]") {
    // Дробное кладётся своими битами, а не значением: игра читает ячейку как
    // float. Положи мы туда единицу целым — натив прочёл бы 1.4e-45.
    recorder().nativeKnown = true;

    REQUIRE(run("nativeargs",
                "const natives = require('natives');\n"
                "natives.setEntityCoords(7, 1.5, 0, 0, false, false, false, true);\n"));

    REQUIRE(recorder().nativeArguments.size() == 8);
    CHECK(recorder().nativeArguments[0] == 7ULL);

    // Полтора в битах float — 0x3FC00000.
    CHECK(recorder().nativeArguments[1] == 0x3FC00000ULL);
    CHECK(recorder().nativeArguments[4] == 0ULL);
    CHECK(recorder().nativeArguments[7] == 1ULL);
}

TEST_CASE("a native missing from this build is simply absent", "[client][js]") {
    // Нативам, которых нет в нашей сборке игры, хеш не выдумывается: неверный
    // даёт не строку в журнале, а вылет игры в мгновение вызова.
    REQUIRE(run("nonative", "const natives = require('natives');\n"
                            "require('alt-client').log('нет такого: ' +\n"
                            "    (natives.такогоНативаНет === undefined));\n"));

    CHECK(said("нет такого: true"));
}

TEST_CASE("an unresolved native answers nothing instead of zero", "[client][js]") {
    // Ноль отличается от «натив вернул ноль» именно тем, что при отказе
    // результат не трогается: выдать одно за другое значило бы прятать
    // неразрешённый хеш.
    recorder().nativeKnown = false;

    REQUIRE(run("unresolved", "const natives = require('natives');\n"
                              "const ответ = natives.getGameTimer();\n"
                              "require('alt-client').log('ответ ' +\n"
                              "    (ответ === null ? 'пусто' : ответ));\n"));

    CHECK(said("ответ пусто"));

    recorder().nativeKnown = true;
}


TEST_CASE("the client knows every player of the session, not only itself", "[client][js]") {
    // То самое, чего недоставало: клиент видел вокруг себя одного — себя.
    Recorder& kept = recorder();
    kept.selfId = 7;
    kept.players = {{7, 111}, {9, 222}};
    kept.names = {{7, "oxy"}, {9, "сосед"}};

    REQUIRE(run("roster", "const alt = require('alt-client');\n"
                          "alt.log('всего ' + alt.Player.all.length);\n"
                          "const кто = alt.Player.getByID(9);\n"
                          "alt.log('рядом ' + кто.id + ' ' + кто.name);\n"
                          "alt.log('я ' + alt.Player.local.id);\n"));

    CHECK(said("всего 2"));
    CHECK(said("рядом 9 сосед"));
    CHECK(said("я 7"));
}

TEST_CASE("a player of the session without a body is still in the list", "[client][js]") {
    // Игрок в сессии есть, а тела у него здесь ещё нет: он далеко или его модель
    // грузится. У alt:V такой игрок тоже есть — с нулевым scriptID, — и списки
    // разведены ровно поэтому.
    Recorder& kept = recorder();
    kept.selfId = 1;
    kept.players = {{1, 100}, {2, 0}};
    kept.names = {{1, "я"}, {2, "далёкий"}};

    REQUIRE(run("streamed", "const alt = require('alt-client');\n"
                            "alt.log('всех ' + alt.Player.all.length +\n"
                            "    ', рядом ' + alt.Player.streamedIn.length);\n"
                            "alt.log('зовётся ' + alt.Player.getByID(2).name);\n"));

    CHECK(said("всех 2, рядом 1"));
    CHECK(said("зовётся далёкий"));
}

TEST_CASE("the same player of the session is the same object twice", "[client][js]") {
    // Тождество здесь не удобство: режимы сравнивают сущности через === и кладут
    // их ключами в Map. Две обёртки на одного человека означали бы «меня в
    // списке нет» — и ни одной жалобы.
    Recorder& kept = recorder();
    kept.selfId = 3;
    kept.players = {{3, 300}, {4, 400}};
    kept.names = {{3, "я"}, {4, "он"}};

    REQUIRE(run("identity", "const alt = require('alt-client');\n"
                            "const первый = alt.Player.getByID(4);\n"
                            "const второй = alt.Player.all.find((кто) => кто.id === 4);\n"
                            "alt.log('он тот же: ' + (первый === второй));\n"
                            "alt.log('я тот же: ' +\n"
                            "    (alt.Player.all.find((кто) => кто.id === 3) ===\n"
                            "     alt.Player.local));\n"));

    CHECK(said("он тот же: true"));
    CHECK(said("я тот же: true"));
}

TEST_CASE("a synced meta of another player is readable", "[client][js]") {
    // Раньше отказывало вслух: номера чужой сущности клиент не знал, а сервер
    // рассылает метаданные именно по номерам.
    Recorder& kept = recorder();
    kept.selfId = 5;
    kept.players = {{5, 500}, {6, 600}};
    kept.names = {{5, "я"}, {6, "он"}};

    REQUIRE(run("othermeta", "const alt = require('alt-client');\n"
                             "alt.onServer('готово', () => {\n"
                             "    const он = alt.Player.getByID(6);\n"
                             "    alt.log('организация: ' + он.getSyncedMeta('org'));\n"
                             "});\n"));

    // Служебное имя то же, что и на сервере (alt_server.js, kSyncedMetaEvent).
    Engine::instance()->dispatchServerEvent(text("__oxymp:meta"),
                                            bytes(R"(["player",6,"org","мафия"])"));
    Engine::instance()->dispatchServerEvent(text("готово"), bytes("[]"));

    CHECK(said("организация: мафия"));
}

TEST_CASE("a game handle resolves back to the session entity", "[client][js]") {
    // Обратный перевод: луч, попавший в машину, отдаёт дескриптор, и без него
    // ресурс не узнал бы, во что попал.
    Recorder& kept = recorder();
    kept.selfId = 8;
    kept.players = {{8, 800}};
    kept.names = {{8, "я"}};
    kept.vehicles = {{31, 3100}};

    // Подставной клиент отвечает на всякий натив одним и тем же — в том числе
    // «сущность есть» и «это машина». Больше здесь от нативов ничего и не нужно:
    // проверяется перевод, а не игра.
    kept.nativeKnown = true;
    kept.nativeAnswer = 1;

    REQUIRE(run("reverse", "const alt = require('alt-client');\n"
                           "const машина = alt.fromScriptID(3100);\n"
                           "alt.log('машина ' + машина.id +\n"
                           "    ', она же: ' + (машина === alt.Vehicle.getByID(31)));\n"
                           "alt.log('машин ' + alt.Vehicle.all.length);\n"));

    CHECK(said("машина 31, она же: true"));
    CHECK(said("машин 1"));
}

TEST_CASE("an entity of the game alone has no session number", "[client][js]") {
    // Случайный прохожий принадлежит игре, а не серверу. Ноль здесь был бы
    // ложью: ноль — законный номер, и отправленный на сервер он указал бы на
    // живого человека.
    Recorder& kept = recorder();
    kept.selfId = 2;
    kept.players = {{2, 200}};
    kept.vehicles = {};
    kept.nativeKnown = true;
    kept.nativeAnswer = 1;

    REQUIRE(run("stranger", "const alt = require('alt-client');\n"
                            "const прохожий = alt.fromScriptID(999);\n"
                            "try {\n"
                            "    прохожий.id;\n"
                            "    alt.log('номер выдан — так быть не должно');\n"
                            "} catch (отказ) {\n"
                            "    alt.log('отказ: ' + отказ.message);\n"
                            "}\n"));

    CHECK(said("отказ: entity.id"));
}
TEST_CASE("a broken resource does not take the process down", "[client][js]") {
    // Процесс здесь — сама игра, и уронить её ошибкой в чужом ресурсе нельзя.
    CHECK_FALSE(run("broken", "это не JavaScript ((((\n"));

    CHECK(Engine::instance().ready());
}

TEST_CASE("a throwing handler does not stop the others", "[client][js]") {
    REQUIRE(run("throwing", "const alt = require('alt-client');\n"
                            "alt.onServer('раз', () => { throw new Error('нарочно'); });\n"
                            "alt.onServer('раз', () => alt.log('второй обработчик дошёл'));\n"));

    Engine::instance()->dispatchServerEvent(text("раз"), bytes("[]"));

    CHECK(said("второй обработчик дошёл"));
}

TEST_CASE("what is not done yet refuses out loud", "[client][js]") {
    // Молчаливая заглушка хуже отсутствия: её ищут часами.
    REQUIRE(run("absent", "const alt = require('alt-client');\n"
                          "try { alt.Voice(); } catch (e) { alt.log('отказ: ' + e.message); }\n"));

    CHECK(said("отказ: alt.Voice: в oxyMP этого ещё нет"));
}

TEST_CASE("the entity layer is built on natives", "[client][js]") {
    // Свойство, которое можно спросить у игры, спрашивается у игры — и никогда
    // не кешируется: она меняет их каждый кадр.
    recorder().nativeKnown = true;
    recorder().nativeAnswer = 4321;

    REQUIRE(run("entities", "const alt = require('alt-client');\n"
                            "const я = alt.Player.local;\n"
                            "alt.log('дескриптор ' + я.scriptID);\n"));

    // PLAYER_PED_ID: хеш выверен по живой игре.
    CHECK(recorder().nativeHash == 0x4A8C381C258A124DULL);
    CHECK(said("дескриптор 4321"));
}

TEST_CASE("a blip is made through the game, not invented", "[client][js]") {
    recorder().nativeKnown = true;
    recorder().nativeAnswer = 77;

    REQUIRE(run("blip", "const alt = require('alt-client');\n"
                        "const метка = new alt.PointBlip(1, 2, 3);\n"
                        "alt.log('метка ' + метка.scriptID);\n"));

    CHECK(said("метка 77"));
}

TEST_CASE("a blip that the game refuses is an error, not a ghost", "[client][js]") {
    // Ноль от игры означает «метки нет». Обернув его молча, мы отдали бы
    // ресурсу объект, у которого не работает ничего и который не жалуется.
    recorder().nativeKnown = true;
    recorder().nativeAnswer = 0;

    REQUIRE(run("blipfail", "const alt = require('alt-client');\n"
                            "try { new alt.PointBlip(1, 2, 3); }\n"
                            "catch (e) { alt.log('отказ метки: ' + e.message); }\n"));

    CHECK(said("отказ метки: метка не завелась: игра отказала"));
}


TEST_CASE("a timer fires when the engine is pumped", "[client][js]") {
    REQUIRE(run("timer", "const alt = require('alt-client');\n"
                         "const номер = alt.setTimeout(() => alt.log('таймер сработал'), 1);\n"
                         "alt.log('номер число: ' + (typeof номер === 'number'));\n"));

    CHECK(said("номер число: true"));

    // Прокрутка несколько раз подряд: цикл событий отдаёт таймер не раньше, чем
    // истечёт его срок, а срок здесь измеряется настоящими миллисекундами.
    for (int attempt = 0; attempt < 200 && !said("таймер сработал"); ++attempt) {
        ::Sleep(1);
        Engine::instance()->tick();
    }

    CHECK(said("таймер сработал"));
}

// Доводы события сверены с `@altv/types-client`, а не вспомянуты: у общих
// метаданных это `globalSyncedMetaChange(key, value, oldValue)` — без сущности
// вовсе. Прежде отсюда уходило `syncedMetaChange` с родом и номером впереди, и
// обработчик, написанный под alt:V, получал строку «global» там, где ждал игрока.
TEST_CASE("global synced meta arrives without an entity", "[client][js]") {
    // Метаданные приходят служебным событием, а не своим сообщением: событие
    // уже умеет ходить, а сообщение стоило бы номера в протоколе.
    REQUIRE(run("meta", "const alt = require('alt-client');\n"
                        "alt.on('globalSyncedMetaChange', (ключ, что, было) =>\n"
                        "    alt.log(`меняли ${ключ} = ${что}, было ${было}`));\n"
                        "alt.on('syncedMetaChange', () =>\n"
                        "    alt.logError('общие метаданные ушли не тем событием'));\n"));

    Engine::instance()->dispatchServerEvent(
        text("__oxymp:meta"), bytes("[\"global\",0,\"погода\",\"ясно\",false]"));
    Engine::instance()->dispatchServerEvent(
        text("__oxymp:meta"), bytes("[\"global\",0,\"погода\",\"дождь\",false]"));

    // Прежнее значение приходит четвёртым доводом, и снимать его нужно до
    // записи: после присваивания взять его будет уже неоткуда.
    CHECK(said("меняли погода = ясно, было undefined"));
    CHECK(said("меняли погода = дождь, было ясно"));
}

// `syncedMetaChange(entity, key, value, oldValue)` — сущность первым доводом, а
// не род с номером. Режим ждёт именно её: он читает у неё имя, положение и
// прочие метаданные, а по числу не прочтёт ничего.
TEST_CASE("synced meta of a player arrives as the player himself", "[client][js]") {
    Recorder& kept = recorder();
    kept.selfId = 7;
    kept.players = {{7, 111}, {9, 222}};
    kept.names = {{7, "oxy"}, {9, "сосед"}};

    REQUIRE(run("metaentity",
                "const alt = require('alt-client');\n"
                "alt.on('syncedMetaChange', (кто, ключ, что) =>\n"
                "    alt.log(`${кто instanceof alt.Player} ${кто.id} ${кто.name} ${ключ}=${что}`));\n"));

    Engine::instance()->dispatchServerEvent(
        text("__oxymp:meta"), bytes("[\"player\",9,\"звание\",\"старший\",false]"));

    CHECK(said("true 9 сосед звание=старший"));
}

// Потоковые метаданные у alt:V ходят своим событием. Хранятся они у нас в одном
// месте с обычными и доходят одинаково, и различает их признак в посылке: не
// различай мы их, режим, подписанный на оба события, считал бы каждое изменение
// дважды.
TEST_CASE("stream synced meta has an event of its own", "[client][js]") {
    Recorder& kept = recorder();
    kept.selfId = 1;
    kept.players = {{1, 100}};
    kept.names = {{1, "я"}};

    REQUIRE(run("metastream",
                "const alt = require('alt-client');\n"
                "alt.on('streamSyncedMetaChange', (кто, ключ, что) =>\n"
                "    alt.log(`потоковая ${кто.id}.${ключ}=${что}`));\n"
                "alt.on('syncedMetaChange', () =>\n"
                "    alt.logError('потоковая ушла обычным событием'));\n"));

    Engine::instance()->dispatchServerEvent(
        text("__oxymp:meta"), bytes("[\"player\",1,\"метка\",\"своя\",true]"));

    CHECK(said("потоковая 1.метка=своя"));
}

// Сущности у клиента может не быть вовсе: предмет и кукла по номеру сессии ему
// не известны, а игрок мог ещё не быть объявлен. Событие тогда не объявляется —
// объявить его не с чем, а сущностью первым доводом alt:V обещает именно
// сущность, а не число.
TEST_CASE("synced meta of an entity the client does not know announces nothing",
          "[client][js]") {
    REQUIRE(run("metaunknown",
                "const alt = require('alt-client');\n"
                "alt.on('syncedMetaChange', () =>\n"
                "    alt.logError('событие объявлено без сущности'));\n"
                "alt.onServer('прочти', () => alt.log('машина жива'));\n"));

    Engine::instance()->dispatchServerEvent(
        text("__oxymp:meta"), bytes("[\"ped\",3,\"роль\",\"охранник\",false]"));
    Engine::instance()->dispatchServerEvent(text("прочти"), bytes("[]"));

    // Незнакомый род не уронил машину: следующее событие дошло как обычно.
    CHECK(said("машина жива"));
}

TEST_CASE("global synced meta is readable after it arrives", "[client][js]") {
    REQUIRE(run("metaread", "const alt = require('alt-client');\n"
                            "alt.onServer('прочти', () =>\n"
                            "    alt.log('час: ' + alt.getSyncedMeta('час')));\n"));

    Engine::instance()->dispatchServerEvent(text("__oxymp:meta"),
                                            bytes("[\"global\",0,\"час\",12]"));
    Engine::instance()->dispatchServerEvent(text("прочти"), bytes("[]"));

    CHECK(said("час: 12"));
}

TEST_CASE("a deleted synced key stops being readable", "[client][js]") {
    // Пустота в значении означает «ключ убрали», а не «значение пустое».
    REQUIRE(run("metadel", "const alt = require('alt-client');\n"
                           "alt.onServer('прочти', () =>\n"
                           "    alt.log('осталось: ' + alt.getSyncedMeta('вр')));\n"));

    Engine::instance()->dispatchServerEvent(text("__oxymp:meta"),
                                            bytes("[\"global\",0,\"вр\",\"есть\"]"));
    Engine::instance()->dispatchServerEvent(text("__oxymp:meta"),
                                            bytes("[\"global\",0,\"вр\",null]"));
    Engine::instance()->dispatchServerEvent(text("прочти"), bytes("[]"));

    CHECK(said("осталось: undefined"));
}

TEST_CASE("the local player knows its session number at once", "[client][js]") {
    // Спрашивается у клиента, а не присылается событием: ресурс читает номер
    // первыми же строками, ещё до того, как до него дойдёт хоть одно событие.
    recorder().selfId = 7;

    REQUIRE(run("selfid", "const alt = require('alt-client');\n"
                          "alt.log('я — номер ' + alt.Player.local.id);\n"));

    CHECK(said("я — номер 7"));
}

TEST_CASE("the local player reads its own synced meta", "[client][js]") {
    recorder().selfId = 7;

    REQUIRE(run("selfmeta",
                "const alt = require('alt-client');\n"
                "alt.onServer('спроси', () =>\n"
                "    alt.log('моё имя: ' + alt.Player.local.getSyncedMeta('имя')));\n"));

    Engine::instance()->dispatchServerEvent(text("__oxymp:meta"),
                                            bytes("[\"player\",7,\"имя\",\"Иван\"]"));
    Engine::instance()->dispatchServerEvent(text("спроси"), bytes("[]"));

    CHECK(said("моё имя: Иван"));
}

TEST_CASE("meta of an entity without a number refuses out loud", "[client][js]") {
    // undefined выглядело бы как «ключа нет», и режим искал бы ошибку на
    // сервере, где её нет.
    //
    // Отказ этот остался и после того, как переводчик сущностей появился, — но
    // значит теперь другое. Раньше он значил «клиент не знает номеров вовсе»;
    // теперь — «у этой сущности номера нет и быть не может»: прохожий с улицы
    // принадлежит игре, а не серверу.
    REQUIRE(run("metaentity", "const alt = require('alt-client');\n"
                              "const чужой = new alt.Ped(123);\n"
                              "try { чужой.getSyncedMeta('x'); }\n"
                              "catch (e) { alt.log('отказ meta: ' + e.message); }\n"));

    CHECK(said("отказ meta: entity.getSyncedMeta: у этой сущности нет номера"));
}

TEST_CASE("a resource written as an ES module runs", "[client][js]") {
    // Клиентская половина у alt:V почти всегда модуль ES: собранный бандл
    // начинается с `import * as alt from 'alt-client'`. Загрузчик CommonJS
    // спотыкается о первую же такую строку — «Cannot use import statement
    // outside a module», — и на этом чужой режим кончался.
    REQUIRE(run("esm", "import * as alt from 'alt-client';\n"
                       "import { Vector3 } from 'alt-shared';\n"
                       "alt.log('модуль ES поднялся, длина ' + new Vector3(0, 3, 4).length);\n",
                "index.mjs"));

    CHECK(said("модуль ES поднялся, длина 5"));
}

TEST_CASE("named imports of natives work in an ES module", "[client][js]") {
    // `import { getGameTimer } from 'natives'` — так их и тянут собранные
    // бандлы. Именованный экспорт обязан быть тем же самым, что и через
    // посредник: иначе получились бы два разных натива с одним именем.
    recorder().nativeKnown = true;
    recorder().nativeAnswer = 999;

    REQUIRE(run("esmnatives", "import { getGameTimer } from 'natives';\n"
                              "import * as alt from 'alt-client';\n"
                              "alt.log('время ' + getGameTimer());\n",
                "index.mjs"));

    CHECK(recorder().nativeHash == 0x1DD05E817C89C737ULL);
    CHECK(said("время 999"));
}

TEST_CASE("an ES module that throws is reported as a failed resource", "[client][js]") {
    // Ошибка в первой строке чужого ресурса обязана попасть в журнал отказом
    // ресурса, а не необработанным отказом обещания спустя кадр, когда связать
    // её уже не с чем.
    CHECK_FALSE(run("esmbroken", "import * as alt from 'alt-client';\n"
                                 "throw new Error('нарочно');\n",
                    "index.mjs"));

    CHECK(said("нарочно"));
}

TEST_CASE("a module disguised as commonjs still runs", "[client][js]") {
    // Собранный бандл alt:V зовётся `index.cjs`, а внутри у него `import`.
    // Node глядит на расширение, объявляет файл CommonJS и жалуется «Cannot use
    // import statement outside a module» — на этом чужой режим и кончался.
    REQUIRE(run("disguised", "import * as alt from 'alt-client';\n"
                             "alt.log('переодетый модуль поднялся');\n",
                "index.cjs"));

    CHECK(said("переодетый модуль поднялся"));
}

TEST_CASE("a real commonjs file is left alone", "[client][js]") {
    // Обратная сторона: проверка на модуль не должна принимать за него обычный
    // файл CommonJS — тот сломался бы, объявленный модулем, на первом же
    // `module.exports`.
    REQUIRE(run("realcjs", "const alt = require('alt-client');\n"
                           "module.exports = {};\n"
                           "alt.log('обычный CommonJS поднялся');\n",
                "index.cjs"));

    CHECK(said("обычный CommonJS поднялся"));
}

// --- Сеть ресурса ------------------------------------------------------------
//
// Сокета здесь не поднимается ни одного, и это не лень: проверка, которой нужен
// живой сервер, не запускается никогда — ни в чужой сборке, ни без сети.
// Проверяется то, что решается без неё: имена, заголовки, состояния и отказы.

TEST_CASE("the resource has an http client with the methods alt:V promises",
          "[client][js]") {
    REQUIRE(run("http", "const alt = require('alt-client');\n"
                        "const клиент = new alt.HttpClient();\n"
                        "const имена = ['get', 'head', 'post', 'put', 'delete',\n"
                        "               'connect', 'options', 'trace', 'patch'];\n"
                        "alt.log('все на месте: ' +\n"
                        "    имена.every((имя) => typeof клиент[имя] === 'function'));\n"));

    CHECK(said("все на месте: true"));
}

TEST_CASE("extra headers stay with the http client, not with the request",
          "[client][js]") {
    // Так это устроено у alt:V: ресурс заводит один клиент, кладёт в него ключ
    // доступа и потом ходит им по всем своим адресам.
    REQUIRE(run("httphead", "const alt = require('alt-client');\n"
                            "const клиент = new alt.HttpClient();\n"
                            "клиент.setExtraHeader('X-Ключ', 'секрет');\n"
                            "alt.log('заголовок: ' + клиент.getExtraHeaders()['X-Ключ']);\n"
                            "const снятые = клиент.getExtraHeaders();\n"
                            "снятые['X-Ключ'] = 'подмена';\n"
                            "alt.log('после подмены: ' +\n"
                            "    клиент.getExtraHeaders()['X-Ключ']);\n"));

    CHECK(said("заголовок: секрет"));

    // Отданный наружу набор — копия: иначе заголовки можно было бы менять в
    // обход setExtraHeader, и менять молча.
    CHECK(said("после подмены: секрет"));
}

// Отказ обещания, а не пустой ответ: ресурс, принявший тишину за пустое тело,
// унёс бы эту ложь дальше.
TEST_CASE("an http request to nowhere refuses out loud", "[client][js]") {
    REQUIRE(run("httpbad", "const alt = require('alt-client');\n"
                           "const клиент = new alt.HttpClient();\n"
                           "клиент.get('не адрес вовсе')\n"
                           "    .then(() => alt.logError('запрос удался, а не должен был'))\n"
                           "    .catch((беда) => alt.log('отказ: ' + беда.message));\n"));

    // Отказ обещания приходит следующим оборотом цикла событий, а крутит его
    // клиент кадром игры — здесь же вместо кадра прокрутка вручную.
    for (int attempt = 0; attempt < 200 && !said("отказ:"); ++attempt) {
        ::Sleep(1);
        Engine::instance()->tick();
    }

    CHECK(said("отказ: alt.HttpClient: адрес «не адрес вовсе» не разобран"));
}

TEST_CASE("a websocket starts closed and keeps its sub protocols", "[client][js]") {
    REQUIRE(run("ws", "const alt = require('alt-client');\n"
                      "const связь = new alt.WebSocketClient('ws://127.0.0.1:1/');\n"
                      "alt.log('адрес: ' + связь.url);\n"
                      "alt.log('состояние: ' + связь.readyState + ' из ' +\n"
                      "    alt.WebSocketReadyState.Closed);\n"
                      "связь.addSubProtocol('чат');\n"
                      "связь.addSubProtocol('эхо');\n"
                      "alt.log('протоколы: ' + связь.getSubProtocols().join(','));\n"
                      "alt.log('послать до связи: ' + связь.send('привет'));\n"));

    CHECK(said("адрес: ws://127.0.0.1:1/"));

    // До start связи нет вовсе, и состояние у неё то же, что после закрытия.
    CHECK(said("состояние: 3 из 3"));
    CHECK(said("протоколы: чат,эхо"));

    // Послать в неподнятую связь нельзя, и об этом говорится ложью в ответе, а
    // не броском: у alt:V send отдаёт признак удачи.
    CHECK(said("послать до связи: false"));
}

TEST_CASE("websocket listeners can be added and taken back", "[client][js]") {
    REQUIRE(run("wsoff", "const alt = require('alt-client');\n"
                         "const связь = new alt.WebSocketClient('ws://127.0.0.1:1/');\n"
                         "const слушатель = () => {};\n"
                         "связь.on('open', слушатель);\n"
                         "alt.log('подписан: ' + связь.getEventListeners('open').length);\n"
                         "связь.off('open', слушатель);\n"
                         "alt.log('отписан: ' + связь.getEventListeners('open').length);\n"));

    CHECK(said("подписан: 1"));
    CHECK(said("отписан: 0"));
}

// Своих заголовков при рукопожатии стандартный WebSocket не принимает. Молчать
// об этом нельзя, а бросать — тем более: вызов стоит посреди чужой настройки
// связи, и брошенное отсюда унесло бы с собой всё, что идёт следом.
TEST_CASE("a websocket says once that it cannot carry extra headers", "[client][js]") {
    REQUIRE(run("wshead", "const alt = require('alt-client');\n"
                          "const связь = new alt.WebSocketClient('ws://127.0.0.1:1/');\n"
                          "связь.setExtraHeader('X-Ключ', 'секрет');\n"
                          "alt.log('после заголовка выполнение продолжилось');\n"));

    CHECK(said("websocket.setExtraHeader"));
    CHECK(said("после заголовка выполнение продолжилось"));
}
