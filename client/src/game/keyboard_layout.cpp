#include "keyboard_layout.hpp"

#include "code_patch.hpp"
#include "hook.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <format>
#include <atomic>
#include <intrin.h>
#include <span>

#include <windows.h>

#include <msctf.h>

namespace oxymp::client::game {
namespace {

/// Разметка того куска кода, который правится.
///
/// ```
///   +0   40 8A C6            mov  al, sil
///   +3   48 39 35 ? ? ? ?    cmp  [rip+X], rsi     раскладка потока — наша?
///   +10  75 08               jnz  +8               чужая — грузить
///   +12  84 C0               test al, al
///   +14  0F 84 ? ? ? ?       je   мимо загрузки
///   +20  33 D2 33 C9 ...     загрузка раскладки en-US
/// ```
///
/// Правка занимает двенадцать байт — сверку, оба перехода и первый байт `je`, —
/// а тринадцатым кладёт `E9` на место второго. Смещение перехода остаётся тем
/// же: `E9` со своими четырьмя байтами занимает ровно столько же, сколько
/// `0F 84` со своими, считая от следующей инструкции, и уводит туда же, куда
/// уводил `je`. То есть игре не придумывается нового поведения — у неё остаётся
/// одна из двух её же ветвей, та, которой она и так идёт, когда раскладка её
/// устраивает.
constexpr std::ptrdiff_t kPatchAt = 3;

/// Байты, по которым сверяется место перед записью.
///
/// Проверяются только неподвижные: у сверки и перехода есть свои смещения, и они
/// у каждой сборки свои.
constexpr std::array<std::uint8_t, 3> kMoveBytes{0x40, 0x8A, 0xC6};
constexpr std::array<std::uint8_t, 3> kCompareBytes{0x48, 0x39, 0x35};
constexpr std::array<std::uint8_t, 4> kBranchBytes{0x75, 0x08, 0x84, 0xC0};
constexpr std::array<std::uint8_t, 2> kJumpIfEqual{0x0F, 0x84};

/// Ничего не делающая инструкция и опкод безусловного перехода.
constexpr std::uint8_t kNoOperation = 0x90;
constexpr std::uint8_t kJumpAlways = 0xE9;

/// Записывается одним куском, а не по частям, и это не мелочь: между двумя
/// записями игра вправе пройти по этому месту, а половина правки — это чужая
/// инструкция, собранная из старых байт и новых.
constexpr std::array<std::uint8_t, 13> kPatch{
    kNoOperation, kNoOperation, kNoOperation, kNoOperation, kNoOperation,
    kNoOperation, kNoOperation, kNoOperation, kNoOperation, kNoOperation,
    kNoOperation, kNoOperation, kJumpAlways,
};

[[nodiscard]] bool matches(const std::uint8_t* where, std::span<const std::uint8_t> expected) {
    for (std::size_t at = 0; at < expected.size(); ++at) {
        if (where[at] != expected[at]) {
            return false;
        }
    }

    return true;
}

} // namespace

bool unlockKeyboardLayout(const EngineAddresses& addresses, std::string& error) {
    auto* const code = addresses.pointerTo<std::uint8_t*>("keyboard_layout_lock");
    if (code == nullptr) {
        error = "адрес навязанной раскладки не разрешён";
        return false;
    }

    // Сверка перед записью обязательна. Сигнатура могла совпасть не там, где
    // нужно, оставаясь при этом формально однозначной, — а тринадцать чужих байт
    // посреди чужой функции уронят игру без всяких объяснений.
    if (!matches(code, kMoveBytes) || !matches(code + kPatchAt, kCompareBytes) ||
        !matches(code + 10, kBranchBytes) || !matches(code + 14, kJumpIfEqual)) {
        error = std::format("по адресу раскладки лежит не то, что ожидалось ({:#04x} {:#04x} "
                            "{:#04x}) — сигнатура указывает не туда, правки не будет",
                            code[0], code[1], code[2]);
        return false;
    }

    if (!code::write(code + kPatchAt, kPatch.data(), kPatch.size(), error)) {
        return false;
    }

    spdlog::info("раскладка отпущена: {:#x} больше не навязывает en-US",
                 reinterpret_cast<std::uintptr_t>(code));

    return true;
}


namespace {

/// Сколько языков ввода разрешено держать в виду.
///
/// Больше десятка их не бывает даже у переводчиков, а предел нужен: список
/// спрашивается в готовый массив.
constexpr int kMaxLayouts = 16;

/// Язык, следующий за нынешним по тому же кругу, по которому его водит Windows.
///
/// Ноль означает «переключать не на что»: язык ввода один.
[[nodiscard]] LANGID nextLanguage() {
    std::array<HKL, kMaxLayouts> layouts{};

    const int count = ::GetKeyboardLayoutList(kMaxLayouts, layouts.data());
    if (count <= 1) {
        return 0;
    }

    // Младшее слово раскладки — это и есть язык; старшее говорит, какой именно
    // раскладкой этого языка набирают.
    const auto current =
        static_cast<LANGID>(reinterpret_cast<std::uintptr_t>(::GetKeyboardLayout(0)) & 0xFFFFU);

    for (int i = 0; i < count; ++i) {
        const auto language = static_cast<LANGID>(
            reinterpret_cast<std::uintptr_t>(layouts[static_cast<std::size_t>(i)]) & 0xFFFFU);

        if (language == current) {
            return static_cast<LANGID>(
                reinterpret_cast<std::uintptr_t>(layouts[static_cast<std::size_t>((i + 1) % count)]) &
                0xFFFFU);
        }
    }

    return static_cast<LANGID>(reinterpret_cast<std::uintptr_t>(layouts[0]) & 0xFFFFU);
}

/// Сказали ли уже, чем кончилась просьба. Нажатий много, строка нужна одна на
/// каждый исход.
bool g_toldSwitched = false;
bool g_toldFailed = false;

} // namespace

void switchInputLanguage() {
    const LANGID next = nextLanguage();
    if (next == 0) {
        return;
    }

    const HKL before = ::GetKeyboardLayout(0);

    // Модель потока не навязывается: поток игры своё COM уже завёл, и второй
    // раз заводить его чужими руками — верный способ получить RPC_E_CHANGED_MODE
    // на ровном месте. Не завёл — значит не завёл, и просьба просто не пройдёт.
    ITfInputProcessorProfiles* profiles = nullptr;

    HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITfInputProcessorProfiles,
                                    reinterpret_cast<void**>(&profiles));

