#include "resource.hpp"

#include "bindings.hpp"
#include "convert.hpp"

#include <alt_bootstrap.hpp>
#include <alt_enums.hpp>
#include <alt_objects.hpp>
#include <alt_server.hpp>
#include <alt_shared.hpp>

#include <spdlog/spdlog.h>

#include <uv.h>

#include <string>
#include <utility>

namespace oxymp::script::js {
namespace {

/// Как событие сессии называется в скрипте.
///
/// Имена подобраны так же, как у alt:V и RAGE MP, и это не подражание: тот, кто
/// уже писал под них, не должен заново гадать, называется ли вход игрока
/// playerConnect или playerJoin.
[[nodiscard]] std::string_view nameOf(EventKind kind) noexcept {
    switch (kind) {
    case EventKind::PlayerConnect:
        return "playerConnect";
    case EventKind::PlayerDisconnect:
        return "playerDisconnect";
    case EventKind::PlayerSpawn:
        return "playerSpawn";
    case EventKind::PlayerDeath:
        return "playerDeath";
    case EventKind::PlayerChat:
        return "playerChat";
    case EventKind::VehicleCreate:
        return "vehicleCreate";
    case EventKind::VehicleDestroy:
        return "vehicleDestroy";
    case EventKind::Tick:
        return "tick";
    case EventKind::ClientEvent:
        // Отдельного имени нет: события от клиента различаются своим именем, и
        // собирается оно в dispatch.
        return {};
    }

    return {};
}

/// Что исполняется в ресурсе первым.
///
/// Пять встроенных файлов подряд, в строгом порядке: перечисления, общая часть
/// API alt:V, серверная часть, серверные объекты, запуск. Порядок не
/// переставляется — каждый следующий собирается из предыдущего, — и потому
/// склеены они здесь, а не отдаются движку по одному: одна склейка исполняется
/// одним вызовом и в одном контексте, а пять вызовов пришлось бы ещё и
/// проверять по отдельности.
///
/// Сами файлы лежат в script-js/js и встраиваются в бинарник при сборке
/// (cmake/embed_text.cmake). Не рядом с сервером — файл рядом можно потерять,
/// перепутать версией или подменить, а слой alt:V обязан совпадать со сборкой
/// сервера точно.
[[nodiscard]] std::string buildBootstrap() {
    std::string script;

    script.reserve(embedded::altEnums.size() + embedded::altShared.size() +
                   embedded::altServer.size() + embedded::altObjects.size() +
                   embedded::altBootstrap.size() + 5U);

    // Перевод строки между файлами обязателен: последняя строка одного и первая
    // другого иначе слились бы в одну.
    for (const std::string_view part : {embedded::altEnums, embedded::altShared,
                                        embedded::altServer, embedded::altObjects,
                                        embedded::altBootstrap}) {
        script.append(part);
        script.push_back('\n');
    }

    return script;
}

} // namespace

Resource::Resource(std::string name, std::filesystem::path root, Core& core,
                   node::MultiIsolatePlatform& platform)
    : name_(std::move(name)),
      root_(std::move(root)),
      core_(&core),
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
        playerShape_.Reset();
        vehicleShape_.Reset();
    }

    // Остановка — снаружи блокировки, и это не вкусовщина: так делает
    // собственный пример встраивания Node (test/embedding/embedtest.cc). Она
    // сама входит в изолят там, где ей нужно, и завершает его; сделай мы это
    // изнутри — охранники на стеке пережили бы то, что охраняют.
    node::Stop(setup_->env());
    setup_.reset();
}

Resource* Resource::of(v8::Isolate* isolate) noexcept {
    return isolate == nullptr ? nullptr : static_cast<Resource*>(isolate->GetData(kIsolateSlot));
}

