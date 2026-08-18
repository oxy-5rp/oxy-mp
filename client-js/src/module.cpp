// Точка входа клиентской скриптовой машины.
//
// Здесь живёт всё, что видно снаружи: одна экспортируемая функция и таблица
// действий за ней. Всё остальное — изоляты, привязки, слой alt — закрыто и
// наружу не торчит.

#include <oxymp/client/js/abi.h>

#include "convert.hpp"
#include "node_library.hpp"
#include "resource.hpp"

#include <cppgc/platform.h>
#include <node.h>
#include <v8.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oxymp::client::js {
namespace {

/// Сколько потоков движок берёт себе под свои задачи.
///
/// Два, а не четыре, как на сервере, и это не экономия ради экономии. Машина
/// живёт внутри процесса игры и делит с ней ядра: игре они нужны под кадр, и
/// отнятое у неё видно сразу — просадкой, а не цифрой в журнале. Уборка мусора
/// и отложенная компиляция потерпят.
constexpr int kBackgroundThreads = 2;

/// Хозяйство Node живёт до конца процесса и намеренно не убирается.
///
/// Та же причина, что и на сервере: V8 держит на платформу ссылку, а порядок
/// разрушения глобальных объектов языку не подчиняется. Разобранная не вовремя,
/// она даёт вылет на выходе — здесь это вылет игры при закрытии.
struct Process {
    std::unique_ptr<node::MultiIsolatePlatform> platform;
    std::shared_ptr<node::InitializationResult> initialization;
    bool ready = false;
};

Process& process() {
    static Process* const instance = new Process{};
    return *instance;
}

/// Что клиент попросил у нас делать. Копия, а не указатель.
///
/// Копия обязательна: клиент передаёт структуру со своего стека, и после выхода
/// из точки входа её там уже нет. Указатель на неё пережил бы её саму.
OxympJsHost& host() {
    static OxympJsHost instance{};
    return instance;
}

/// Поднятые ресурсы по имени.
///
/// Упорядоченная, а не хеш-таблица: порядок обхода должен быть один и тот же от
/// запуска к запуску, иначе порядок получения событий зависел бы от расположения
/// строк в памяти.
std::map<std::string, std::unique_ptr<Resource>, std::less<>>& resources() {
    static auto* const instance = new std::map<std::string, std::unique_ptr<Resource>,
                                               std::less<>>{};
    return *instance;
}

void report(OxympJsLogLevel level, std::string_view line) {
    const OxympJsHost& to = host();

    if (to.log != nullptr) {
        to.log(to.context, level, toAbi("client-js"), toAbi(line));
    }
}

std::int32_t setUp() {
    Process& node = process();

    if (node.ready) {
        return 1;
    }

    // Сама библиотека движка — первым делом и до всякого обращения к Node:
    // подгружается она по требованию, и до этой строки в процессе её нет.
    if (!ensureNodeLibrary()) {
        report(kOxympJsLogError, "libnode.dll не найдена рядом с машиной");
        return 0;
    }

    // Своё имя, а не argv игры: доводы командной строки GTA к Node не относятся
    // вовсе, и разбирать их ему нечем — он честно пожалуется и не поднимется.
    const std::vector<std::string> arguments{"oxymp-client"};

    node.initialization = node::InitializeOncePerProcess(
        arguments,
        static_cast<node::ProcessInitializationFlags::Flags>(
            node::ProcessInitializationFlags::kNoInitializeV8 |
            node::ProcessInitializationFlags::kNoInitializeNodeV8Platform |
            node::ProcessInitializationFlags::kNoInitializeCppgc |
            // Обработчики сигналов и разбор стека при падении остаются за игрой.
            // Node охотно ставит свои, и тогда вылет игры разбирал бы он — а
            // разбирал бы неверно, потому что стек там не его.
            node::ProcessInitializationFlags::kNoDefaultSignalHandling));

    if (node.initialization == nullptr) {
        report(kOxympJsLogError, "Node не поднялся");
        return 0;
    }

    for (const std::string& complaint : node.initialization->errors()) {
        report(kOxympJsLogWarning, complaint);
    }

    if (node.initialization->early_return()) {
        report(kOxympJsLogError, node.initialization->errors().empty()
                                     ? "Node прервал запуск"
                                     : node.initialization->errors().front());
        return 0;
    }

    node.platform = node::MultiIsolatePlatform::Create(kBackgroundThreads);

    v8::V8::InitializePlatform(node.platform.get());
    cppgc::InitializeProcess(node.platform->GetPageAllocator());
    v8::V8::Initialize();

    node.ready = true;

    report(kOxympJsLogInfo, "движок JS клиента поднят");
    return 1;
}

std::int32_t startResource(OxympJsText name, OxympJsText root, OxympJsText entry) {
    if (!process().ready) {
        return 0;
    }

    const std::string key = fromAbi(name);

    if (key.empty() || resources().contains(key)) {
        return 0;
    }

    const std::filesystem::path rootPath = fromAbi(root);
    const std::filesystem::path entryPath = rootPath / fromAbi(entry);

    std::error_code failure;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(entryPath, failure);

    if (failure || !std::filesystem::exists(resolved)) {
        report(kOxympJsLogError, "точки входа \"" + entryPath.string() + "\" нет");
        return 0;
    }

    auto resource = std::make_unique<Resource>(key, rootPath, host(), *process().platform);

    if (!resource->start(resolved)) {
        return 0;
    }

    resources().emplace(key, std::move(resource));
    return 1;
}

void stopResource(OxympJsText name) {
    resources().erase(fromAbi(name));
}

void tick() {
    for (const auto& [name, resource] : resources()) {
        resource->pump();
    }
}

/// Разносит событие по всем поднятым ресурсам.
///
/// Всем, а не одному: имя события придумывает ресурс, и слушать чужое имя ему
/// никто не запрещает — так же, как и на сервере.
void deliver(std::string_view name, std::string_view payload) {
    for (const auto& [key, resource] : resources()) {
        resource->dispatch(name, payload);
    }
}

void dispatchServerEvent(OxympJsText name, OxympJsBytes payload) {
    deliver("server:" + fromAbi(name),
            std::string_view{reinterpret_cast<const char*>(payload.data),
                             static_cast<std::size_t>(payload.length)});
}

void dispatchWebViewEvent(std::uint32_t view, OxympJsText name, OxympJsBytes payload) {
    // Номер окна уезжает в имя события, а не в доводы, и это не хитрость. Слой
    // на JavaScript держит по списку подписок на окно, и разводить их по имени
    // дешевле, чем звать всех подряд и просить каждого сверить номер.
    deliver("view:" + std::to_string(view) + ":" + fromAbi(name),
            std::string_view{reinterpret_cast<const char*>(payload.data),
                             static_cast<std::size_t>(payload.length)});
}

void dispatchSessionEvent(OxympJsText name) {
    deliver(fromAbi(name), {});
}

void dispatchKey(std::uint32_t key, std::int32_t down) {
    deliver(down != 0 ? "keydown" : "keyup", std::to_string(key));
}

void tearDown() {
    // Ресурсы разбираются, платформа — нет: см. Process выше.
    resources().clear();
}

constexpr OxympJsEngine kEngine{
    &setUp,
    &startResource,
    &stopResource,
    &tick,
    &dispatchServerEvent,
    &dispatchWebViewEvent,
    &dispatchSessionEvent,
    &dispatchKey,
    &tearDown,
};

} // namespace
} // namespace oxymp::client::js

extern "C" __declspec(dllexport) const OxympJsEngine* oxympClientJsEntry(
    std::uint32_t abiVersion, const OxympJsHost* requested) {
    // Проверка версии — первым делом и до всякого обращения к полям.
    //
    // Иначе структура от прошлой сборки была бы истолкована по нынешней
    // раскладке: смещения разъехались бы, и мы позвали бы по указателю, который
    // указывает не туда. Отказ здесь честнее падения там.
    if (abiVersion != OXYMP_CLIENT_JS_ABI_VERSION || requested == nullptr) {
        return nullptr;
    }

    oxymp::client::js::host() = *requested;

    return &oxymp::client::js::kEngine;
}
