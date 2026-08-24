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

/// Поднятый движок.
///
/// Один на прогон: Node заводится один раз на процесс — это его правило, не
/// наше, и поднять его заново нельзя.
Engine* engine() {
    static FakeCore core;
    static Events events;
    static std::string error;
    static const std::unique_ptr<Engine> only = Engine::create(core, events, error);

    if (only == nullptr && failure().empty()) {
        failure() = "движок не поднялся: " + error;
    }

    return only.get();
}

void brokenEntryIsRefused() {
    const Sandbox broken{"index.js", "throw new Error('нарочно ломаюсь');\n"};

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
    const Sandbox broken{"index.js", "throw new Error('первый ломается');\n"};

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

const std::map<std::string, std::function<void()>>& cases() {
    static const std::map<std::string, std::function<void()>> known{
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