bool Resource::start(const std::filesystem::path& main, std::string& error) {
    // Точка входа уезжает в argv вторым доводом: Node строит по нему __filename,
    // __dirname и корень поиска пакетов. Иначе ресурс искал бы node_modules
    // рядом с сервером, а не рядом с собой.
    const std::vector<std::string> arguments{"oxymp-server", main.string()};
    const std::vector<std::string> execArguments{};

    // Инспектор отключается, и без этого второй ресурс роняет сервер.
    //
    // Отладчик Node заводит на процесс один поток ввода-вывода и проверяет это
    // утверждением: второе окружение попадает в
    // `Assertion failed: (start_io_thread_async_initialized.exchange(true)) ==
    // (false)` внутри inspector_agent.cc и завершает процесс целиком. Не
    // исключение, которое можно поймать, — падение с разбором стека.
    //
    // Проявлялось это так, что сервер с двумя ресурсами на JS поднимал первый и
    // молча умирал на втором: в журнале оставалась строка про первый и больше
    // ничего. Одному ресурсу — а до сих пор их и был один — не мешало ничто.
    //
    // Отладчик при этом не теряется совсем: один ресурс мог бы его получить,
    // если однажды понадобится. Но тогда это должен быть осознанный выбор
    // одного из них, а не то, кому повезло подняться первым.
    constexpr auto kEnvironmentFlags = static_cast<node::EnvironmentFlags::Flags>(
        node::EnvironmentFlags::kDefaultFlags | node::EnvironmentFlags::kNoCreateInspector);

    std::vector<std::string> complaints;
    setup_ = node::CommonEnvironmentSetup::Create(platform_, &complaints, arguments,
                                                  execArguments, kEnvironmentFlags);

    if (setup_ == nullptr) {
        error = complaints.empty() ? "окружение Node не создалось" : complaints.front();
        return false;
    }

    v8::Isolate* const isolate = setup_->isolate();
    isolate->SetData(kIsolateSlot, this);

    bool loaded = false;

    // Отдельным блоком: уборка неудавшегося ресурса разбирает изолят, и делать
    // это можно только после того, как охранники на стеке сняты. Разрушенный
    // изолят они бы пережили.
    {
        const v8::Locker locker{isolate};
        const v8::Isolate::Scope isolateScope{isolate};
        const v8::HandleScope handles{isolate};
        const v8::Local<v8::Context> context = setup_->context();
        const v8::Context::Scope contextScope{context};

        installBindings(*this, context);

        // Ловушка нужна своя: без неё Node напечатает ошибку сам и завершит
        // процесс, а нам сервер ронять нельзя — виноват один ресурс, а не сессия.
        const v8::TryCatch caught{isolate};

        loaded = !node::LoadEnvironment(setup_->env(), buildBootstrap()).IsEmpty();

        // Точка входа грузится обещанием: к этой строке она ещё не исполнилась.
        //
        // Ждём здесь, а не отдаём наверх «поднимется когда-нибудь»: хозяину
        // сервера нужно сказать, поднялся ресурс или нет, а ошибка в его первой
        // строке должна попасть в журнал отказом ресурса — не необработанным
        // отказом обещания спустя такт, когда связать её уже не с чем.
        // Уборка при отказе — не здесь: она разбирает изолят, а делать это можно
        // только после того, как охранники на стеке сняты. Здесь только признак.
        const bool started = loaded && awaitStart(context, isolate, error);

        if (loaded && !started) {
            loaded = false;

            handlers_.clear();
            playerShape_.Reset();
            vehicleShape_.Reset();
        } else if (!loaded) {
            if (caught.HasCaught()) {
                error = fromJs(isolate, caught.Exception());

                // Место ошибки ценнее её текста: «x is not a function» без
                // строки ищут часами.
                const v8::Local<v8::Message> message = caught.Message();
                if (!message.IsEmpty()) {
                    int line = 0;
                    (void)message->GetLineNumber(context).To(&line);

                    error += std::format(" ({}:{})",
                                         fromJs(isolate, message->GetScriptResourceName()), line);
                }
            } else {
                error = "точка входа не исполнилась";
            }

            handlers_.clear();
            playerShape_.Reset();
            vehicleShape_.Reset();
        }
    }

    if (!loaded) {
        node::Stop(setup_->env());
        setup_.reset();
        return false;
    }

    return true;
}

