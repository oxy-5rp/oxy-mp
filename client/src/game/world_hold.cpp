#include "world_hold.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <thread>

#include <windows.h>

namespace oxymp::client::game {
namespace {

// Место, которое мы подменяем, — конец цикла ожидания раскрутки:
//
//     add  eax, -5
//     cmp  eax, 1
//     jbe  выход              ; состояния 5 и 6 — ждём дальше, ничего не меняем
//     mov  dword [состояние], 9   ; <- десять байт, которые мы забираем себе
//     jmp  выход
//
// Девятка означает «грузить мир», и записывает её игра ровно здесь — в тот миг,
// когда решила, что ждать больше нечего. Это и есть точка, в которой её нужно
// придержать: раньше — рано, позже — некуда.
//
// Число 9 входит в сигнатуру, и это не украшение: парой строк выше стоит такая
// же связка, пишущая 0x17, и без числа сигнатура совпала бы дважды.

/// Состояние «грузить мир».
constexpr std::int32_t kLoadWorldState = 9;

/// Сколько спать между оборотами цикла удержания.
///
/// Пятнадцать миллисекунд — то же, что у CitizenFX. Это не частота показа: кадр
/// крутит look_alive, а сон лишь не даёт циклу занять ядро целиком.
constexpr std::chrono::milliseconds kSpinPause{15};

/// Первые два байта подменяемой инструкции: `mov dword [rip+X], imm32`.
constexpr std::uint8_t kWriteOpcode[] = {0xC7, 0x05};

/// Длина подменяемой инструкции: два байта кода, четыре смещения, четыре числа.
constexpr std::size_t kWriteLength = 10;

/// Где внутри неё лежит записываемое число.
constexpr std::size_t kWriteImmediateOffset = 6;

/// `call rel32` — пять байт, остальные добиваются пустышками.
constexpr std::uint8_t kCallOpcode = 0xE8;
constexpr std::uint8_t kNop = 0x90;
constexpr std::size_t kCallLength = 5;

/// Переходник: `mov rax, адрес; jmp rax`.
///
/// Нужен затем, что `call rel32` дотягивается только на два гигабайта, а наш
/// модуль Windows кладёт в память где ей вздумается — рядом с игрой он
/// оказывается разве что случайно. Поэтому вызов идёт не к нам, а в клочок
/// памяти, выделенный рядом с самой игрой, и уже он прыгает по полному адресу.
constexpr std::uint8_t kThunkTemplate[] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, imm64
    0xFF, 0xE0,                         // jmp rax
};

/// Смещение адреса внутри переходника.
constexpr std::size_t kThunkAddressOffset = 2;

/// Насколько далеко от игры соглашаемся выделить переходник.
///
/// Меньше двух гигабайт, и с запасом: смещение в `call rel32` знаковое, и
/// упираться в самую его границу незачем.
constexpr std::uintptr_t kThunkReach = 0x7000'0000;

/// Пишет байты в код игры, открыв его на запись и закрыв обратно.
[[nodiscard]] bool writeCode(void* where, const void* what, std::size_t size,
                             std::string& error) {
    DWORD previous = 0;
    if (::VirtualProtect(where, size, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        error = std::format("не удалось открыть код игры для записи: код ошибки Windows {}",
                            ::GetLastError());
        return false;
    }

    std::memcpy(where, what, size);

    DWORD restored = 0;
    ::VirtualProtect(where, size, previous, &restored);

    ::FlushInstructionCache(::GetCurrentProcess(), where, size);

    return true;
}

/// Выделяет исполняемый клочок памяти в пределах досягаемости `call rel32`.
[[nodiscard]] void* allocateNear(std::uintptr_t anchor, std::size_t size) {
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);

    const std::uintptr_t step = info.dwAllocationGranularity;
    if (step == 0) {
        return nullptr;
    }

    // Шагаем от места записи в обе стороны: ближе — лучше, а какая из сторон
    // окажется свободной, заранее не известно.
    for (std::uintptr_t distance = step; distance < kThunkReach; distance += step) {
        const std::uintptr_t candidates[] = {
            anchor > distance ? (anchor - distance) & ~(step - 1) : 0,
            (anchor + distance) & ~(step - 1),
        };

        for (const std::uintptr_t candidate : candidates) {
            if (candidate == 0) {
                continue;
            }

            void* const memory =
                ::VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
            if (memory != nullptr) {
                return memory;
            }
        }
    }

    return nullptr;
}

} // namespace

