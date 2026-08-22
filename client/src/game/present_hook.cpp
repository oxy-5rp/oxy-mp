#include "present_hook.hpp"

#include "hook.hpp"

#include <spdlog/spdlog.h>

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

namespace oxymp::client::game {
namespace {

/// Места нужных методов в таблице интерфейса swapchain.
///
/// Таблица у IDXGISwapChain одна на все реализации, и порядок в ней задан
/// заголовками Windows: сперва три метода IUnknown, затем четыре IDXGIObject,
/// затем один IDXGIDeviceSubObject, и только после них — свои.
constexpr std::size_t kPresentSlot = 8;
constexpr std::size_t kResizeBuffersSlot = 13;

/// Место Present1 в таблице более поздней разновидности swapchain.
///
/// Показать кадр можно двумя способами, и какой выберет игра, снаружи не видно:
/// Present достался DirectX 11 с самого начала, Present1 появился вместе с
/// IDXGISwapChain1. Перехватываются оба — не угадав, мы получили бы не ошибку, а
/// молчание: интерфейс просто не появился бы, и искать причину было бы негде.
constexpr std::size_t kPresent1Slot = 22;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT,
                                                    UINT);

/// Перехват один на процесс: подменённые методы — простые функции без
/// состояния, и связать их с объектом иначе нельзя.
struct State {
    Hook present;
    Hook present1;
    Hook resizeBuffers;

    PresentHook::Callback onPresent;
    PresentHook::ResizeCallback onResize;
};

State* g_state = nullptr;

/// Идёт ли уже наша отрисовка в этом потоке.
///
/// Present1 у DXGI внутри вправе позвать Present, и тогда без этого признака мы
/// нарисовали бы интерфейс дважды за кадр: поверх самого себя, с удвоенным
/// наложением по краям.
thread_local bool g_drawing = false;

void drawOnce(State& state, IDXGISwapChain* swapchain) {
    if (g_drawing || !state.onPresent) {
        return;
    }

    g_drawing = true;

    // Ошибка в нашем рисовании не должна уносить с собой игру: кадр без
    // интерфейса — неприятность, вылет посреди боя — беда.
    try {
        state.onPresent(swapchain);
    } catch (...) {
        spdlog::error("drawing the interface into the frame failed");
    }

    g_drawing = false;
}

HRESULT STDMETHODCALLTYPE presentDetour(IDXGISwapChain* swapchain, UINT interval, UINT flags) {
    State* const state = g_state;
    if (state == nullptr) {
        // Сюда попасть нельзя: состояние заводится раньше подмены и живёт до
        // конца процесса. Проверка стоит на случай, если однажды это перестанет
        // быть правдой, — вызов по нулевому указателю дороже одной ветки.
        return S_OK;
    }

    drawOnce(*state, swapchain);

    return state->present.original<PresentFn>()(swapchain, interval, flags);
}

HRESULT STDMETHODCALLTYPE present1Detour(IDXGISwapChain* swapchain, UINT interval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    State* const state = g_state;
    if (state == nullptr) {
        return S_OK;
    }

    drawOnce(*state, swapchain);

    return state->present1.original<Present1Fn>()(swapchain, interval, flags, parameters);
}

HRESULT STDMETHODCALLTYPE resizeBuffersDetour(IDXGISwapChain* swapchain, UINT count, UINT width,
                                              UINT height, DXGI_FORMAT format, UINT flags) {
    State* const state = g_state;
    if (state == nullptr) {
        return S_OK;
    }

    // До вызова исходной, а не после: пересоздание не удастся, пока хоть кто-то
    // держит ссылку на прежние буферы кадра.
    if (state->onResize) {
        try {
            state->onResize();
        } catch (...) {
            spdlog::error("releasing frame resources failed");
        }
    }

    return state->resizeBuffers.original<ResizeBuffersFn>()(swapchain, count, width, height,
                                                            format, flags);
}

/// Добывает таблицу методов swapchain, заведя свой одноразовый.
///
/// Ни устройства игры, ни её swapchain у нас нет, а таблица нужна. Она, однако,
/// общая у всех реализаций интерфейса — значит годится любой swapchain, в том
/// числе созданный нами для невидимого окна размером в один пиксель.
/// Таблицы методов обеих разновидностей swapchain.
struct VTables {
    void** basic = nullptr;
    void** extended = nullptr;
};

VTables captureSwapchainVTables(std::string& error) {
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &::DefWindowProcW;
    description.hInstance = ::GetModuleHandleW(nullptr);
    description.lpszClassName = L"oxyMPDeviceProbe";

    const ATOM registered = ::RegisterClassExW(&description);
    if (registered == 0) {
        error = "не удалось завести окно для опроса графики";
        return {};
    }

    HWND window = ::CreateWindowExW(0, description.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1,
                                    1, nullptr, nullptr, description.hInstance, nullptr);
    if (window == nullptr) {
        ::UnregisterClassW(description.lpszClassName, description.hInstance);
        error = "не удалось создать окно для опроса графики";
        return {};
    }

    DXGI_SWAP_CHAIN_DESC chain{};
    chain.BufferCount = 1;
    chain.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain.OutputWindow = window;
    chain.SampleDesc.Count = 1;
    chain.Windowed = TRUE;
    chain.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};

