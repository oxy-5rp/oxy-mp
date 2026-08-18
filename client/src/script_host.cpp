#include "script_host.hpp"

#include "game/native_call.hpp"
#include "game/native_table.hpp"

#include <oxymp/client/js/abi.h>

#include <spdlog/spdlog.h>

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace oxymp::client {
namespace {

/// Где машина лежит относительно клиента.
///
/// Тот же путь знает и сборка (OXYMP_NODE_SUBDIRECTORY в cmake/node.cmake), и
/// раскладка раздачи. Разъехаться им нельзя.
constexpr const wchar_t* kModuleDirectory = L"modules";
constexpr const wchar_t* kModuleName = L"js";
constexpr const wchar_t* kLibrary = L"oxymp-client-js.dll";

[[nodiscard]] std::string_view view(OxympJsText text) {
    return text.data == nullptr ? std::string_view{}
                                : std::string_view{text.data,
                                                   static_cast<std::size_t>(text.length)};
}

[[nodiscard]] std::string_view view(OxympJsBytes bytes) {
    return bytes.data == nullptr
               ? std::string_view{}
               : std::string_view{reinterpret_cast<const char*>(bytes.data),
                                  static_cast<std::size_t>(bytes.length)};
}

[[nodiscard]] OxympJsText text(std::string_view value) {
    return OxympJsText{value.data(), static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] OxympJsBytes bytes(std::string_view value) {
    return OxympJsBytes{reinterpret_cast<const std::uint8_t*>(value.data()),
                        static_cast<std::uint32_t>(value.size())};
}

} // namespace

/// Внутренности, зависящие от заголовка границы.
struct ScriptHost::State {
    HMODULE library = nullptr;
    const OxympJsEngine* engine = nullptr;
    OxympJsHost host{};
    Hooks hooks;

    /// Имя игрока, отданное машине последним.
    ///
    /// Живёт здесь, а не во временной строке, потому что через границу уходит
    /// указатель: отдай мы его на временную, машина прочла бы освобождённую
    /// память. Граница обещает ровно это — строка жива до следующего вызова.
    std::string lastName;
};

namespace {

/// Клиент, которому принадлежит машина.
///
/// Одиночка, и это не небрежность. Машина отдаёт `context` обратно в каждом
/// вызове, и его хватило бы — но Node поднимается один раз на процесс, а значит
/// и машина в процессе может быть только одна. Заводить видимость второй,
/// раздавая указатели, значило бы обещать то, чего не бывает.
ScriptHost::State*& current() {
    static ScriptHost::State* instance = nullptr;
    return instance;
}

void onLog(void*, OxympJsLogLevel level, OxympJsText resource, OxympJsText line) {
    const std::string_view name = view(resource);
    const std::string_view message = view(line);

    switch (level) {
    case kOxympJsLogWarning:
        spdlog::warn("[{}] {}", name, message);
        return;
    case kOxympJsLogError:
        spdlog::error("[{}] {}", name, message);
        return;
    case kOxympJsLogDebug:
        spdlog::debug("[{}] {}", name, message);
        return;
    case kOxympJsLogInfo:
        break;
    }

    spdlog::info("[{}] {}", name, message);
}

void onEmitServer(void*, OxympJsText name, OxympJsBytes payload) {
    ScriptHost::State* const state = current();

    if (state != nullptr && state->hooks.emitServer) {
        state->hooks.emitServer(view(name), view(payload));
    }
}

/// Вызов натива игры.
///
/// Ячейки перекладываются в контекст вызова как есть: что в них лежит — целое,
/// биты дробного или указатель, — решено на той стороне, где известна подпись
/// натива. Здесь остаётся найти обработчик и позвать его.
std::int32_t onCallNative(void*, std::uint64_t hash, const std::uint64_t* arguments,
                          std::uint32_t argumentCount, std::uint64_t* results,
                          std::uint32_t resultCapacity) {
    ScriptHost::State* const state = current();

    if (state == nullptr || state->hooks.natives == nullptr) {
        return 0;
    }

    const game::NativeHandler handler = state->hooks.natives->handlerFor(hash);

    // Пусто — натива с таким хешем в игре нет. Звать по нулевому указателю
    // нельзя, а выдумать ответ — тем более: скрипт получит отказ и узнает о нём.
    if (handler == nullptr) {
        return 0;
    }

    game::NativeContext context;

    for (std::uint32_t i = 0; i < argumentCount && i < game::NativeContext::kMaxArguments; ++i) {
        context.push(arguments[i]);
    }

    handler(context.address());

    // Ответ забирается ячейками целиком: вектор возвращается тремя подряд, и
    // сколько их значимо, знает та сторона, где известна подпись.
    for (std::uint32_t i = 0; i < resultCapacity && i < game::NativeContext::kMaxArguments; ++i) {
        results[i] = context.result<std::uint64_t>(i);
    }

    return 1;
}

std::int32_t onLocalPlayerId(void*) {
    ScriptHost::State* const state = current();

    // −1, а не ноль: ноль — это законный номер первого игрока, и выдать его за
    // «нас ещё не приняли» значило бы отдать ресурсу чужое имя.
    return state != nullptr && state->hooks.localPlayerId ? state->hooks.localPlayerId() : -1;
}

OxympJsBytes onReadResourceFile(void*, OxympJsText, OxympJsText) {
    // Файлы ресурса клиент уже разложил на диске (см. ResourceCache), и машина
    // читает их обычным путём — своим require. Отдельный путь через границу
    // понадобится тогда, когда ресурсы перестанут ложиться файлами.
    return OxympJsBytes{nullptr, 0};
}

/// Список сущностей сессии этого рода.
///
/// Заполняет сколько влезло, возвращает сколько есть. Обрезать молча нельзя:
/// обрезанный список выглядит как полный, и искать потом «пропавшего игрока»
/// пришлось бы в самом ресурсе, где всё верно.
std::uint32_t onListEntities(void*, OxympJsEntityKind kind, OxympJsEntity* entities,
                             std::uint32_t capacity) {
    ScriptHost::State* const state = current();

    if (state == nullptr) {
        return 0;
    }

    const auto& source = kind == kOxympJsEntityVehicle ? state->hooks.entities.vehicles
                                                       : state->hooks.entities.players;

    if (!source) {
        return 0;
    }

    const std::vector<ScriptHost::Hooks::Entity> found = source();

    for (std::size_t i = 0; i < found.size() && i < capacity; ++i) {
        entities[i] = OxympJsEntity{.id = found[i].id, .handle = found[i].handle};
    }

    return static_cast<std::uint32_t>(found.size());
}

std::int32_t onEntityHandle(void*, OxympJsEntityKind kind, std::int32_t id) {
    ScriptHost::State* const state = current();

    if (state == nullptr) {
        return 0;
    }

    const auto& resolve = kind == kOxympJsEntityVehicle ? state->hooks.entities.carOf
                                                        : state->hooks.entities.pedOf;

    return resolve ? resolve(id) : 0;
}

std::int32_t onEntityId(void*, OxympJsEntityKind kind, std::int32_t handle) {
    ScriptHost::State* const state = current();

    if (state == nullptr) {
        return -1;
    }

    const auto& resolve = kind == kOxympJsEntityVehicle ? state->hooks.entities.vehicleAt
                                                        : state->hooks.entities.playerAt;

    return resolve ? resolve(handle) : -1;
}

OxympJsText onPlayerName(void*, std::int32_t id) {
    ScriptHost::State* const state = current();

    if (state == nullptr || !state->hooks.entities.nameOf) {
        return OxympJsText{nullptr, 0};
    }

    state->lastName = state->hooks.entities.nameOf(id);

    return text(state->lastName);
}

std::uint32_t onCreateWebView(void*, OxympJsText resource, OxympJsText url) {
    ScriptHost::State* const state = current();

    if (state == nullptr || !state->hooks.createView) {
        return 0;
    }

    return state->hooks.createView(view(resource), view(url));
}

void onDestroyWebView(void*, std::uint32_t view_) {
    ScriptHost::State* const state = current();

    if (state != nullptr && state->hooks.destroyView) {
        state->hooks.destroyView(view_);
    }
}

void onEmitWebView(void*, std::uint32_t view_, OxympJsText name, OxympJsBytes payload) {
    ScriptHost::State* const state = current();

    if (state != nullptr && state->hooks.emitView) {
        state->hooks.emitView(view_, view(name), view(payload));
    }
}

void onSetWebViewVisible(void*, std::uint32_t view_, std::int32_t visible) {
    ScriptHost::State* const state = current();

    if (state != nullptr && state->hooks.showView) {
        state->hooks.showView(view_, visible != 0);
    }
}

void onSetWebViewFocused(void*, std::uint32_t view_, std::int32_t focused) {
    ScriptHost::State* const state = current();

    if (state != nullptr && state->hooks.focusView) {
        state->hooks.focusView(view_, focused != 0);
    }
}

} // namespace

std::unique_ptr<ScriptHost> ScriptHost::load(const std::filesystem::path& clientDirectory,
                                             Hooks hooks, std::string& error) {
    // Вторую машину в процессе завести нельзя: Node поднимается один раз.
    if (current() != nullptr) {
        error = "скриптовая машина уже поднята";
        return nullptr;
    }

    const std::filesystem::path library =
        clientDirectory / kModuleDirectory / kModuleName / kLibrary;

    std::error_code failure;
    if (!std::filesystem::is_regular_file(library, failure)) {
        error = "нет " + library.string();
        return nullptr;
    }

    // LOAD_WITH_ALTERED_SEARCH_PATH заставляет Windows искать зависимости самой
    // библиотеки рядом с ней, а не рядом с GTA5.exe. Без этого машина не нашла
    // бы ни libnode.dll, ни своей библиотеки времени выполнения: для внедрённого
    // модуля «рядом с исполняемым файлом» — это папка игры, куда oxyMP не кладёт
    // ничего.
    const HMODULE handle =
        ::LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (handle == nullptr) {
        error = "не загрузилась " + library.string();
        return nullptr;
    }

    const auto entry = reinterpret_cast<OxympJsEntry>(
        reinterpret_cast<void*>(::GetProcAddress(handle, OXYMP_CLIENT_JS_ENTRY_NAME)));

    if (entry == nullptr) {
        ::FreeLibrary(handle);
        error = "в машине нет точки входа";
        return nullptr;
    }

    auto state = std::make_unique<State>();
    state->library = handle;
    state->hooks = std::move(hooks);

    state->host.context = state.get();
    state->host.log = &onLog;
    state->host.emitServer = &onEmitServer;
    state->host.callNative = &onCallNative;
    state->host.localPlayerId = &onLocalPlayerId;
    state->host.readResourceFile = &onReadResourceFile;
    state->host.listEntities = &onListEntities;
    state->host.entityHandle = &onEntityHandle;
    state->host.entityId = &onEntityId;
    state->host.playerName = &onPlayerName;
    state->host.createWebView = &onCreateWebView;
    state->host.destroyWebView = &onDestroyWebView;
    state->host.emitWebView = &onEmitWebView;
    state->host.setWebViewVisible = &onSetWebViewVisible;
    state->host.setWebViewFocused = &onSetWebViewFocused;

    // Одиночка ставится до сверки версии: первое, что машина делает в ответ на
    // отказ, — говорит об этом в журнал, а журнал ей отдаём мы.
    current() = state.get();

    state->engine = entry(OXYMP_CLIENT_JS_ABI_VERSION, &state->host);

    if (state->engine == nullptr) {
        current() = nullptr;
        ::FreeLibrary(handle);
        error = "машина другой версии границы";
        return nullptr;
    }

    if (state->engine->setUp() == 0) {
        current() = nullptr;
        ::FreeLibrary(handle);
        error = "движок не поднялся";
        return nullptr;
    }

    // Конструктор закрыт, поэтому объект собирается здесь, а не make_unique.
    std::unique_ptr<ScriptHost> host{new ScriptHost{}};
    host->state_ = std::move(state);

    return host;
}

ScriptHost::~ScriptHost() {
    if (state_ == nullptr) {
        return;
    }

    if (state_->engine != nullptr) {
        state_->engine->tearDown();
    }

    current() = nullptr;

    // Библиотека не выгружается, и это намеренно. Node поднят на процесс и
    // держит свои потоки; выгрузи мы её здесь — они остались бы исполнять код по
    // адресам, которых больше нет. Игра пережила бы это ровно до следующего
    // кадра. Память вернёт система, когда процесс кончится.
}

bool ScriptHost::startResource(std::string_view name, const std::filesystem::path& root,
                               std::string_view entry) {
    if (state_ == nullptr || state_->engine == nullptr) {
        return false;
    }

    const std::string rootPath = root.string();

    return state_->engine->startResource(text(name), text(rootPath), text(entry)) != 0;
}

void ScriptHost::stopResource(std::string_view name) {
    if (state_ != nullptr && state_->engine != nullptr) {
        state_->engine->stopResource(text(name));
    }
}

void ScriptHost::tick() {
    if (state_ != nullptr && state_->engine != nullptr) {
        state_->engine->tick();
    }
}

void ScriptHost::serverEvent(std::string_view name, std::string_view payload) {
    if (state_ != nullptr && state_->engine != nullptr) {
        state_->engine->dispatchServerEvent(text(name), bytes(payload));
    }
}

void ScriptHost::viewEvent(std::uint32_t view_, std::string_view name, std::string_view payload) {
    if (state_ != nullptr && state_->engine != nullptr) {
        state_->engine->dispatchWebViewEvent(view_, text(name), bytes(payload));
    }
}

void ScriptHost::sessionEvent(std::string_view name) {
    if (state_ != nullptr && state_->engine != nullptr) {
        state_->engine->dispatchSessionEvent(text(name));
    }
}

void ScriptHost::key(std::uint32_t code, bool down) {
    if (state_ != nullptr && state_->engine != nullptr) {
        state_->engine->dispatchKey(code, down ? 1 : 0);
    }
}

} // namespace oxymp::client