bool Resource::awaitStart(v8::Local<v8::Context> context, v8::Isolate* isolate,
                          std::string& error) {
    // Предел нужен не от медленного ресурса, а от зависшего: обещание, которое
    // никогда не разрешится, оставило бы нас крутиться навсегда. Двухсот
    // оборотов хватает с большим запасом — загрузчик модулей укладывается в
    // единицы, а тяжёлое ресурс делает уже после подъёма.
    constexpr int kStartSpins = 200;

    const v8::Local<v8::Object> global = context->Global();

    for (int spin = 0; spin < kStartSpins; ++spin) {
        (void)uv_run(setup_->event_loop(), UV_RUN_NOWAIT);
        platform_->DrainTasks(isolate);
        isolate->PerformMicrotaskCheckpoint();

        v8::Local<v8::Value> state;
        if (!global->Get(context, toJs(isolate, "__oxympStart")).ToLocal(&state) ||
            !state->IsObject()) {
            error = "слой не объявил признака запуска";
            return false;
        }

        const v8::Local<v8::Object> started = state.As<v8::Object>();

        v8::Local<v8::Value> done;
        if (!started->Get(context, toJs(isolate, "готов")).ToLocal(&done) ||
            !done->BooleanValue(isolate)) {
            continue;
        }

        v8::Local<v8::Value> failure;
        if (!started->Get(context, toJs(isolate, "ошибка")).ToLocal(&failure)) {
            return true;
        }

        // Пустота проверяется через StrictEquals: IsNull и IsUndefined
        // определены и в заголовке V8, и в libnode, и линковщик отказывается
        // выбирать между ними (LNK2005).
        if (failure->StrictEquals(v8::Null(isolate)) ||
            failure->StrictEquals(v8::Undefined(isolate))) {
            return true;
        }

        // Стек ценнее текста: «x is not a function» без места ищут часами.
        if (failure->IsObject()) {
            v8::Local<v8::Value> stack;

            if (failure.As<v8::Object>()->Get(context, toJs(isolate, "stack")).ToLocal(&stack) &&
                stack->IsString()) {
                error = fromJs(isolate, stack);
                return false;
            }
        }

        error = fromJs(isolate, failure);
        return false;
    }

    error = "точка входа не исполнилась за отведённое время";
    return false;
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
    // хоть что-нибудь, — то есть до конца сессии, если ресурс завёл таймер. Такт
    // сервера этого не переживёт.
    (void)uv_run(setup_->event_loop(), UV_RUN_NOWAIT);

    // Задачи движка — уборка мусора, разбор отложенной компиляции — идут своей
    // очередью, и без этого вызова не идут вовсе.
    platform_->DrainTasks(isolate);

    // Обещания разрешаются в конце такта, а не когда придётся: иначе `await`
    // в обработчике продолжился бы неизвестно когда.
    isolate->PerformMicrotaskCheckpoint();
}

void Resource::subscribe(std::string name, v8::Local<v8::Function> handler) {
    handlers_[std::move(name)].emplace_back(setup_->isolate(), handler);
}

v8::Local<v8::FunctionTemplate> Resource::playerShape() const {
    return playerShape_.IsEmpty() ? v8::Local<v8::FunctionTemplate>{}
                                  : playerShape_.Get(setup_->isolate());
}

v8::Local<v8::FunctionTemplate> Resource::vehicleShape() const {
    return vehicleShape_.IsEmpty() ? v8::Local<v8::FunctionTemplate>{}
                                   : vehicleShape_.Get(setup_->isolate());
}

void Resource::setPlayerShape(v8::Local<v8::FunctionTemplate> value) {
    playerShape_.Reset(setup_->isolate(), value);
}

void Resource::setVehicleShape(v8::Local<v8::FunctionTemplate> value) {
    vehicleShape_.Reset(setup_->isolate(), value);
}

