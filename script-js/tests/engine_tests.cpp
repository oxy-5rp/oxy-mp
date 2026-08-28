// Проверки серверной скриптовой машины.
//
// Проверяется здесь одно — что движок переживает чужие ошибки. Сокета ни одна из
// них не поднимает: ядро подставное, ресурсы пишутся во временный каталог.
//
// **Набора у серверной машины до сих пор не было, и это дорого стоило.** У
// клиентской он есть, и та же поломка в ней ловилась проверкой «ресурс, который
// бросает, объявляется отказавшим». На сервере её ловить было нечем: он падал
// целиком — молча, с разбором стека V8 — от одной опечатки в чужом режиме.
//
// **Без Catch2, и это не небрежность.** Машина собирается с динамической
// библиотекой времени выполнения — иначе рядом с ней не живёт `libnode.dll`, —
// а Catch2 в этом проекте собран со статической, потому что с ней собран клиент.
// Смешать их в одном исполняемом файле нельзя: линковщик отвечает LNK2038, и он
// прав — две кучи в одном процессе рано или поздно портят память. Поэтому здесь
// свой разбор доводов на полсотни строк, а дробность в ctest та же: имя проверки
// уходит доводом, и каждая идёт своим процессом.

#include <oxymp/script/js/engine.hpp>

#include "../../script/tests/fake_core.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>

namespace {

using oxymp::script::Event;
using oxymp::script::EventKind;
using oxymp::script::Events;
using oxymp::script::js::Engine;
using oxymp::script::testing::FakeCore;

/// Что пошло не так. Пусто — проверка прошла.
std::string& failure() {
    static std::string reason;
    return reason;
}

void expect(bool condition, std::string_view what) {
    if (!condition && failure().empty()) {
        failure() = std::string{what};
    }
}

/// Каталог с ресурсом на диске, убирающий за собой.
class Sandbox {
public:
    Sandbox(std::string_view entry, std::string_view source) {
        root_ = std::filesystem::temp_directory_path() /
                ("oxymp-script-js-" + std::to_string(token()) + "-" + std::to_string(counter()));

        std::filesystem::create_directories(root_);

        std::ofstream file{root_ / entry, std::ios::binary};
        file << source;
    }

    ~Sandbox() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

private:
    [[nodiscard]] static int counter() {
        static int next = 0;
        return ++next;
    }

    [[nodiscard]] static unsigned token() {
        static const unsigned value = std::random_device{}();
        return value;
    }

