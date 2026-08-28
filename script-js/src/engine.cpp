#include <oxymp/script/js/engine.hpp>

#include "node_library.hpp"
#include "resource.hpp"

#include <spdlog/spdlog.h>

#include <cppgc/platform.h>
#include <node.h>
#include <v8.h>

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oxymp::script::js {
namespace {

/// Сколько потоков движок берёт себе под свои задачи.
///
/// Это не потоки скриптов: скрипт исполняется в одном, на такте сервера. Здесь —
/// уборка мусора, отложенная компиляция и прочая работа V8, которую он умеет
/// делать в стороне. Четыре — то же число, что берёт себе Node по умолчанию на
/// обычной машине.
constexpr int kBackgroundThreads = 4;

/// Приставка, под которой до слоя alt:V доходит объявленное ресурсом.
///
/// См. `NodeEngine::announce`: ею разведены два потока событий, приходящих под
/// одним именем.
constexpr std::string_view kLocalPrefix = "local:";

/// Имена событий жизни ресурса — целиком, а не собранные из кусков.
///
/// Собранное имя работает точно так же, но его не найти поиском — ни человеку,
/// ни машинной сверке с объявлениями alt:V, по которой здесь считают паритет.
/// Спрятанное в склейке, оно числится отсутствующим; на этом уже попались имена
/// зон.
constexpr std::string_view kResourceStart = "resourceStart";
constexpr std::string_view kResourceStop = "resourceStop";
constexpr std::string_view kAnyResourceStart = "anyResourceStart";

/// Имя события о неподнявшемся ресурсе. Отдельной строкой, а не собранное из
/// кусков: паритет считается сверкой имён с объявлениями alt:V, и спрятанное в
/// склейке имя числится отсутствующим.
constexpr std::string_view kAnyResourceError = "anyResourceError";
constexpr std::string_view kAnyResourceStop = "anyResourceStop";

/// Строка в вид, годный для JSON.
///
/// Доводы событий между ресурсами ходят строкой JSON (см. `Resource::deliver` и
/// `decodeLocal` в слое alt:V), и имя ресурса, попавшее туда как есть, развалило
/// бы разбор на первой же кавычке в имени. Имена ресурсов — это имена каталогов,
/// и кавычка в них законна.
///
/// Свой, а не nlohmann: здесь укладывается одна строка, а тянуть ради неё
/// заголовок на десять тысяч строк в модуль, который и так собирается вместе с
/// V8, не за что.
///
/// Названа не `quoted`, и это не вкусовщина: у стандартной библиотеки есть
/// `std::quoted`, и поиск по доводу находит её вместе с нашей — довод-то
/// `std::string` из `std`. Побеждала при этом она: её перегрузка принимает
/// `const std::string&` без преобразования, а наша — `string_view` с
/// преобразованием. Наружу это выходило непонятной руганью внутри `<format>` на
/// то, что у типа нет `parse`.
[[nodiscard]] std::string asJsonString(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');

    for (const char symbol : value) {
        switch (symbol) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            // Управляющие символы JSON запрещает голыми. В именах каталогов их
            // не бывает, но разбор на них всё равно споткнулся бы.
            if (static_cast<unsigned char>(symbol) < 0x20U) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(symbol));
            } else {
                out.push_back(symbol);
            }
            break;
        }
    }

    out.push_back('"');
    return out;
}

/// Node поднимается один раз на процесс — это его правило, не наше.
///
/// Отсюда и глобальные: платформа переживает все ресурсы и не разбирается до
/// конца работы. Разбирать её и поднимать заново Node не позволяет.
struct Process {
    std::shared_ptr<node::InitializationResult> initialization;
    std::unique_ptr<node::MultiIsolatePlatform> platform;
    bool ready = false;
};

/// Хозяйство Node живёт до конца процесса и намеренно не убирается.
///
/// Утечка здесь осознанная, и слово «утечка» ей не вполне подходит: память
/// возвращает система, когда процесс кончается, и вернуть её раньше всё равно
/// некому. А вот разобрать платформу вовремя нельзя: V8 держит на неё ссылку, и
/// порядок разрушения глобальных объектов языку не подчиняется — обычный
/// статический объект разрушился бы после того, как V8 перестал существовать,
/// или до того, как перестал в нём нуждаться. И то и другое даёт вылет на
/// выходе — самый неприятный из возможных, потому что случается уже после того,
/// как сервер попрощался.
///
/// Так поступают все, кто встраивает V8, и по той же причине.
Process& process() {
    static Process* const instance = new Process{};
    return *instance;
}

