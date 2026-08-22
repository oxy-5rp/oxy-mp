#include "raw_input.hpp"

#include "hook.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <vector>

#include <windows.h>

namespace oxymp::client::game {
namespace {

using RegisterFunction = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);

/// Настоящая просьба Windows, сохранённая отдельно от состояния перехвата.
///
/// Отдельно — чтобы обработчику всегда было куда передать просьбу игры. Иначе
/// в тот единственный миг, когда состояние уже снято, а перехват ещё стоит,
/// нам осталось бы только отказать — и игра осталась бы вовсе без клавиатуры
/// из-за нашей уборки.
RegisterFunction g_original = nullptr;

} // namespace

struct RawInput::State {
    Hook hook;

    /// Сказали ли уже, что признак снят. Просьб бывает несколько, а строка в
    /// журнале нужна одна.
    std::atomic<bool> told{false};

    static BOOL WINAPI reg(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
        State* const state = active_;

        // Исходная берётся у перехвата, если её ещё не успели запомнить:
        // перехват начинает работать в тот же миг, как встал, а запоминаем мы
        // строкой позже, и в эту щель игра вправе попроситься.
        RegisterFunction original = g_original;
        if (original == nullptr && state != nullptr) {
            original = state->hook.original<RegisterFunction>();
        }

        if (original == nullptr) {
            // Отказывать игре в клавиатуре из-за нашей неготовности нельзя:
            // честнее сказать «получилось» и не мешать.
            return TRUE;
        }

        if (state == nullptr || devices == nullptr || count == 0 ||
            size != sizeof(RAWINPUTDEVICE)) {
            // Не наш случай — пропускаем как есть. Чужой размер записи означает,
            // что Windows однажды сменила устройство этой просьбы, и трогать её
            // вслепую нельзя.
            return original(devices, count, size);
        }

        // Список копируется: нам его отдали на чтение, и править чужую память
        // мы не вправе — она может лежать хоть в постоянных данных игры.
        std::vector<RAWINPUTDEVICE> copy{devices, devices + count};

        bool stripped = false;
        for (RAWINPUTDEVICE& device : copy) {
            // Каждая просьба записывается целиком, и это не лишнее: разбирать
            // «раскладка всё равно не переключается» иначе не на чем. Виден и
            // набор устройств, и признаки, и окно, которому игра просит слать
            // ввод. Отладочной записью, а не обычной: просьбу игра повторяет на
            // каждый возврат фокуса.
            spdlog::debug("сырой ввод: игра просит страницу {:#06x} назначение {:#06x} "
                          "признаки {:#010x} окно {:#x}",
                          device.usUsagePage, device.usUsage, device.dwFlags,
                          reinterpret_cast<std::uintptr_t>(device.hwndTarget));

            if ((device.dwFlags & RIDEV_NOHOTKEYS) != 0) {
                device.dwFlags &= ~static_cast<DWORD>(RIDEV_NOHOTKEYS);
                stripped = true;
            }
        }

        if (stripped && !state->told.exchange(true)) {
            spdlog::debug("сырой ввод: у игры отобран отказ от системных сочетаний — "
                         "смена раскладки снова работает");
        }

        const BOOL answer = original(copy.data(), count, size);

        if (answer == FALSE) {
            spdlog::warn("raw input: Windows refused the request, error {}", ::GetLastError());
        }

        return answer;
    }
};

RawInput::State* RawInput::active_ = nullptr;

std::unique_ptr<RawInput> RawInput::install(std::string& error) {
    if (active_ != nullptr) {
        error = "сырой ввод уже перехвачен";
        return nullptr;
    }

    const HMODULE user = ::GetModuleHandleW(L"user32.dll");
    if (user == nullptr) {
        error = "user32 не найдена — перехватывать нечего";
        return nullptr;
    }

    void* const target = reinterpret_cast<void*>(::GetProcAddress(user, "RegisterRawInputDevices"));
    if (target == nullptr) {
        error = "RegisterRawInputDevices не найдена";
        return nullptr;
    }

    auto state = std::make_unique<State>();

    // Указатель ставится раньше перехвата: подменённая функция вправе быть
    // вызвана в тот же миг.
    active_ = state.get();

    if (!state->hook.install(target, reinterpret_cast<void*>(&State::reg), error)) {
        active_ = nullptr;
        return nullptr;
    }

    // Запоминается сразу за установкой и до первого вызова: обработчику без неё
    // некуда передать просьбу игры.
    g_original = state->hook.original<RegisterFunction>();

    spdlog::debug("перехват сырого ввода поставлен");

    auto input = std::unique_ptr<RawInput>{new RawInput};
    input->state_ = std::move(state);
    return input;
}

RawInput::~RawInput() {
    // Порядок обязателен: пока перехват стоит, обработчик обязан оставаться
    // рабочим, поэтому сперва снятие и только потом забывание состояния.
    if (state_ != nullptr) {
        state_->hook.remove();
    }

    active_ = nullptr;
}

} // namespace oxymp::client::game