    std::filesystem::path root_;
};

/// Шина событий сессии. Через неё проверки объявляют то, что объявляет сервер.
Events& bus() {
    static Events only;
    return only;
}

/// Поднятый движок.
///
/// Один на прогон: Node заводится один раз на процесс — это его правило, не
/// наше, и поднять его заново нельзя.
Engine* engine() {
    static FakeCore core;
    Events& events = bus();
    static std::string error;
    static const std::unique_ptr<Engine> only = Engine::create(core, events, error);

    if (only == nullptr && failure().empty()) {
        failure() = "движок не поднялся: " + error;
    }

    return only.get();
}

void brokenEntryIsRefused() {
    const Sandbox broken{"index.js", R"js(throw new Error('нарочно ломаюсь');)js"};

    std::string error;

    expect(!engine()->start("broken", broken.root(), "index.js", error),
           "ресурс с бросающей точкой входа поднялся, а не должен был");

    // Причина названа. Молчаливый отказ здесь стоил бы дороже всего: хозяин
    // сервера видел бы, что режим не работает, и не знал бы почему.
    expect(error.find("нарочно ломаюсь") != std::string::npos,
           "в отказе нет причины: " + error);

    expect(engine()->running() == 0, "отказавший ресурс числится поднятым");
}

void absentNameIsRefused() {
    const Sandbox broken{"index.js", "alt.log('никакого alt здесь нет');\n"};

    std::string error;

    expect(!engine()->start("absent", broken.root(), "index.js", error),
           "ресурс с неизвестным именем поднялся");
    expect(error.find("alt") != std::string::npos, "в отказе не назван виновник: " + error);
}

void wholeResourceRisesAfterBrokenOne() {
    // Ради этой проверки всё и написано.
    //
    // Отказавший ресурс отпускает изолят, и ссылки, которые он держал, обязаны
    // уйти вместе с ним — до разбора, а не после. Две из четырёх однажды
    // остались, их деструкторы сработали по мёртвой памяти, и сервер падал
    // целиком: `Check failed: node->IsInUse()`. Снаружи это выглядело так, что
    // сервер молча умирает при запуске, и виноватого в журнале не было.
    const Sandbox broken{"index.js", R"js(throw new Error('нарочно');)js"};

    std::string error;
    expect(!engine()->start("first", broken.root(), "index.js", error),
           "сломанный ресурс поднялся");

    const Sandbox whole{"index.js", "const alt = require('alt-server');\n"
                                    "alt.log('второй целый');\n"};

    expect(engine()->start("second", whole.root(), "index.js", error),
           "целый ресурс не поднялся следом за сломанным: " + error);

    expect(engine()->running() == 1, "поднятых ресурсов не один");

    engine()->stop("second");
    expect(engine()->running() == 0, "остановленный ресурс числится поднятым");
}

void missingEntryIsNamed() {
    const Sandbox empty{"index.js", "\n"};

    std::string error;

    expect(!engine()->start("missing", empty.root(), "нет-такого.js", error),
           "ресурс без точки входа поднялся");
    expect(error.find("нет-такого.js") != std::string::npos,
           "в отказе не названа точка входа: " + error);
}

void escapingEntryIsRefused() {
    // Точка входа приходит из resource.toml, а его пишет хозяин режима. Уводящий
    // наружу путь — не обязательно злой умысел, но и пускать его незачем.
    const Sandbox resource{"index.js", "\n"};

    std::string error;

    expect(!engine()->start("escaping", resource.root(), "../../secret.js", error),
           "точка входа за пределами ресурса принята");
}

/// Что ресурс записал о себе на диск. Пусто — не записал ничего.
///
/// Через файл, а не через возвращённое значение, и обойти это нельзя: у каждого
/// ресурса свой изолят, и заглянуть в него отсюда нечем. Файл же он пишет тем
/// же `fs`, каким пользуются настоящие режимы.
[[nodiscard]] std::string wrote(const std::filesystem::path& where) {
    std::ifstream file{where, std::ios::binary};
    if (!file) {
        return {};
    }

    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void aResourceHearsItsOwnStart() {
    // `resourceStart` объявляется самому ресурсу, и объявляется после того, как
    // он попал в список поднятых: обработчик первым делом спрашивает
    // `alt.Resource.current`, и до списка ответить ему было бы нечем.
    const Sandbox resource{"index.js",
                           "const alt = require('alt-server');\n"
                           "const fs = require('fs');\n"
                           "const path = require('path');\n"
                           "alt.on('resourceStart', (errored) =>\n"
                           "    fs.writeFileSync(path.join(__dirname, 'started.txt'),\n"
                           "                     `${errored} ${alt.Resource.current.name}`));\n"};

    std::string error;
    expect(engine()->start("mine", resource.root(), "index.js", error),
           "ресурс не поднялся: " + error);

    // Довод — «поднялся ли с ошибкой», и он ложь: отказавший ресурс до этого
    // события не доходит вовсе.
    expect(wrote(resource.root() / "started.txt") == "false mine",
           "resourceStart не объявлен или пришёл не с тем: " +
               wrote(resource.root() / "started.txt"));

    engine()->stop("mine");
}

/// О ресурсе, который не поднялся, соседи узнают.
///
/// Молчать нельзя: не вставший ресурс выглядит для соседей точно так же, как
/// ресурс, которого не просили, — а на соседей режимы вешают свою сборку.
void aRefusedResourceIsAnnouncedToNeighbours() {
    const Sandbox first{"index.js", R"js(
const alt = require('alt-server');
const fs = require('fs');
const path = require('path');

alt.on('anyResourceError', (name) =>
    fs.writeFileSync(path.join(__dirname, 'broken.txt'), name));
)js"};

    std::string error;
    expect(engine()->start("first", first.root(), "index.js", error),
           "первый ресурс не поднялся: " + error);

    // Точка входа, которая падает на первой строке.
    const Sandbox broken{"index.js", R"js(throw new Error('нарочно');)js"};

    expect(!engine()->start("broken", broken.root(), "index.js", error),
           "сломанный ресурс поднялся, чего быть не должно");

    expect(wrote(first.root() / "broken.txt") == "broken",
           "сосед не услышал о сломанном: " + wrote(first.root() / "broken.txt"));

    engine()->stop("first");
}

void aResourceHearsAboutItsNeighbours() {
    const Sandbox first{"index.js",
                        "const alt = require('alt-server');\n"
                        "const fs = require('fs');\n"
                        "const path = require('path');\n"
                        "alt.on('anyResourceStart', (name) =>\n"
                        "    fs.writeFileSync(path.join(__dirname, 'saw.txt'), name));\n"
                        "alt.on('anyResourceStop', (name) =>\n"
                        "    fs.writeFileSync(path.join(__dirname, 'gone.txt'), name));\n"};

    std::string error;
    expect(engine()->start("first", first.root(), "index.js", error),
           "первый ресурс не поднялся: " + error);

    const Sandbox second{"index.js", "require('alt-server');\n"};
    expect(engine()->start("second", second.root(), "index.js", error),
           "второй ресурс не поднялся: " + error);

    expect(wrote(first.root() / "saw.txt") == "second",
           "сосед не услышал о поднявшемся: " + wrote(first.root() / "saw.txt"));

    engine()->stop("second");

    // Об уходе соседа тоже говорят, и говорят до разбора: обработчик застаёт
    // уходящего ещё живым.
    expect(wrote(first.root() / "gone.txt") == "second",
           "сосед не услышал об ушедшем: " + wrote(first.root() / "gone.txt"));

    engine()->stop("first");
}

/// Отказ во входе доходит до ресурса со всем, что о нём известно.
///
/// Игрока в этом событии нет и быть не может: отказ случается раньше, чем игрок
/// заводится. Проверять его стоит именно поэтому — обработчик, написанный под
/// alt:V, начинается не с игрока, а с причины, и перепутанный порядок доводов
/// молчал бы: все три законны и по отдельности правдоподобны.
void aRefusedConnectionArrivesWhole() {
    const Sandbox resource{"index.js", R"js(
const alt = require('alt-server');
const fs = require('fs');
const path = require('path');

alt.on('playerConnectDenied', (reason, name, ip) =>
    fs.writeFileSync(path.join(__dirname, 'denied.txt'), `${reason}|${name}|${ip}`));
)js"};

    std::string error;
    expect(engine()->start("denied", resource.root(), "index.js", error),
           "ресурс не поднялся: " + error);

    Event denied;
    denied.kind = EventKind::PlayerConnectDenied;
    denied.reason = 5;
    denied.name = "oxy";
    denied.text = "127.0.0.1";

    (void)bus().dispatch(denied);

    expect(wrote(resource.root() / "denied.txt") == "5|oxy|127.0.0.1",
           "отказ пришёл не тем: " + wrote(resource.root() / "denied.txt"));
}

/// Событие сессии с единственным строковым доводом доходит целым.
///
/// **Ровно здесь слой однажды промахнулся, и промахнулся молча.** Через одно имя
/// до него доходят двое: события сессии, у которых доводы готовые, и объявленные
/// ресурсом, у которых довод один — строка JSON. Различал он их счётом доводов —
/// «один довод и он строка, значит от ресурса», — и первое же событие сессии,
/// подошедшее под это описание, разобралось как чужое: `consoleCommand('stop')`
/// без доводов пришёл обработчику именем `undefined`.
///
/// Беда сидела ровно в половине случаев: команда с доводами работала. Поймать её
/// набором нельзя было ничем — она живёт на стыке ядра и слоя, и видно её только
/// изнутри поднятого ресурса.
void aSessionEventWithOneStringArrivesWhole() {
    // Сырой литерал, а не строка с переносами: JavaScript внутри читается как
    // JavaScript, а не как лестница из кавычек.
    const Sandbox resource{"index.js", R"js(
const alt = require('alt-server');
const fs = require('fs');
const path = require('path');

alt.on('consoleCommand', (name, ...args) =>
    fs.writeFileSync(path.join(__dirname, 'typed.txt'), `${name}|${args.length}`));
)js"};

    std::string error;
    expect(engine()->start("console", resource.root(), "index.js", error),
           "ресурс не поднялся: " + error);

    Event typed;
    typed.kind = EventKind::ConsoleCommand;
    typed.name = "stop";

    (void)bus().dispatch(typed);

    expect(wrote(resource.root() / "typed.txt") == "stop|0",
           "команда без доводов пришла не той: " + wrote(resource.root() / "typed.txt"));

    // И с доводами — тем же путём, чтобы видно было, что починка не сломала
    // вторую половину.
    Event withArguments;
    withArguments.kind = EventKind::ConsoleCommand;
    withArguments.name = "kick";
    withArguments.arguments = {"oxy", "за дело"};

    (void)bus().dispatch(withArguments);

    expect(wrote(resource.root() / "typed.txt") == "kick|2",
           "команда с доводами пришла не той: " + wrote(resource.root() / "typed.txt"));

    engine()->stop("console");
}

/// Событие, объявленное ресурсом, по-прежнему доходит разобранным.
///
/// Пара к предыдущей: разведя два потока по именам, легко было увести не туда
/// второй. Здесь ресурс объявляет событие сам себе — так же, как это делает
/// `alt.emit` у alt:V.
///
/// Объявляется оно из обработчика, а не из точки входа, и это не прихоть:
/// разнос объявленного (`onAnnounce`) ставится движком **после** запуска
/// ресурса, и `alt.emit`, позванный прямо из точки входа, не доходит никуда.
/// Проверять здесь надо тот путь, которым событиями пользуются на самом деле, —
/// из обработчика.
void aResourceStillHearsItsOwnEmit() {
    const Sandbox resource{"index.js", R"js(
const alt = require('alt-server');
const fs = require('fs');
const path = require('path');

alt.on('своё', (число, слово) =>
    fs.writeFileSync(path.join(__dirname, 'own.txt'), `${число}|${слово}`));

alt.on('consoleCommand', () => alt.emit('своё', 42, 'привет'));
)js"};

    std::string error;
    expect(engine()->start("own", resource.root(), "index.js", error),
           "ресурс не поднялся: " + error);

    Event typed;
    typed.kind = EventKind::ConsoleCommand;
    typed.name = "давай";

    (void)bus().dispatch(typed);

    // Число осталось числом, а строка строкой: доводы объявленного ресурсом
    // ходят уложенными в JSON, и разбирает их отдельный мостик.
    expect(wrote(resource.root() / "own.txt") == "42|привет",
           "объявленное ресурсом пришло не тем: " + wrote(resource.root() / "own.txt"));

    engine()->stop("own");
}

const std::map<std::string, std::function<void()>>& cases() {
    static const std::map<std::string, std::function<void()>> known{
        {"a-session-event-with-one-string-arrives-whole",
         &aSessionEventWithOneStringArrivesWhole},
        {"a-refused-connection-arrives-whole", &aRefusedConnectionArrivesWhole},
        {"a-resource-still-hears-its-own-emit", &aResourceStillHearsItsOwnEmit},
        {"a-resource-hears-its-own-start", &aResourceHearsItsOwnStart},
        {"a-resource-hears-about-its-neighbours", &aResourceHearsAboutItsNeighbours},
        {"a-refused-resource-is-announced-to-neighbours",
         &aRefusedResourceIsAnnouncedToNeighbours},
        {"broken-entry-is-refused", &brokenEntryIsRefused},
        {"absent-name-is-refused", &absentNameIsRefused},
        {"whole-resource-rises-after-broken-one", &wholeResourceRisesAfterBrokenOne},
        {"missing-entry-is-named", &missingEntryIsNamed},
        {"escaping-entry-is-refused", &escapingEntryIsRefused},
    };

    return known;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("нужно имя проверки. Известные:\n");

        for (const auto& [name, run] : cases()) {
            std::printf("  %s\n", name.c_str());
        }

        return 2;
    }

    const auto found = cases().find(argv[1]);

    if (found == cases().end()) {
        std::printf("нет такой проверки: %s\n", argv[1]);
        return 2;
    }

    if (engine() == nullptr) {
        std::printf("%s\n", failure().c_str());
        return 1;
    }

    found->second();

    if (!failure().empty()) {
        std::printf("%s\n", failure().c_str());
        return 1;
    }

    return 0;
}