/// Поднимает Node, если он ещё не поднят. false — с объяснением в error.
[[nodiscard]] bool ensureProcess(std::string& error) {
    Process& node = process();

    if (node.ready) {
        return true;
    }

    // Сама библиотека движка — первым делом и до всякого обращения к Node:
    // подгружается она по требованию, и до этой строки в процессе её нет.
    if (!ensureNodeLibrary(error)) {
        return false;
    }

    // Своё имя, а не argv сервера: доводы командной строки oxyMP к Node не
    // относятся вовсе, и разбирать их ему нечем — он честно пожалуется на
    // неизвестный ключ и откажется подниматься.
    const std::vector<std::string> arguments{"oxymp-server"};

    // Платформу заводим сами и потому просим Node её не заводить: своя нужна,
    // чтобы прокручивать её задачи на такте сервера, а не отдавать это на волю
    // чужого цикла.
    //
    // Уборщик мусора C++ (cppgc) по той же причине отключён здесь и заводится
    // ниже: ему нужен распределитель страниц, а тот принадлежит платформе,
    // которой в это мгновение ещё нет. Порядок взят из собственного примера
    // встраивания Node — test/embedding/embedtest.cc.
    node.initialization = node::InitializeOncePerProcess(
        arguments,
        static_cast<node::ProcessInitializationFlags::Flags>(
            node::ProcessInitializationFlags::kNoInitializeV8 |
            node::ProcessInitializationFlags::kNoInitializeNodeV8Platform |
            node::ProcessInitializationFlags::kNoInitializeCppgc));

    if (node.initialization == nullptr) {
        error = "Node не поднялся";
        return false;
    }

    for (const std::string& complaint : node.initialization->errors()) {
        spdlog::warn("Node: {}", complaint);
    }

    if (node.initialization->early_return()) {
        error = node.initialization->errors().empty()
                    ? "Node прервал запуск"
                    : node.initialization->errors().front();
        return false;
    }

    node.platform = node::MultiIsolatePlatform::Create(kBackgroundThreads);

    v8::V8::InitializePlatform(node.platform.get());
    cppgc::InitializeProcess(node.platform->GetPageAllocator());
    v8::V8::Initialize();

    node.ready = true;

    spdlog::info("JS engine up: Node {}, V8 {}", NODE_VERSION_STRING,
                 v8::V8::GetVersion());

    return true;
}

/// Уводит ли путь за пределы каталога ресурса.
///
/// Проверяется потому, что рядом с ресурсами лежит и server.cfg, и настройки
/// сервера, и чужие ресурсы. Точка входа, названная как «../../server.cfg», не
/// должна и пробоваться.
[[nodiscard]] bool escapes(const std::filesystem::path& root,
                           const std::filesystem::path& candidate) {
    std::error_code failure;

    const std::filesystem::path realRoot = std::filesystem::weakly_canonical(root, failure);
    if (failure) {
        return true;
    }

    const std::filesystem::path realCandidate =
        std::filesystem::weakly_canonical(candidate, failure);
    if (failure) {
        return true;
    }

    const std::filesystem::path relative =
        std::filesystem::relative(realCandidate, realRoot, failure);

    return failure || relative.empty() || *relative.begin() == "..";
}

/// Движок поверх Node.
///
/// Он же слушатель событий сессии: подписывается один на всех и разносит
/// происходящее по поднятым ресурсам. Отдельная подписка на каждый ресурс дала
/// бы то же самое, но лишила бы возможности прокрутить их циклы событий разом —
/// а делать это надо ровно раз в такт.
class NodeEngine final : public Engine, private Listener {
public:
    NodeEngine(Core& core, Events& events) noexcept : core_(&core), events_(&events) {
        events_->subscribe(this);
    }

    ~NodeEngine() override {
        // Сперва отписка, потом уборка ресурсов: пока подписка жива, событие
        // может прийти в любое мгновение, а ресурсов уже не будет.
        events_->unsubscribe(this);
        resources_.clear();
    }