    if (hr == CO_E_NOTINITIALIZED) {
        // Поток без COM — заводим его сами и уже не отпускаем: отпустив, мы
        // закрыли бы COM тому, кто мог начать им пользоваться следом.
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITfInputProcessorProfiles,
                                reinterpret_cast<void**>(&profiles));
    }

    if (FAILED(hr) || profiles == nullptr) {
        if (!g_toldFailed) {
            g_toldFailed = true;
            spdlog::warn("служба текстового ввода не отозвалась ({:#x}) — язык не переключить",
                         static_cast<std::uint32_t>(hr));
        }
        return;
    }

    const HRESULT changed = profiles->ChangeCurrentLanguage(next);
    profiles->Release();

    if (FAILED(changed)) {
        if (!g_toldFailed) {
            g_toldFailed = true;
            spdlog::warn("служба текстового ввода отказала в смене языка на {:#06x}: {:#x}", next,
                         static_cast<std::uint32_t>(changed));
        }
        return;
    }

    // Замер до и после — не отладочный мусор: только он отличает «служба не
    // послушалась» от «послушалась, а раскладку потока кто-то вернул».
    spdlog::info("язык ввода: было {:#x}, просили {:#06x}, стало {:#x}",
                 reinterpret_cast<std::uintptr_t>(before), next,
                 reinterpret_cast<std::uintptr_t>(::GetKeyboardLayout(0)));
}