bool Resource::dispatch(const Event& event) {
    if (setup_ == nullptr) {
        return true;
    }

    v8::Isolate* const isolate = setup_->isolate();

    const v8::Locker locker{isolate};
    const v8::Isolate::Scope isolateScope{isolate};
    const v8::HandleScope handles{isolate};
    const v8::Local<v8::Context> context = setup_->context();
    const v8::Context::Scope contextScope{context};

    // Ловушка на всё, что здесь происходит, включая сборку доводов.
    //
    // Заведена не для порядка: исключение, брошенное вне ловушки, Node считает
    // роковым и завершает процесс — вместе с сессией и всеми, кто в ней. Одна
    // опечатка в привязке не должна стоить сервера.
    const v8::TryCatch caught{isolate};

    std::vector<v8::Local<v8::Value>> arguments;

    // Событие от клиента различается своим именем, а не видом: имя придумывает
    // ресурс, и совпасть с именем события сессии оно не может — префикс разводит
    // их по разным полкам.
    const std::string name = event.kind == EventKind::ClientEvent
                                 ? "client:" + event.name
                                 : std::string{nameOf(event.kind)};

    if (name.empty()) {
        return true;
    }

    switch (event.kind) {
    case EventKind::PlayerConnect:
    case EventKind::PlayerSpawn:
        arguments.push_back(wrapPlayer(*this, context, event.player.id()));
        break;

    case EventKind::PlayerDisconnect:
        arguments.push_back(wrapPlayer(*this, context, event.player.id()));
        arguments.push_back(toJs(isolate, event.text));
        break;

    case EventKind::PlayerDeath:
        arguments.push_back(wrapPlayer(*this, context, event.player.id()));

        // Смерть без убийцы — падение, утопление, воля скрипта. Пустота честнее
        // игрока с недействительным номером: тот выглядел бы настоящим.
        arguments.push_back(event.killer.valid()
                                ? wrapPlayer(*this, context, event.killer.id())
                                : v8::Local<v8::Value>{v8::Null(isolate)});
        arguments.push_back(v8::Number::New(isolate, static_cast<double>(event.weapon)));
        break;

    case EventKind::PlayerChat:
        arguments.push_back(wrapPlayer(*this, context, event.player.id()));
        arguments.push_back(toJs(isolate, event.text));
        break;

    case EventKind::VehicleCreate:
    case EventKind::VehicleDestroy:
        arguments.push_back(wrapVehicle(*this, context, event.vehicle.id()));
        break;

    case EventKind::ClientEvent:
        arguments.push_back(wrapPlayer(*this, context, event.player.id()));
        arguments.push_back(toJs(isolate, event.text));
        break;

    case EventKind::Tick:
        break;
    }

    if (caught.HasCaught()) {
        report(context, caught);
        return true;
    }

    return call(context, name, arguments);
}

bool Resource::call(v8::Local<v8::Context> context, std::string_view name,
                    std::vector<v8::Local<v8::Value>>& arguments) {
    const auto found = handlers_.find(std::string{name});
    if (found == handlers_.end()) {
        return true;
    }

    v8::Isolate* const isolate = context->GetIsolate();
    bool proceed = true;

    // Копия списка нарочно: обработчик волен подписать ещё одного прямо отсюда,
    // и перебор по живому списку сломался бы на первой же такой подписке.
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
        const bool ok = handler
                            ->Call(context, context->Global(), static_cast<int>(arguments.size()),
                                   arguments.data())
                            .ToLocal(&outcome);

        if (!ok) {
            // Упавший обработчик не останавливает ни остальных, ни сервер: одна
            // ошибка в одном режиме не повод обрывать сессию всем.
            report(context, caught);
            continue;
        }

        // Отмена считается только явным false. Обработчик, ничего не вернувший,
        // отменять не собирался — а undefined ложен, и считай мы его отказом,
        // всякая подписка на реплику молча съедала бы чат.
        if (outcome->IsBoolean() && !outcome->BooleanValue(isolate)) {
            proceed = false;
        }
    }

    return proceed;
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

    spdlog::error("[{}] ошибка в обработчике: {}{}", name_, fromJs(isolate, caught.Exception()),
                  where);
}

} // namespace oxymp::script::js