    bool start(std::string_view name, const std::filesystem::path& root,
               const std::filesystem::path& main, std::string& error) override {
        const std::string key{name};

        if (resources_.contains(key)) {
            error = "уже поднят";
            return false;
        }

        if (escapes(root, root / main)) {
            error = std::format("точка входа \"{}\" уводит за пределы ресурса", main.string());
            return false;
        }

        // Полный путь, а не относительный, и это требование Node, а не наша
        // придирчивость: по нему он строит __filename, __dirname и корень
        // поиска пакетов. Относительный он отвергает прямо — «filename must be
        // an absolute path».
        //
        // Заодно расходятся разделители: собранный из «resources/example» и
        // «server/index.js» путь несёт и обратную косую черту, и прямую.
        std::error_code failure;
        const std::filesystem::path entry = std::filesystem::weakly_canonical(root / main, failure);

        if (failure || !std::filesystem::exists(entry)) {
            error = std::format("точки входа \"{}\" нет", (root / main).string());
            return false;
        }

        auto resource = std::make_unique<Resource>(key, root, *core_, *process().platform);

        // Список соседей ставится до запуска, а не после: ресурс вправе
        // спросить о них прямо из точки входа, и ответить ему к тому мгновению
        // уже должно быть чем. Себя он в этом списке не увидит — его туда ещё
        // не положили, — и это верно: спрашивают о соседях, а про себя есть
        // `Resource.current`.
        resource->onRoster([this] {
            std::vector<std::pair<std::string, std::string>> everyone;
            everyone.reserve(resources_.size());

            for (const auto& [key, each] : resources_) {
                everyone.emplace_back(key, each->root().string());
            }

            return everyone;
        });

        if (!resource->start(entry, error)) {
            // Неподнявшийся получает свой `resourceStart` с признаком «сломан»,
            // а соседи — `anyResourceError`. Молчать нельзя: ресурс, который не
            // встал, выглядит для соседей точно так же, как ресурс, которого не
            // просили, — а на соседей режимы вешают свою сборку.
            //
            // Ему самому — до разбора: разобранный уже ничего не услышит.
            resource->deliver(std::string{kLocalPrefix} + std::string{kResourceStart},
                              "[true]");

            announce(kAnyResourceError, std::format("[{}]", asJsonString(key)));
            return false;
        }

        // Объявленное этим ресурсом разносит движок: только он знает про
        // остальные. Ставится это после запуска — ресурс волен объявить
        // что-нибудь прямо из точки входа, и слушать его к тому мгновению уже
        // должно быть кому.
        resource->onAnnounce([this](std::string_view name, std::string_view payload) {
            announce(name, payload);
        });

        Resource& started = *resource;
        resources_.emplace(key, std::move(resource));

        // Жизнь ресурса объявляется ему самому и соседям — четырьмя событиями
        // alt:V, которых у нас не было вовсе.
        //
        // Порядок здесь значим. Сперва ресурс кладётся в список, потом ему
        // объявляют `resourceStart`: обработчик первым делом спрашивает
        // `alt.Resource.current`, и не окажись он к тому мгновению в списке —
        // получил бы пустоту. Затем `anyResourceStart` слышат все, включая его
        // самого: так же поступает и alt:V.
        //
        // Довод `resourceStart` — «поднялся ли с ошибкой». У нас он всегда
        // ложь: ресурс, отказавший при запуске, до этого места не доходит
        // вовсе — `start` вернул бы false выше. Место за доводом держится
        // затем, что режим читает его по счёту.
        // Под той же приставкой, что и объявленное ресурсами: доводы здесь
        // тоже уложены в JSON, и разбирать их должен тот же мостик. Без неё
        // обработчик получил бы строку «[false]» вместо признака — набор это и
        // поймал.
        started.deliver(std::string{kLocalPrefix} + std::string{kResourceStart},
                        "[false]");
        announce(kAnyResourceStart, std::format("[{}]", asJsonString(key)));

        return true;
    }

