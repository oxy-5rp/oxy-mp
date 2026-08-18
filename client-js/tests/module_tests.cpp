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
#include <string>
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

OxympJsBytes onReadResourceFile(void*, OxympJsText, OxympJsText) {
    return OxympJsBytes{nullptr, 0};
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
        host_.readResourceFile = &onReadResourceFile;
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
    explicit Sandbox(std::string_view script) {
        root_ = std::filesystem::temp_directory_path() /
                ("oxymp-client-js-" + std::to_string(::GetTickCount64()) + "-" +
                 std::to_string(counter()++));

        std::filesystem::create_directories(root_);

        std::ofstream file{root_ / "index.js", std::ios::binary};
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
[[nodiscard]] bool run(std::string_view name, std::string_view script) {
    static std::vector<std::unique_ptr<Sandbox>> kept;

    kept.push_back(std::make_unique<Sandbox>(script));

    const std::string root = kept.back()->root();

    if (Engine::instance()->startResource(text(name), text(root), text("index.js")) == 0) {
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
                          "try { alt.Player(); } catch (e) { alt.log('отказ: ' + e.message); }\n"));

    CHECK(said("отказ: alt.Player: в oxyMP этого ещё нет"));
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
