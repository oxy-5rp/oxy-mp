#include <oxymp/webui/browser.hpp>

// Порядок обязателен: заголовок WebView2 написан на макросах COM — interface,
// STDMETHODCALLTYPE и прочих, — которых без objbase.h не существует. Собранный
// раньше него, он рассыпается сотней синтаксических ошибок, ни одна из которых
// не называет настоящую причину.
#include <windows.h>

#include <objbase.h>
#include <unknwn.h>

#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <spdlog/spdlog.h>

#include <string>

namespace oxymp::webui {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

/// Переводит UTF-8 в UTF-16: движок принимает только широкие строки.
std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          size);

    return wide;
}

std::string narrow(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }

    // Без завершающего нуля: он нужен строке C, а не std::string.
    std::string narrowed(static_cast<std::size_t>(size) - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, narrowed.data(), size, nullptr, nullptr);

    return narrowed;
}

} // namespace

/// Всё, что связывает нас с движком.
///
/// Вынесено из заголовка целиком: заголовки WebView2 тянут за собой COM и WRL, и
/// всё, что подключит наш заголовок, получило бы их следом.
struct Browser::State {
    HWND window = nullptr;

    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;

    MessageHandler handler;

    /// Что показать, когда движок поднимется.
    ///
    /// Страницу просят показать раньше, чем движок готов: он поднимается сам,
    /// своим чередом, и ждать этого — значит держать окно пустым. Просьба
    /// запоминается и выполняется, как только появится чем.
    std::string pending;

    /// Просвечивает ли страница там, где ничего не нарисовано.
    bool transparent = false;
};

Browser::Browser() : state_(std::make_unique<State>()) {}

Browser::~Browser() {
    if (state_->controller) {
        state_->controller->Close();
    }
}

bool Browser::ready() const noexcept {
    return state_->view != nullptr;
}

std::unique_ptr<Browser> Browser::create(HWND window, const std::wstring& userDataDirectory,
                                         bool transparent, std::string& error) {
    // Движок построен на COM, и поток, из которого его заводят, обязан быть в
    // однопоточном подразделении. Без этого создание либо отказывает, либо —
    // хуже — проходит, а страница остаётся пустой.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        error = "не удалось подготовить COM для движка интерфейса";
        return nullptr;
    }

    std::unique_ptr<Browser> browser{new Browser};
    browser->state_->window = window;
    browser->state_->transparent = transparent;

    State* const state = browser->state_.get();

    // Chromium сам решает, видно ли его окно, и перестаёт рисовать то, которое
    // считает закрытым другим. Для окна, лежащего поверх игры и намеренно не
    // забирающего у неё ввод, этот расчёт всегда даёт «не видно» — и страница
    // становится чёрным прямоугольником. Расчёт приходится выключать.
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-features=CalculateNativeWinOcclusion");

    const HRESULT started = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataDirectory.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [state](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || environment == nullptr) {
                    spdlog::error("the interface engine did not start: {:#010x}",
                                  static_cast<std::uint32_t>(result));
                    return result;
                }

                return environment->CreateCoreWebView2Controller(
                    state->window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [state](HRESULT created,
                                ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(created) || controller == nullptr) {
                                spdlog::error("the page was not created: {:#010x}",
                                              static_cast<std::uint32_t>(created));
                                return created;
                            }

                            state->controller = controller;
                            controller->get_CoreWebView2(&state->view);

                            if (state->view == nullptr) {
                                return E_FAIL;
                            }

                            // Прозрачный фон просят через более поздний
                            // интерфейс, которого может и не оказаться: он
                            // появился не в первой версии движка. Отказ здесь
                            // не беда — страница просто будет с фоном.
                            if (state->transparent) {
                                ComPtr<ICoreWebView2Controller2> shading;
                                if (SUCCEEDED(controller->QueryInterface(
                                        IID_PPV_ARGS(&shading)))) {
                                    shading->put_DefaultBackgroundColor(
                                        COREWEBVIEW2_COLOR{0, 0, 0, 0});
                                } else {
                                    spdlog::warn("the interface engine cannot do a transparent background");
                                }
                            }

                            // Страница наша, и предлагать на ней меню браузера,
                            // отладчик и горячие клавиши незачем: это интерфейс
                            // игры, а не окно браузера.
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(state->view->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            state->view->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [state](ICoreWebView2*,
                                            ICoreWebView2WebMessageReceivedEventArgs* args)
                                        -> HRESULT {
                                        if (!state->handler) {
                                            return S_OK;
                                        }

                                        LPWSTR json = nullptr;
                                        if (FAILED(args->get_WebMessageAsJson(&json))) {
                                            return S_OK;
                                        }

                                        state->handler(narrow(json));
                                        ::CoTaskMemFree(json);

                                        return S_OK;
                                    })
                                    .Get(),
                                nullptr);

                            RECT bounds{};
                            ::GetClientRect(state->window, &bounds);
                            controller->put_Bounds(bounds);

                            spdlog::debug("движок интерфейса готов, окно {}x{}",
                                         bounds.right - bounds.left, bounds.bottom - bounds.top);

                            if (!state->pending.empty()) {
                                const std::wstring wide = widen(state->pending);
                                const HRESULT shown = state->view->NavigateToString(wide.c_str());

                                spdlog::debug("страница показана: {} символов, код {:#010x}",
                                             wide.size(), static_cast<std::uint32_t>(shown));

                                state->pending.clear();
                            }

                            return S_OK;
                        })
                        .Get());
            })
            .Get());

    if (FAILED(started)) {
        // Отдельным сообщением, потому что причина почти всегда одна и та же, и
        // называть её прямо дешевле, чем оставлять код ошибки.
        error = started == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                    ? "не найден WebView2 Runtime — установите его с сайта Microsoft"
                    : "не удалось запустить движок интерфейса";
        return nullptr;
    }

    return browser;
}

void Browser::show(std::string_view html) {
    if (state_->view == nullptr) {
        state_->pending.assign(html);
        return;
    }

    state_->view->NavigateToString(widen(html).c_str());
}

void Browser::post(std::string_view json) {
    if (state_->view == nullptr) {
        return;
    }

    state_->view->PostWebMessageAsJson(widen(json).c_str());
}

void Browser::resize(int width, int height) {
    if (!state_->controller) {
        return;
    }

    const RECT bounds{0, 0, width, height};
    state_->controller->put_Bounds(bounds);
}

void Browser::onMessage(MessageHandler handler) {
    state_->handler = std::move(handler);
}

} // namespace oxymp::webui
