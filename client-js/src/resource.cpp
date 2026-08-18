#include "resource.hpp"

#include "bindings.hpp"
#include "convert.hpp"

#include <alt_client.hpp>
#include <alt_client_bootstrap.hpp>
#include <alt_enums.hpp>
#include <alt_natives.hpp>
#include <alt_natives_table.hpp>
#include <alt_shared.hpp>

#include <uv.h>

#include <format>
#include <utility>

namespace oxymp::client::js {
namespace {

/// Что исполняется в ресурсе первым.
///
/// Перечисления и общая часть берутся у сервера дословно — те же самые файлы из
/// `script-js/js`. Это не переиспользование ради экономии: `alt-shared` потому и
/// зовётся общей частью, что на обеих сторонах она обязана быть одна и та же.
/// Разойдись здесь Vector3 или hash хоть в мелочи — и ресурс, считающий
/// расстояние на клиенте и на сервере, получал бы два разных ответа.
[[nodiscard]] std::string buildBootstrap() {
    std::string script;

    script.reserve(embedded::altEnums.size() + embedded::altShared.size() +
                   embedded::altNativesTable.size() + embedded::altNatives.size() +
                   embedded::altClient.size() + embedded::altClientBootstrap.size() + 6U);

    for (const std::string_view part : {embedded::altEnums, embedded::altShared,
                                        embedded::altNativesTable, embedded::altNatives,
                                        embedded::altClient, embedded::altClientBootstrap}) {
        script.append(part);
        script.push_back('\n');
    }

    return script;
}

} // namespace

Resource::Resource(std::string name, std::filesystem::path root, const OxympJsHost& host,
                   node::MultiIsolatePlatform& platform)
    : name_(std::move(name)),
      root_(std::move(root)),
      host_(&host),
      platform_(&platform) {}

Resource::~Resource() {
    if (setup_ == nullptr) {
        return;
    }

    v8::Isolate* const isolate = setup_->isolate();

    // Ссылки на функции отпускаются, войдя в изолят: держать v8::Global после
    // того, как изолят разобран, нельзя, а отпустить их можно только изнутри.
    {
        const v8::Locker locker{isolate};
        const v8::Isolate::Scope isolateScope{isolate};
        const v8::HandleScope handles{isolate};
        const v8::Context::Scope contextScope{setup_->context()};

        handlers_.clear();
    }

    // Остановка — снаружи блокировки: так делает собственный пример встраивания
    // Node. Она сама входит в изолят там, где ей нужно.
    node::Stop(setup_->env());
    setup_.reset();
}

Resource* Resource::of(v8::Isolate* isolate) noexcept {
    return isolate == nullptr ? nullptr : static_cast<Resource*>(isolate->GetData(kIsolateSlot));
}

void Resource::log(OxympJsLogLevel level, std::string_view line) const {
    if (host_ == nullptr || host_->log == nullptr) {
        return;
    }

    host_->log(host_->context, level, toAbi(name_), toAbi(line));
}

bool Resource::start(const std::filesystem::path& entry) {
    // Точка входа уезжает в argv вторым доводом: по нему Node строит __filename,
    // __dirname и корень поиска пакетов.
    const std::vector<std::string> arguments{"oxymp-client", entry.string()};
    const std::vector<std::string> execArguments{};

    // Инспектор отключается по той же причине, что и на сервере: он заводит на
    // процесс один поток ввода-вывода и роняет утверждением второе окружение.
    // Здесь это тем важнее, что процесс — сама игра.
    constexpr auto kEnvironmentFlags = static_cast<node::EnvironmentFlags::Flags>(
        node::EnvironmentFlags::kDefaultFlags | node::EnvironmentFlags::kNoCreateInspector);

    std::vector<std::string> complaints;
    setup_ = node::CommonEnvironmentSetup::Create(platform_, &complaints, arguments,
                                                  execArguments, kEnvironmentFlags);

    if (setup_ == nullptr) {
        log(kOxympJsLogError,
            complaints.empty() ? "окружение Node не создалось" : complaints.front());
        return false;
    }

    v8::Isolate* const isolate = setup_->isolate();
    isolate->SetData(kIsolateSlot, this);

    bool loaded = false;

    // Отдельным блоком: уборка неудавшегося ресурса разбирает изолят, и делать
    // это можно только после того, как охранники на стеке сняты.
    {
        const v8::Locker locker{isolate};
        const v8::Isolate::Scope isolateScope{isolate};
        const v8::HandleScope handles{isolate};
        const v8::Local<v8::Context> context = setup_->context();
        const v8::Context::Scope contextScope{context};

        installBindings(*this, context);

        // Ловушка нужна своя: без неё Node напечатает ошибку сам и завершит
        // процесс, а процесс здесь — игра.
        const v8::TryCatch caught{isolate};

        loaded = !node::LoadEnvironment(setup_->env(), buildBootstrap()).IsEmpty();

        if (!loaded) {
            std::string reason = caught.HasCaught() ? fromJs(isolate, caught.Exception())
                                                    : "точка входа не исполнилась";

            // Место ошибки ценнее её текста: сообщение вида «x is not a
            // function» без строки ищут часами.
            const v8::Local<v8::Message> message = caught.Message();
            if (!message.IsEmpty()) {
                int line = 0;
                (void)message->GetLineNumber(context).To(&line);

                reason += std::format(" ({}:{})", fromJs(isolate, message->GetScriptResourceName()),
                                      line);
            }

            log(kOxympJsLogError, reason);
            handlers_.clear();
        }
    }

    if (!loaded) {
        node::Stop(setup_->env());
        setup_.reset();
        return false;
    }

    return true;
}

void Resource::pump() {
    if (setup_ == nullptr) {
        return;
    }

    v8::Isolate* const isolate = setup_->isolate();

    const v8::Locker locker{isolate};
    const v8::Isolate::Scope isolateScope{isolate};
    const v8::HandleScope handles{isolate};
    const v8::Context::Scope contextScope{setup_->context()};

    // UV_RUN_NOWAIT, а не SpinEventLoop: последний крутится, пока в цикле есть
    // хоть что-нибудь, — то есть до конца сессии, если ресурс завёл таймер. Кадр
    // игры этого не переживёт.
    (void)uv_run(setup_->event_loop(), UV_RUN_NOWAIT);

    platform_->DrainTasks(isolate);
    isolate->PerformMicrotaskCheckpoint();
}

void Resource::subscribe(std::string name, v8::Local<v8::Function> handler) {
    if (setup_ == nullptr) {
        return;
    }

    handlers_[std::move(name)].emplace_back(setup_->isolate(), handler);
}

void Resource::dispatch(std::string_view name, std::string_view payload) {
    if (setup_ == nullptr) {
        return;
    }

    v8::Isolate* const isolate = setup_->isolate();

    const v8::Locker locker{isolate};
    const v8::Isolate::Scope isolateScope{isolate};
    const v8::HandleScope handles{isolate};
    const v8::Local<v8::Context> context = setup_->context();
    const v8::Context::Scope contextScope{context};

    const auto found = handlers_.find(std::string{name});
    if (found == handlers_.end()) {
        return;
    }

    const v8::TryCatch outer{isolate};

    v8::Local<v8::Value> arguments[] = {toJs(isolate, payload)};

    // Копия числа, а не списка: обработчик волен подписать ещё одного прямо
    // отсюда, и перебор по живому списку сломался бы на первой такой подписке.
    const std::size_t count = found->second.size();

    for (std::size_t i = 0; i < count; ++i) {
        // Список берётся заново на каждом шаге: карта могла перестроиться, пока
        // работал прошлый обработчик.
        const auto again = handlers_.find(std::string{name});
        if (again == handlers_.end() || i >= again->second.size()) {
            break;
        }

        const v8::Local<v8::Function> handler = again->second[i].Get(isolate);

        const v8::TryCatch caught{isolate};

        v8::Local<v8::Value> outcome;
        if (!handler->Call(context, context->Global(), 1, arguments).ToLocal(&outcome)) {
            // Упавший обработчик не останавливает ни остальных, ни игру: одна
            // ошибка в одном ресурсе не повод выкидывать игрока из сессии.
            report(context, caught);
        }
    }
}

void Resource::report(v8::Local<v8::Context> context, const v8::TryCatch& caught) const {
    v8::Isolate* const isolate = context->GetIsolate();

    std::string where;

    const v8::Local<v8::Message> message = caught.Message();
    if (!message.IsEmpty()) {
        int line = 0;
        (void)message->GetLineNumber(context).To(&line);

        where = std::format(" ({}:{})", fromJs(isolate, message->GetScriptResourceName()), line);
    }

    log(kOxympJsLogError,
        std::format("ошибка в обработчике: {}{}", fromJs(isolate, caught.Exception()), where));
}

} // namespace oxymp::client::js