namespace {

using ActivateFunction = HKL(WINAPI*)(HKL, UINT);

/// Настоящая просьба Windows. Отдельно от состояния — чтобы обработчику всегда
/// было куда передать чужой вызов.
ActivateFunction g_activate = nullptr;

/// Где в памяти лежит сама игра.
///
/// По этим границам вызовы игры отличаются от вызовов Windows: адрес возврата
/// внутри — значит звала игра.
std::uintptr_t g_gameBegin = 0;
std::uintptr_t g_gameEnd = 0;

[[nodiscard]] bool insideGame(const void* address) {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return at >= g_gameBegin && at < g_gameEnd;
}

} // namespace

struct LayoutGuard::State {
    Hook activate;

    /// Сказали ли уже, что игре отказано: просьб у неё много, строка нужна одна.
    std::atomic<bool> told{false};

    static HKL WINAPI detour(HKL layout, UINT flags) {
        State* const state = active_;

        if (g_activate == nullptr) {
            return nullptr;
        }

        if (state == nullptr || !insideGame(_ReturnAddress())) {
            return g_activate(layout, flags);
        }

        if (!state->told.exchange(true)) {
            spdlog::info("игре отказано в возврате своей раскладки: она просила {:#x}",
                         reinterpret_cast<std::uintptr_t>(layout));
        }

        // Отказ выглядит как согласие: игре отвечается той раскладкой, что стоит
        // сейчас. Ответить нулём значило бы сказать «не вышло», а на это она
        // вправе повести себя как угодно.
        return ::GetKeyboardLayout(0);
    }
};

LayoutGuard::State* LayoutGuard::active_ = nullptr;

std::unique_ptr<LayoutGuard> LayoutGuard::install(std::string& error) {
    if (active_ != nullptr) {
        error = "раскладка уже удерживается";
        return nullptr;
    }

    const HMODULE game = ::GetModuleHandleW(nullptr);
    const HMODULE user = ::GetModuleHandleW(L"user32.dll");

    if (game == nullptr || user == nullptr) {
        error = "не найден образ игры или user32 — перехватывать нечего";
        return nullptr;
    }

    // Границы образа берутся из его же заголовков: другого источника размера у
    // загруженного модуля нет.
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto* const headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const std::uint8_t*>(game) + dos->e_lfanew);

    g_gameBegin = reinterpret_cast<std::uintptr_t>(game);
    g_gameEnd = g_gameBegin + headers->OptionalHeader.SizeOfImage;

    void* const target = reinterpret_cast<void*>(::GetProcAddress(user, "ActivateKeyboardLayout"));
    if (target == nullptr) {
        error = "ActivateKeyboardLayout не найдена";
        return nullptr;
    }

    auto state = std::make_unique<State>();

    // Указатель ставится раньше перехвата: подменённая функция вправе быть
    // вызвана в тот же миг.
    active_ = state.get();

    if (!state->activate.install(target, reinterpret_cast<void*>(&State::detour), error)) {
        active_ = nullptr;
        return nullptr;
    }

    g_activate = state->activate.original<ActivateFunction>();

    spdlog::info("раскладка удерживается: игре её больше не вернуть, {:#x}—{:#x}", g_gameBegin,
                 g_gameEnd);

    auto guard = std::unique_ptr<LayoutGuard>{new LayoutGuard};
    guard->state_ = std::move(state);
    return guard;
}

LayoutGuard::~LayoutGuard() {
    // Порядок обязателен: пока перехват стоит, обработчик обязан оставаться
    // рабочим, поэтому сперва снятие и только потом забывание состояния.
    if (state_ != nullptr) {
        state_->activate.remove();
    }

    active_ = nullptr;
}

} // namespace oxymp::client::game
