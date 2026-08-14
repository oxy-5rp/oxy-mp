#include <oxymp/script/js/engine.hpp>

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

    spdlog::info("движок JS поднят: Node {}, V8 {}", NODE_VERSION_STRING,
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

        if (!resource->start(entry, error)) {
            return false;
        }

        resources_.emplace(key, std::move(resource));
        return true;
    }

    void stop(std::string_view name) override { resources_.erase(std::string{name}); }

    [[nodiscard]] std::size_t running() const noexcept override { return resources_.size(); }

private:
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