    bool stop(std::string_view name) override {
        const std::string key{name};
        const auto found = resources_.find(key);

        if (found == resources_.end()) {
            return false;
        }

        // Объявляется до разбора, а не после: обработчик застаёт свой ресурс
        // ещё живым — с окнами, таймерами и подписками, — и убрать за собой ему
        // есть чем. Объяви мы это после, объявлять было бы уже нечему: изолята
        // не осталось бы. То же правило и у `playerDisconnect`.
        found->second->deliver(std::string{kLocalPrefix} + std::string{kResourceStop}, "[]");
        announce(kAnyResourceStop, std::format("[{}]", asJsonString(key)));

        resources_.erase(found);
        return true;
    }

    [[nodiscard]] std::size_t running() const noexcept override { return resources_.size(); }

private:
    /// Разносит по всем ресурсам то, что объявил один из них.
    ///
    /// Всем, включая объявившего: у alt:V `alt.emit` слышит и тот, кто его
    /// позвал, если он на это имя подписан. Здесь так же — иначе ресурс,
    /// говорящий сам с собой через шину, вёл бы себя не как у alt:V.
    void announce(std::string_view name, std::string_view payload) {
        // Имя уезжает с приставкой, и это не украшение.
        //
        // **Двум потокам событий нужно разойтись, и по форме доводов их не
        // развести.** Через одно и то же имя до слоя alt:V доходят события
        // сессии — их зовёт ядро с готовыми доводами — и события, объявленные
        // ресурсом, у которых довод один, строкой JSON. Прежде слой различал их
        // счётом доводов: «один довод и он строка — значит от ресурса».
        //
        // Стоило это первого же события сессии, у которого довод оказался
        // ровно один и строкой: `consoleCommand("stop")` без доводов слой принял
        // за объявление ресурса, попробовал разобрать «stop» как JSON и,
        // разумеется, не смог — обработчику пришло имя `undefined`. Команда с
        // доводами при этом работала: у неё доводов было больше одного.
        //
        // Приставка снимает вопрос совсем: `local:` не может прийти из ядра, а
        // ядро не может прислать `local:`. Гадать больше не о чем.
        // Ограничение глубины — от бесконечности, а не от вкуса. Ресурс волен
        // объявить из обработчика то же самое имя, и без предела сервер завис
        // бы намертво, не сказав ни слова: снаружи это неотличимо от
        // зависшего обработчика, и искать причину было бы не по чему.
        if (announcing_ >= kMaxAnnounceDepth) {
            spdlog::error("event \"{}\" announced itself deeper than {} times — "
                          "not spreading it further",
                          name, kMaxAnnounceDepth);
            return;
        }

        ++announcing_;

        const std::string local = std::string{kLocalPrefix} + std::string{name};

        for (const auto& [key, resource] : resources_) {
            resource->deliver(local, payload);
        }

        --announcing_;
    }

    /// Насколько глубоко событие вправе объявить само себя.
    static constexpr int kMaxAnnounceDepth = 16;

    int announcing_ = 0;

    bool handle(const Event& event) override {
        // Такт — единственное место, где движку дают поработать самому: таймеры,
        // обещания, ввод-вывод. Прокрутка идёт до рассылки, а не после, и это
        // важно: обработчик такта, поставивший setTimeout(fn, 0), увидит его
        // сработавшим на следующем такте, а не через два.
        if (event.kind == EventKind::Tick) {
            for (const auto& [name, resource] : resources_) {
                resource->pump();
            }
        }

        bool proceed = true;

        for (const auto& [name, resource] : resources_) {
            // Отказ одного ресурса не отменяет рассылки остальным, и это не
            // небрежность. Ресурсы независимы: чат-фильтр, отменивший реплику,
            // не должен лишать журналирующий ресурс возможности её записать.
            if (!resource->dispatch(event)) {
                proceed = false;
            }
        }

        return proceed;
    }

    Core* core_ = nullptr;
    Events* events_ = nullptr;

    /// Поднятые ресурсы по имени.
    ///
    /// Упорядоченная, а не хеш-таблица: порядок обхода должен быть один и тот
    /// же от запуска к запуску, иначе порядок получения событий зависел бы от
    /// расположения строк в памяти.
    std::map<std::string, std::unique_ptr<Resource>, std::less<>> resources_;
};

} // namespace

std::unique_ptr<Engine> Engine::create(Core& core, Events& events, std::string& error) {
    if (!ensureProcess(error)) {
        return nullptr;
    }

    return std::make_unique<NodeEngine>(core, events);
}

} // namespace oxymp::script::js
