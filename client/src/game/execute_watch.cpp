#include "execute_watch.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>
#include <format>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Сколько срабатываний записывать, прежде чем снять ловушку.
///
/// Три. Нужен один ответ, а не поток одинаковых: исключение на каждое исполнение
/// стоит дорого, а вызывающий у одной и той же инструкции почти всегда один и
/// тот же. Три — чтобы увидеть, что он и правда один.
constexpr int kMaxReports = 3;

/// Ловушка одна на процесс: обработчик исключений — простая функция без
/// состояния, и связать её с объектом иначе нельзя.
struct State {
    void* address = nullptr;
    std::uint32_t threadId = 0;
    std::string label;

    void* handler = nullptr;

    std::atomic<int> reported{0};
    std::atomic<bool> armed{false};
};

State* g_state = nullptr;

/// Куда именно пришёлся адрес: имя модуля и смещение в нём.
std::string locate(const void* address) {
    HMODULE module = nullptr;

    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(address), &module) == 0) {
        return "вне известных модулей";
    }

    std::wstring path(MAX_PATH, L'\0');
    const DWORD written =
        ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(written);

    const auto offset = static_cast<std::uintptr_t>(static_cast<const std::uint8_t*>(address) -
                                                    reinterpret_cast<std::uint8_t*>(module));

    return std::format("{}+{:#x}", std::filesystem::path{path}.filename().string(), offset);
}

/// Раскладывает отладочные регистры потока.
///
/// Делается это только для остановленного потока: пока он идёт, его регистры
/// принадлежат процессору, а не нам, и записанное в них не доедет.
bool armThread(HANDLE thread, void* address, bool armed) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    if (::GetThreadContext(thread, &context) == 0) {
        return false;
    }

    if (armed) {
        context.Dr0 = reinterpret_cast<DWORD64>(address);

        // Нулевой регистр, ловушка на исполнение, длина в один байт: биты 16-19
        // задают вид и длину, бит 0 включает саму ловушку. Исполнение — это
        // ноль в обоих полях вида и длины.
        context.Dr7 = (context.Dr7 & ~0xF0000ULL) | 1ULL;
    } else {
        context.Dr0 = 0;
        context.Dr7 &= ~1ULL;
    }

    return ::SetThreadContext(thread, &context) != 0;
}

/// Ставит и снимает ловушку из отдельного потока.
///
/// Из своего собственного потока это делать нельзя: чтобы разложить чужие
/// отладочные регистры, поток надо остановить, а поток, остановивший сам себя,
/// не возобновится никогда. Один раз на этом уже застряли намертво.
bool changeArming(std::uint32_t threadId, void* address, bool armed) {
    struct Request {
        std::uint32_t threadId;
        void* address;
        bool armed;
        bool done;
    };

    Request request{threadId, address, armed, false};

    const HANDLE worker = ::CreateThread(
        nullptr, 0,
        [](LPVOID raw) -> DWORD {
            auto& own = *static_cast<Request*>(raw);

            const HANDLE thread =
                ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                             FALSE, own.threadId);
            if (thread == nullptr) {
                return 0;
            }

            if (::SuspendThread(thread) != static_cast<DWORD>(-1)) {
                own.done = armThread(thread, own.address, own.armed);
                ::ResumeThread(thread);
            }

            ::CloseHandle(thread);
            return 0;
        },
        &request, 0, nullptr);

    if (worker == nullptr) {
        return false;
    }

    ::WaitForSingleObject(worker, 5000);
    ::CloseHandle(worker);

    return request.done;
}

/// Сколько ячеек стека просматривать в поисках обратных адресов.
///
/// Тридцать два — это четверть килобайта: столько занимают несколько кадров
/// вызова, а глубже ответа всё равно нет.
constexpr std::size_t kStackSlots = 32;

/// Сколько найденных адресов записывать.
constexpr int kMaxCallers = 6;

/// Похоже ли значение на адрес внутри кода запущенной игры.
bool insideGame(std::uintptr_t value) {
    const HMODULE game = ::GetModuleHandleW(nullptr);
    if (game == nullptr) {
        return false;
    }

    const auto* const base = reinterpret_cast<const std::uint8_t*>(game);

    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    const auto* const headers =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(base);
    const auto end = start + headers->OptionalHeader.SizeOfImage;

    return value >= start && value < end;
}