struct WorldHold::State {
    /// Состояние раскрутки игры. Читается в цикле и меняется чужим потоком,
    /// оттого volatile: без него оптимизатор вправе прочитать его однажды.
    volatile std::int32_t* initState = nullptr;

    /// Куда мы вписали вызов.
    std::uint8_t* site = nullptr;

    /// Что там лежало до нас.
    std::uint8_t original[kWriteLength]{};

    /// Переходник рядом с игрой.
    void* thunk = nullptr;

    void (*lookAlive)() = nullptr;
    void (*servicing)() = nullptr;

    /// Просили ли отпустить.
    std::atomic<bool> releaseWanted{false};

    /// Сидит ли поток игры в нашем цикле прямо сейчас.
    ///
    /// Нужно при разборке: снимать за собой, пока чужой поток исполняет наш код,
    /// нельзя — модуль выгрузится, и игра уйдёт исполнять освобождённую память.
    std::atomic<bool> inLoop{false};

    /// Побывал ли поток игры у нас вообще.
    std::atomic<bool> visited{false};
};

WorldHold::State* WorldHold::active_ = nullptr;

/// Возврат из этой функции — и есть отпускание: поток игры вернётся ровно туда,
/// откуда пришёл, и пойдёт грузить мир, ничего не зная о задержке.
void WorldHold::gameThreadHold() {
    State* const hold = active_;
    if (hold == nullptr) {
        return;
    }

    hold->visited.store(true);

    const std::int32_t arrival = *hold->initState;

    // Отпустили — значит делаем то, ради чего игра сюда шла, и не задерживаем.
    // Сюда она приходит и после отпускания: место это в цикле, и второй раз
    // держать её было бы уже не удержанием, а поломкой.
    if (hold->releaseWanted.load()) {
        *hold->initState = kLoadWorldState;
        return;
    }

    hold->inLoop.store(true);

    spdlog::info("мир придержан: поток игры ждёт выбора сервера, состояние {}", arrival);

    std::int32_t lastSeen = arrival;

    while (!hold->releaseWanted.load()) {
        hold->servicing();
        hold->lookAlive();

        // Состояние меняется и пока мы держим: игра толкает его вперёд из того
        // же кадра, который крутим мы. Каждая перемена — строка в журнале: этим
        // местом разбирались дважды, и в следующий раз разбираться придётся по
        // нему же.
        if (const std::int32_t now = *hold->initState; now != lastSeen) {
            spdlog::info("раскрутка игры: состояние {} вместо {}", now, lastSeen);
            lastSeen = now;
        }

        std::this_thread::sleep_for(kSpinPause);
    }

    // Ровно то, что записала бы подменённая инструкция. Отпускание — это она же,
    // только сделанная позже.
    *hold->initState = kLoadWorldState;

    spdlog::info("мир отпущен: состояние {}, игра грузится дальше сама", *hold->initState);

    hold->inLoop.store(false);
}