    const HRESULT created = ::D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted,
        static_cast<UINT>(std::size(wanted)), D3D11_SDK_VERSION, &chain, &swapchain, &device,
        &level, &context);

    VTables tables;

    if (SUCCEEDED(created) && swapchain != nullptr) {
        tables.basic = *reinterpret_cast<void***>(swapchain);

        // Более поздняя разновидность спрашивается отдельно: её может не
        // оказаться на старой Windows, и это не беда — тогда игра показывает
        // кадр только первым способом.
        IDXGISwapChain1* extended = nullptr;
        if (SUCCEEDED(swapchain->QueryInterface(__uuidof(IDXGISwapChain1),
                                                reinterpret_cast<void**>(&extended)))) {
            tables.extended = *reinterpret_cast<void***>(extended);
            extended->Release();
        }
    } else {
        error = "не удалось создать пробное устройство графики";
    }

    if (context != nullptr) {
        context->Release();
    }
    if (device != nullptr) {
        device->Release();
    }
    if (swapchain != nullptr) {
        swapchain->Release();
    }

    ::DestroyWindow(window);
    ::UnregisterClassW(description.lpszClassName, description.hInstance);

    return tables;
}

} // namespace

std::unique_ptr<PresentHook> PresentHook::install(Callback onPresent, ResizeCallback onResize,
                                                  std::string& error) {
    if (g_state != nullptr) {
        error = "перехват показа кадра уже стоит";
        return nullptr;
    }

    const VTables tables = captureSwapchainVTables(error);
    if (tables.basic == nullptr) {
        return nullptr;
    }

    std::unique_ptr<PresentHook> hook{new PresentHook};
    auto state = std::make_unique<State>();

    state->onPresent = std::move(onPresent);
    state->onResize = std::move(onResize);

    // Порядок обязателен: подменённый метод вправе быть вызван сразу же,
    // поэтому состояние должно быть видно до подмены.
    g_state = state.get();

    if (!state->present.install(tables.basic[kPresentSlot],
                                reinterpret_cast<void*>(&presentDetour), error)) {
        g_state = nullptr;
        return nullptr;
    }

    if (tables.extended != nullptr) {
        std::string present1Error;

        if (!state->present1.install(tables.extended[kPresent1Slot],
                                     reinterpret_cast<void*>(&present1Detour), present1Error)) {
            spdlog::warn("the second present hook was not installed: {}", present1Error);
        }
    }

    std::string resizeError;
    if (!state->resizeBuffers.install(tables.basic[kResizeBuffersSlot],
                                      reinterpret_cast<void*>(&resizeBuffersDetour),
                                      resizeError)) {
        // Не беда, из-за которой стоит отказываться от интерфейса: без этого
        // перехвата смена разрешения обойдётся пересозданием наших ресурсов на
        // следующем кадре, а не заранее.
        spdlog::warn("the buffer resize hook was not installed: {}", resizeError);
    }

    // Состояние переживает объект намеренно: перехват снимается в разрушителе,
    // но подменённый метод может быть в этот миг на середине работы, и
    // освобождать его состояние немедленно нельзя.
    state.release();

    spdlog::debug("перехват показа кадра поставлен");
    return hook;
}

PresentHook::~PresentHook() {
    if (g_state == nullptr || !g_state->onPresent) {
        return;
    }

    // Обработчики обнуляются, а сам перехват остаётся стоять — и это не
    // недоделка, а единственный безопасный порядок.
    //
    // Поток отрисовки игры может находиться внутри подменённого метода прямо
    // сейчас, и ему ещё предстоит вызвать исходный. Сними мы перехват здесь —
    // адрес исходного метода обнулился бы у него под ногами. Обнулённый же
    // обработчик означает всего лишь кадр без интерфейса, а этих кадров
    // остаётся считаное число: перехваты снимаются следом, все разом, вместе с
    // самим механизмом перехвата — тот умеет останавливать потоки и переносить
    // их из подменённого кода.
    g_state->onPresent = nullptr;
    g_state->onResize = nullptr;

    spdlog::debug("рисование интерфейса в кадр прекращено");
}

} // namespace oxymp::client::game
