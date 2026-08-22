#include "script_tick.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"
#include "native_table.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstddef>

#include <intrin.h>
#include <windows.h>

namespace oxymp::client::game {
namespace {

constexpr std::string_view kTickId = "script_thread_tick";
constexpr std::string_view kActiveThreadTlsId = "active_thread_tls_offset";
constexpr std::string_view kHandlerOffsetId = "script_handler_thread_offset";

/// Штатный тик скриптового потока.
///
/// Соглашение снято с дизассемблера: rcx — сам поток, edx — сколько операций
/// разрешено выполнить, в eax возвращается состояние потока. Возврат обязан
/// дойти до вызывающего в целости: по нему игра решает судьбу потока.
using TickFunction = std::uint32_t (*)(void* thread, std::uint32_t operations);

TickFunction g_original = nullptr;
ScriptTick::Callback g_callback = nullptr;
NativeHandler g_frameCount = nullptr;

/// Смещение в TLS, по которому лежит указатель на активный скриптовый поток.
std::size_t g_activeThreadOffset = 0;

/// Смещение обработчика скрипта внутри объекта скриптового потока.
std::size_t g_handlerOffset = 0;

/// Есть ли у потока обработчик скрипта.
///
/// Обработчик — это то, на чей счёт игра записывает сделанное нативом: заказ
/// модели, созданную машину, завершённый скрипт. Нативы достают его из активного
/// потока и разыменовывают, не проверяя, поэтому подставлять поток без
/// обработчика — верный вылет. Так и был получен вылет внутри REQUEST_MODEL.
bool hasScriptHandler(void* thread) noexcept {
    if (thread == nullptr || g_handlerOffset == 0) {
        return false;
    }

    return *reinterpret_cast<void* const*>(static_cast<char*>(thread) + g_handlerOffset) != nullptr;
}

/// Смещение массива TLS в блоке текущего потока.
///
/// Задано архитектурой x64 в Windows и потому константа: по gs:[0x58] лежит
/// указатель на массив блоков TLS, а первый его элемент — блок главного модуля,
/// то есть самой игры.
constexpr std::uint32_t kTlsArrayOffset = 0x58;

/// Ячейка с активным скриптовым потоком в TLS того потока, где мы сейчас.
///
/// Спрашивается каждый раз, а не запоминается: TLS у каждого потока свой, а
/// перехват ставится из одного потока, работает же в другом.
void** activeThreadSlot() noexcept {
    if (g_activeThreadOffset == 0) {
        return nullptr;
    }

    auto* const tlsArray = reinterpret_cast<char**>(__readgsqword(kTlsArrayOffset));
    if (tlsArray == nullptr || *tlsArray == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<void**>(*tlsArray + g_activeThreadOffset);
}

/// Записана ли в журнал сверка того, что игра оставляет в ячейке.
std::atomic<bool> g_slotReported{false};

std::atomic<std::uint64_t> g_ticks{0};
std::atomic<std::uint64_t> g_frames{0};

/// Поток, из которого игра зовёт тик. Снимается на первом же вызове.
std::atomic<std::uint32_t> g_gameThread{0};

/// Номер кадра, на котором обработчик уже отработал.
///
/// Не атомарный намеренно: тик приходит только с главного потока игры, и
/// синхронизировать здесь нечего. Атомарными оставлены лишь счётчики, которые
/// читаются снаружи.
std::int32_t g_lastFrame = -1;

std::uint32_t detour(void* thread, std::uint32_t operations) {
    g_ticks.fetch_add(1, std::memory_order_relaxed);

    // Снимается на каждом тике, а не однажды: дешевле проверки, а поток игры за
    // время работы не меняется — расхождения не будет.
    g_gameThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);

    // Сперва штатная работа игры, потом наша. Обратный порядок означал бы, что
    // мы рисуем поверх кадра, который игра ещё не начала собирать.
    const std::uint32_t state =
        g_original != nullptr ? g_original(thread, operations) : 0;

    if (g_callback == nullptr || g_frameCount == nullptr) {
        return state;
    }

    // Тик приходит по разу на каждый скриптовый поток, а обработчику положен
    // один раз за кадр. Счётчик кадров игры — самый дешёвый способ их различить.
    const auto frame = invokeNative<std::int32_t>(g_frameCount);
    if (frame == g_lastFrame) {
        return state;
    }

    g_lastFrame = frame;

    // Обработчик скрипта есть не у каждого потока, а во время загрузки — ни у
    // одного. Ждать подходящий нельзя: рисовать нужно как раз тогда, когда их
    // нет. Поэтому выполняемся от любого, а обработчику сообщаем, чего он
    // сейчас лишён.
    const bool ownsResources = hasScriptHandler(thread);

    g_frames.fetch_add(1, std::memory_order_relaxed);

    // Скриптовый контекст в игре не передаётся нативам, а подразумевается: они
    // читают активный поток из TLS. Тик, вернув управление, оставляет ячейку не
    // тем, что нужно нам, поэтому поток подставляется на время обработчика и
    // возвращается обратно сразу после.
    //
    // Без этого нативы, которым контекст безразличен, работают как ни в чём не
    // бывало — рисование, перенос игрока, — а те, что записывают сделанное на
    // счёт вызвавшего их скрипта, роняют игру. Так был получен вылет внутри
    // REQUEST_MODEL при живом и верном хеше натива.
    void** const slot = activeThreadSlot();
    void* const previous = slot != nullptr ? *slot : nullptr;

    if (!g_slotReported.exchange(true)) {
        spdlog::debug("первый кадр клиента: поток {}, обработчик скрипта {}", fmt::ptr(thread),
                     ownsResources ? "есть" : "отсутствует");
        spdlog::debug("активный поток в TLS до подмены: {}", fmt::ptr(previous));
    }

    if (slot != nullptr) {
        *slot = thread;
    }

    g_callback(ownsResources);

    if (slot != nullptr) {
        *slot = previous;
    }

    return state;
}

} // namespace

std::unique_ptr<ScriptTick> ScriptTick::install(const EngineAddresses& addresses,
                                                Callback callback, std::string& error) {
    auto* target = addresses.pointerTo<void*>(kTickId);
    if (target == nullptr) {
        error = "адрес тика скриптового потока не разрешён";
        return nullptr;
    }

    const NativeTable table{addresses};
    NativeHandler frameCount = table.handlerFor(natives::kGetFrameCount);
    if (frameCount == nullptr) {
        error = "натив счётчика кадров не найден — без него кадры не различить";
        return nullptr;
    }

    std::unique_ptr<ScriptTick> tick{new ScriptTick};

    // Всё, чем пользуется обработчик, выставляется до постановки перехвата:
    // после неё он может быть вызван в любое мгновение.
    g_callback = callback;
    g_frameCount = frameCount;
    g_activeThreadOffset = static_cast<std::size_t>(addresses[kActiveThreadTlsId]);
    g_handlerOffset = static_cast<std::size_t>(addresses[kHandlerOffsetId]);
    g_lastFrame = -1;
    g_ticks.store(0);
    g_frames.store(0);
    g_slotReported.store(false);

    if (g_activeThreadOffset == 0) {
        error = "смещение активного скриптового потока в TLS не разрешено";
        return nullptr;
    }
    if (g_handlerOffset == 0) {
        error = "смещение обработчика скрипта в потоке не разрешено";
        return nullptr;
    }

    if (!tick->hook_.install(target, reinterpret_cast<void*>(&detour), error)) {
        g_callback = nullptr;
        g_frameCount = nullptr;
        return nullptr;
    }

    g_original = tick->hook_.original<TickFunction>();

    spdlog::debug("перехват тика скриптов поставлен на {:#x}",
                 reinterpret_cast<std::uintptr_t>(target));

    return tick;
}

ScriptTick::~ScriptTick() {
    // Сперва снятие перехвата, и только потом обнуление того, чем он
    // пользуется: пока перехват стоит, обработчик обязан оставаться рабочим.
    hook_.remove();

    g_original = nullptr;
    g_callback = nullptr;
    g_frameCount = nullptr;
    g_activeThreadOffset = 0;
    g_handlerOffset = 0;
}

std::uint64_t ScriptTick::frames() const noexcept {
    return g_frames.load(std::memory_order_relaxed);
}

std::uint64_t ScriptTick::ticks() const noexcept {
    return g_ticks.load(std::memory_order_relaxed);
}

std::uint32_t ScriptTick::gameThreadId() noexcept {
    return g_gameThread.load(std::memory_order_relaxed);
}

} // namespace oxymp::client::game