std::unique_ptr<WorldHold> WorldHold::install(const EngineAddresses& addresses,
                                              std::string& error) {
    if (active_ != nullptr) {
        error = "удержание мира уже стоит";
        return nullptr;
    }

    auto state = std::make_unique<State>();

    state->initState = addresses.pointerTo<volatile std::int32_t*>("app_init_state");
    state->site = addresses.pointerTo<std::uint8_t*>("app_init_state_write");
    state->lookAlive = addresses.pointerTo<void (*)()>("look_alive");
    state->servicing = addresses.pointerTo<void (*)()>("critical_system_servicing");

    if (state->initState == nullptr || state->site == nullptr) {
        error = "адреса машины состояний запуска не разрешены";
        return nullptr;
    }

    // Без прокрутки кадра держать нельзя, и это не придирка. Пока поток игры у
    // нас, показывать и разбирать очередь окна больше некому: вышла бы не
    // придержанная игра, а повисшее окно, которое Windows объявит не отвечающим.
    if (state->lookAlive == nullptr || state->servicing == nullptr) {
        error = "прокрутка кадра игры не разрешена — держать мир было бы нечем";
        return nullptr;
    }

    // Сверка перед записью обязательна: сигнатура могла совпасть не там, где
    // нужно, оставаясь при этом формально однозначной, — а пять чужих байт,
    // записанных в середину чужой инструкции, уронят игру без объяснений.
    if (std::memcmp(state->site, kWriteOpcode, sizeof(kWriteOpcode)) != 0) {
        error = std::format("по адресу записи состояния лежит {:#04x} {:#04x}, а ожидалось "
                            "{:#04x} {:#04x} — сигнатура указывает не туда, правки не будет",
                            state->site[0], state->site[1], kWriteOpcode[0], kWriteOpcode[1]);
        return nullptr;
    }

    // Сверяется и само записываемое число: девятка — это «грузить мир», и она же
    // то, что мы обещаем написать вместо игры. Окажись там другое — мы подменили
    // бы не тот переход и обещали бы не то.
    std::int32_t written = 0;
    std::memcpy(&written, state->site + kWriteImmediateOffset, sizeof(written));

    if (written != kLoadWorldState) {
        error = std::format("инструкция пишет в состояние {}, а ожидалась {} — это не тот "
                            "переход, правки не будет",
                            written, kLoadWorldState);
        return nullptr;
    }

    std::memcpy(state->original, state->site, kWriteLength);

    state->thunk =
        allocateNear(reinterpret_cast<std::uintptr_t>(state->site), sizeof(kThunkTemplate));
    if (state->thunk == nullptr) {
        error = "рядом с игрой не нашлось памяти под переходник";
        return nullptr;
    }

    {
        std::uint8_t thunk[sizeof(kThunkTemplate)];
        std::memcpy(thunk, kThunkTemplate, sizeof(thunk));

        void (*const target)() = &gameThreadHold;
        std::memcpy(thunk + kThunkAddressOffset, &target, sizeof(target));

        std::memcpy(state->thunk, thunk, sizeof(thunk));
        ::FlushInstructionCache(::GetCurrentProcess(), state->thunk, sizeof(thunk));
    }

    const auto site = reinterpret_cast<std::uintptr_t>(state->site);
    const auto thunk = reinterpret_cast<std::uintptr_t>(state->thunk);

    std::uint8_t patch[kWriteLength];
    patch[0] = kCallOpcode;

    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int64_t>(thunk) - static_cast<std::int64_t>(site) - kCallLength);
    std::memcpy(patch + 1, &displacement, sizeof(displacement));

    // Хвост добивается пустышками: подменяемая инструкция длиннее вызова на пять
    // байт, и оставленный от неё огрызок игра исполнила бы как отдельную
    // инструкцию — а это уже случайный код.
    std::fill(patch + kCallLength, patch + kWriteLength, kNop);

    // Указатель ставится до записи вызова, а не после: с этого мгновения игра
    // вправе прийти к нам в любой миг, и прийти ей нужно к готовому.
    active_ = state.get();

    if (!writeCode(state->site, patch, sizeof(patch), error)) {
        active_ = nullptr;
        ::VirtualFree(state->thunk, 0, MEM_RELEASE);
        return nullptr;
    }

    spdlog::info("удержание мира поставлено: {:#x} зовёт нас через переходник {:#x}", site, thunk);

    auto hold = std::unique_ptr<WorldHold>{new WorldHold};
    hold->state_ = std::move(state);
    return hold;
}

WorldHold::~WorldHold() {
    release();

    // Ждём, пока поток игры выйдет из нашего цикла. Снять код из-под чужого
    // потока — значит отправить его исполнять освобождённую память.
    constexpr auto kLeaveTimeout = std::chrono::seconds{3};

    const auto deadline = std::chrono::steady_clock::now() + kLeaveTimeout;
    while (state_->inLoop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    if (state_->inLoop.load()) {
        // Возвращать байты в этом положении нельзя, и переходник тоже остаётся:
        // поток игры сейчас внутри него. Утечка в несколько десятков байт против
        // вылета — выбор не из трудных.
        spdlog::error("поток игры не вышел из удержания — код оставлен как есть");
        active_ = nullptr;
        return;
    }

    std::string error;
    if (!writeCode(state_->site, state_->original, kWriteLength, error)) {
        spdlog::error("не удалось вернуть запись состояния на место: {}", error);
    }

    active_ = nullptr;

    ::VirtualFree(state_->thunk, 0, MEM_RELEASE);
}

void WorldHold::release() noexcept {
    if (state_->releaseWanted.exchange(true)) {
        return;
    }

    spdlog::info("мир просят отпустить");

    if (!state_->visited.load()) {
        // Поток игры до нас ещё не дошёл. Само по себе это не беда: дойдя, он
        // увидит просьбу и пройдёт мимо, не задерживаясь. Бедой это станет, если
        // он не дойдёт вовсе — а такое бывает, когда клиент внедрился позже, чем
        // игра прошла это место.
        spdlog::info("поток игры до удержания ещё не доходил — отпустим на подходе");
    }
}

} // namespace oxymp::client::game