/// Записывает найденные на стеке адреса кода игры.
void reportCallers(std::uintptr_t rsp) {
    if (rsp == 0) {
        return;
    }

    const auto* const stack = reinterpret_cast<const std::uintptr_t*>(rsp);

    int found = 0;

    for (std::size_t slot = 0; slot < kStackSlots && found < kMaxCallers; ++slot) {
        const std::uintptr_t value = stack[slot];

        if (!insideGame(value)) {
            continue;
        }

        ++found;
        spdlog::info("  на стеке [{}]: {}", slot, locate(reinterpret_cast<const void*>(value)));
    }

    if (found == 0) {
        spdlog::info("  на стеке адресов кода игры не нашлось");
    }
}

LONG CALLBACK onException(EXCEPTION_POINTERS* pointers) {
    State* const state = g_state;

    if (state == nullptr || pointers == nullptr || pointers->ExceptionRecord == nullptr ||
        pointers->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT& context = *pointers->ContextRecord;

    // Ловушка ли это наша: процессор отмечает сработавший регистр в Dr6.
    if ((context.Dr6 & 1ULL) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    context.Dr6 = 0;

    const int seen = state->reported.fetch_add(1) + 1;

    spdlog::info("{}: сработала ловушка ({} из {}) — rcx={:#x} rdx={:#x} r8={:#x} r9={:#x}",
                 state->label, seen, kMaxReports, context.Rcx, context.Rdx, context.R8,
                 context.R9);

    // Стек просматривается целиком, а не только его вершина.
    //
    // На вершине обратный адрес лежит лишь в первой инструкции функции. Ловушка
    // же стоит там, где нужное нам действие происходит, — а это середина, и
    // сверху там что угодно: сохранённые регистры, свои переменные, выравнивание.
    //
    // Поэтому ищем: обратный адрес — это адрес внутри кода игры, и он на стеке
    // есть, просто неизвестно на каком месте. Найденные складываются по порядку,
    // и первые из них — ближайшие вызывающие.
    reportCallers(context.Rsp);

    if (seen >= kMaxReports) {
        // Снимается прямо здесь, в своём же потоке: этот поток уже остановлен
        // исключением, и раскладывать его регистры можно без всякой возни с
        // остановкой.
        context.Dr0 = 0;
        context.Dr7 &= ~1ULL;

        state->armed.store(false);
        spdlog::info("{}: ловушка снята, ответ получен", state->label);
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

std::unique_ptr<ExecuteWatch> ExecuteWatch::install(void* address, std::uint32_t threadId,
                                                    std::string label, std::string& error) {
    if (g_state != nullptr) {
        error = "ловушка на исполнение уже стоит";
        return nullptr;
    }

    if (address == nullptr || threadId == 0) {
        error = "нечего или некуда ставить";
        return nullptr;
    }

    std::unique_ptr<ExecuteWatch> watch{new ExecuteWatch};
    auto state = std::make_unique<State>();

    state->address = address;
    state->threadId = threadId;
    state->label = std::move(label);

    g_state = state.get();

    // Обработчик ставится до ловушки: сработать она может в тот же миг.
    state->handler = ::AddVectoredExceptionHandler(1, &onException);
    if (state->handler == nullptr) {
        g_state = nullptr;
        error = "не удалось поставить обработчик исключений";
        return nullptr;
    }

    if (!changeArming(threadId, address, true)) {
        ::RemoveVectoredExceptionHandler(state->handler);
        g_state = nullptr;
        error = "не удалось разложить отладочные регистры потока игры";
        return nullptr;
    }

    state->armed.store(true);
    state.release();

    spdlog::info("ловушка на исполнение поставлена: {}", locate(address));

    return watch;
}

ExecuteWatch::~ExecuteWatch() {
    if (g_state == nullptr) {
        return;
    }

    if (g_state->armed.exchange(false)) {
        changeArming(g_state->threadId, g_state->address, false);
    }

    if (g_state->handler != nullptr) {
        ::RemoveVectoredExceptionHandler(g_state->handler);
        g_state->handler = nullptr;
    }

    g_state = nullptr;
}

} // namespace oxymp::client::game
