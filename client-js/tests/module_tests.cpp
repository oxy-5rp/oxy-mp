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

    /// Каким номером клиент отвечает на вопрос машины.
    std::int32_t selfId = -1;
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

TEST_CASE("synced meta from the server reaches the client", "[client][js]") {
    // Метаданные приходят служебным событием, а не своим сообщением: событие
    // уже умеет ходить, а сообщение стоило бы номера в протоколе.
    REQUIRE(run("meta", "const alt = require('alt-client');\n"
                        "alt.on('syncedMetaChange', (род, номер, ключ, что) =>\n"
                        "    alt.log(`меняли ${род}:${номер}.${ключ} = ${что}`));\n"));

    Engine::instance()->dispatchServerEvent(text("__oxymp:meta"),
                                            bytes("[\"global\",0,\"погода\",\"дождь\"]"));

    CHECK(said("меняли global:0.погода = дождь"));
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
    REQUIRE(run("metaentity", "const alt = require('alt-client');\n"
                              "const чужой = new alt.Ped(123);\n"
                              "try { чужой.getSyncedMeta('x'); }\n"
                              "catch (e) { alt.log('отказ meta: ' + e.message); }\n"));

    CHECK(said("отказ meta: entity.getSyncedMeta: номер этой сущности клиенту неизвестен"));
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
