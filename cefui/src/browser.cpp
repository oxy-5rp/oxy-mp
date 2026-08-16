#include <oxymp/cefui/browser.hpp>

#include "page_message.hpp"

#include <include/cef_client.h>
#include <include/cef_parser.h>
#include <include/wrapper/cef_helpers.h>

#include <spdlog/spdlog.h>

#include <filesystem>
#include <format>
#include <mutex>

namespace oxymp::cefui {
namespace {

/// Заготовка страницы, доставляемой строкой.
///
/// CEF умеет открыть страницу только по ссылке, и передать ей разметку прямо в
/// вызове нельзя. Обходится это ссылкой с данными: разметка кодируется в неё
/// целиком. Файла на диске при этом не заводится — ровно то же, чем обходился
/// прежний движок.
constexpr const char* kDataPrefix = "data:text/html;charset=utf-8;base64,";

/// Как Chromium называет эту кнопку в событии нажатия.
cef_mouse_button_type_t buttonType(Browser::MouseButton button) {
    switch (button) {
    case Browser::MouseButton::Middle:
        return MBT_MIDDLE;
    case Browser::MouseButton::Right:
        return MBT_RIGHT;
    case Browser::MouseButton::Left:
        break;
    }

    return MBT_LEFT;
}

/// Признак «эта кнопка сейчас нажата», каким его ждёт Chromium.
///
/// Нужен в каждом событии, а не только в нажатии: движение с нажатой кнопкой —
/// это перетаскивание, и отличить его от простого движения странице больше не по
/// чему.
std::uint32_t buttonFlag(Browser::MouseButton button) {
    switch (button) {
    case Browser::MouseButton::Middle:
        return EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    case Browser::MouseButton::Right:
        return EVENTFLAG_RIGHT_MOUSE_BUTTON;
    case Browser::MouseButton::Left:
        break;
    }

    return EVENTFLAG_LEFT_MOUSE_BUTTON;
}

} // namespace

/// Всё, что связывает нас с CEF.
///
/// Вынесено из заголовка целиком: заголовки CEF тянут за собой половину
/// Chromium, и всё, что подключит наш заголовок, получило бы их следом.
///
/// Живёт под общим владением и переживает Browser намеренно: CEF отпускает
/// браузер не сразу, и его обработчик вправе прийти уже после того, как
/// владелец с ним попрощался.
struct Browser::State : public CefClient,
                        public CefRenderHandler,
                        public CefLifeSpanHandler,
                        public CefDisplayHandler,
                        public CefLoadHandler {
    PaintHandler onPaint;
    MessageHandler onMessage;
    EventHandler onEvent;

    std::mutex mutex;
    CefRefPtr<CefBrowser> browser;

    int width = 0;
    int height = 0;

    /// Что показать, когда браузер поднимется.
    ///
    /// Страницу просят показать раньше, чем он готов: поднимается он своим
    /// чередом, и ждать этого — значит держать кадр пустым.
    std::string pending;

    /// Ссылка вместо разметки, если показать просили её. Хранится отдельно, а не
    /// в pending, чтобы не гадать потом, что за строка там лежит.
    std::string pendingUrl;

    /// Какие кнопки мыши сейчас держат нажатыми.
    std::uint32_t held = 0;

    /// Браузер, если он уже есть. Спрашивается отовсюду: он заводится позже
    /// объекта и уходит раньше него.
    CefRefPtr<CefBrowser> current() {
        const std::lock_guard guard{mutex};
        return browser;
    }

    /// Событие мыши в точке с учётом того, что сейчас зажато.
    CefMouseEvent mouseAt(int x, int y) {
        CefMouseEvent event;
        event.x = x;
        event.y = y;

        {
            const std::lock_guard guard{mutex};
            event.modifiers = held;
        }

        return event;
    }

    // --- CefClient ---------------------------------------------------------

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                  CefProcessId, CefRefPtr<CefProcessMessage> message) override {
        const CefRefPtr<CefListValue> arguments = message->GetArgumentList();

        if (message->GetName() == kAltEvent) {
            // Событие разбирается, даже когда слушать его некому: вернуть false
            // значило бы сказать CEF, что сообщение не наше, а оно наше.
            if (onEvent && arguments->GetSize() >= 2) {
                onEvent(arguments->GetString(0).ToString(), arguments->GetString(1).ToString());
            }

            return true;
        }

        if (message->GetName() != kMessageFromPage || !onMessage) {
            return false;
        }

        if (arguments->GetSize() > 0) {
            onMessage(arguments->GetString(0).ToString());
        }

        return true;
    }

    // --- CefRenderHandler --------------------------------------------------

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        const std::lock_guard guard{mutex};

        rect.x = 0;
        rect.y = 0;

        // Ноль здесь означал бы отказ CEF рисовать вовсе, поэтому размер всегда
        // хотя бы в один пиксель.
        rect.width = width > 0 ? width : 1;
        rect.height = height > 0 ? height : 1;
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                 const void* buffer, int paintWidth, int paintHeight) override {
        // Всплывающие списки приходят отдельным слоем и своим прямоугольником.
        // Страница интерфейса их не заводит, и складывать два слоя в один было
        // бы работой впустую.
        if (type != PET_VIEW || !onPaint) {
            return;
        }

        onPaint(static_cast<const std::uint8_t*>(buffer), paintWidth, paintHeight);
    }

    // --- CefLifeSpanHandler ------------------------------------------------

    void OnAfterCreated(CefRefPtr<CefBrowser> created) override {
        std::string html;
        std::string url;

        {
            const std::lock_guard guard{mutex};
            browser = created;
            html = std::exchange(pending, {});
            url = std::exchange(pendingUrl, {});
        }

        if (!url.empty()) {
            loadUrl(url);
        } else if (!html.empty()) {
            load(html);
        }
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        const std::lock_guard guard{mutex};
        browser = nullptr;
    }

    // --- CefDisplayHandler -------------------------------------------------

    /// Пересылает в наш журнал то, что страница пишет себе в консоль.
    ///
    /// Без этого страница жалуется в пустоту. Своих средств разработчика у неё
    /// нет — окна нет вовсе, — и ошибка в разметке выглядит снаружи как «просто
    /// не так нарисовалось», без единой подсказки о причине. У alt:V ровно то
    /// же: строки `[WEB, View ...]` в их журнале — это консоль их страницы.
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                          const CefString& message, const CefString& source,
                          int line) override {
        const std::string where =
            source.empty() ? std::string{"страница"}
                           : std::format("{}:{}", std::filesystem::path{source.ToString()}
                                                      .filename()
                                                      .string(),
                                         line);

        switch (level) {
        case LOGSEVERITY_ERROR:
        case LOGSEVERITY_FATAL:
            spdlog::error("[страница] {} — {}", message.ToString(), where);
            break;
        case LOGSEVERITY_WARNING:
            spdlog::warn("[страница] {} — {}", message.ToString(), where);
            break;
        default:
            spdlog::info("[страница] {} — {}", message.ToString(), where);
            break;
        }

        // false: пусть Chromium запишет её и у себя. Своя запись у него уходит в
        // logs/cef.log, и терять её незачем — там же лежит всё, о чём он говорит
        // помимо страницы.
        return false;
    }

    // --- CefLoadHandler ----------------------------------------------------

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode code,
                     const CefString& text, const CefString& url) override {
        spdlog::error("страница интерфейса не загрузилась: {} ({}), {}", text.ToString(),
                      static_cast<int>(code), url.ToString().substr(0, 64));
    }

    /// Открывает разметку как ссылку с данными.
    void load(const std::string& html) {
        loadUrl(kDataPrefix + CefBase64Encode(html.data(), html.size()).ToString());
    }

    void loadUrl(const std::string& url) {
        CefRefPtr<CefBrowser> target;

        {
            const std::lock_guard guard{mutex};
            target = browser;
        }

        if (target != nullptr) {
            target->GetMainFrame()->LoadURL(url);
        }
    }

    IMPLEMENT_REFCOUNTING(State);
};

Browser::Browser() = default;

Browser::~Browser() {
    if (state_ == nullptr) {
        return;
    }

    CefRefPtr<CefBrowser> browser;

    {
        const std::lock_guard guard{state_->mutex};

        // Обработчики снимаются первыми: пришедший после этого кадр не найдёт
        // владельца и тихо уйдёт в никуда, вместо того чтобы попасть в
        // разрушаемый объект.
        state_->onPaint = nullptr;
        state_->onMessage = nullptr;
        state_->onEvent = nullptr;

        browser = state_->browser;
    }

    if (browser != nullptr) {
        // Принудительно: страница вправе не согласиться на закрытие, а нам не с
        // кем это обсуждать — модуль выгружается.
        browser->GetHost()->CloseBrowser(true);
    }
}

std::unique_ptr<Browser> Browser::create(int width, int height, PaintHandler onPaint,
                                         std::string& error) {
    std::unique_ptr<Browser> browser{new Browser};

    browser->state_ = std::shared_ptr<State>{new State{}, [](State* state) { state->Release(); }};
    browser->state_->AddRef();

    browser->state_->onPaint = std::move(onPaint);
    browser->state_->width = width;
    browser->state_->height = height;

    CefWindowInfo window;

    // Родителя нет и быть не должно: страница рисуется в память, окна у неё нет
    // вовсе. Именно это и отличает её от прежнего движка, которому окно было
    // нужно всегда.
    window.SetAsWindowless(nullptr);

    CefBrowserSettings settings;
    settings.windowless_frame_rate = 20;

    // Фон прозрачный. Настоящий, в четвёртом канале, а не порогом яркости: там,
    // где страница ничего не нарисовала, сквозь неё видно игру.
    settings.background_color = 0;

    if (!CefBrowserHost::CreateBrowser(window, browser->state_.get(), CefString{"about:blank"},
                                       settings, nullptr, nullptr)) {
        error = "не удалось создать страницу CEF";
        return nullptr;
    }

    return browser;
}

void Browser::show(std::string_view html) {
    if (state_ == nullptr) {
        return;
    }

    bool ready = false;

    {
        const std::lock_guard guard{state_->mutex};

        ready = state_->browser != nullptr;
        if (!ready) {
            state_->pending.assign(html);
        }
    }

    if (ready) {
        state_->load(std::string{html});
    }
}

void Browser::open(std::string_view url) {
    if (state_ == nullptr) {
        return;
    }

    bool ready = false;

    {
        const std::lock_guard guard{state_->mutex};

        ready = state_->browser != nullptr;
        if (!ready) {
            state_->pendingUrl.assign(url);

            // Разметка, если её успели попросить, больше не нужна: показать
            // можно что-то одно, и последнее слово за тем, кто сказал позже.
            state_->pending.clear();
        }
    }

    if (ready) {
        state_->loadUrl(std::string{url});
    }
}

void Browser::emit(std::string_view name, std::string_view arguments) {
    if (state_ == nullptr || name.empty()) {
        return;
    }

    CefRefPtr<CefBrowser> browser;

    {
        const std::lock_guard guard{state_->mutex};
        browser = state_->browser;
    }

    if (browser == nullptr) {
        return;
    }

    const CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kAltEvent);
    message->GetArgumentList()->SetString(0, std::string{name});
    message->GetArgumentList()->SetString(1, arguments.empty() ? "[]" : std::string{arguments});

    browser->GetMainFrame()->SendProcessMessage(PID_RENDERER, message);
}

void Browser::post(std::string_view json) {
    if (state_ == nullptr) {
        return;
    }

    CefRefPtr<CefBrowser> browser;

    {
        const std::lock_guard guard{state_->mutex};
        browser = state_->browser;
    }

    if (browser == nullptr) {
        return;
    }

    // Сообщение доставляется вызовом на самой странице, а не отдельным
    // механизмом: у CEF нет своего канала «наружу → страница», зато есть
    // выполнение кода в её окружении. Разбирать доставленное — забота страницы.
    std::string call = "window.oxymp && window.oxymp.receive(";
    call.append(json);
    call.append(");");

    browser->GetMainFrame()->ExecuteJavaScript(call, browser->GetMainFrame()->GetURL(), 0);
}

void Browser::evaluate(std::string_view script) {
    if (state_ == nullptr || script.empty()) {
        return;
    }

    const CefRefPtr<CefBrowser> browser = state_->current();
    if (browser == nullptr) {
        return;
    }

    browser->GetMainFrame()->ExecuteJavaScript(std::string{script},
                                               browser->GetMainFrame()->GetURL(), 0);
}

void Browser::resize(int width, int height) {
    if (state_ == nullptr || width <= 0 || height <= 0) {
        return;
    }

    CefRefPtr<CefBrowser> browser;

    {
        const std::lock_guard guard{state_->mutex};

        if (state_->width == width && state_->height == height) {
            return;
        }

        state_->width = width;
        state_->height = height;

        browser = state_->browser;
    }

    if (browser != nullptr) {
        // CEF спросит новый размер сам, но только когда ему скажут, что тот
        // изменился.
        browser->GetHost()->WasResized();
    }
}

void Browser::moveMouse(int x, int y) {
    if (state_ == nullptr) {
        return;
    }

    if (const CefRefPtr<CefBrowser> browser = state_->current(); browser != nullptr) {
        browser->GetHost()->SendMouseMoveEvent(state_->mouseAt(x, y), false);
    }
}

void Browser::clickMouse(int x, int y, MouseButton button, bool down) {
    if (state_ == nullptr) {
        return;
    }

    const CefRefPtr<CefBrowser> browser = state_->current();
    if (browser == nullptr) {
        return;
    }

    // Признак нажатия учитывается до самого события, а не после: страница ждёт
    // его уже в том событии, которым кнопку нажали.
    {
        const std::lock_guard guard{state_->mutex};

        if (down) {
            state_->held |= buttonFlag(button);
        } else {
            state_->held &= ~buttonFlag(button);
        }
    }

    // Один щелчок, а не два: двойных нажатий страница не различает, и считать
    // их незачем.
    browser->GetHost()->SendMouseClickEvent(state_->mouseAt(x, y), buttonType(button), !down, 1);
}

void Browser::scrollMouse(int x, int y, int delta) {
    if (state_ == nullptr) {
        return;
    }

    if (const CefRefPtr<CefBrowser> browser = state_->current(); browser != nullptr) {
        browser->GetHost()->SendMouseWheelEvent(state_->mouseAt(x, y), 0, delta);
    }
}

void Browser::releaseMouse() {
    if (state_ == nullptr) {
        return;
    }

    const CefRefPtr<CefBrowser> browser = state_->current();
    if (browser == nullptr) {
        return;
    }

    {
        const std::lock_guard guard{state_->mutex};
        state_->held = 0;
    }

    // Точка за пределами страницы вместе с признаком ухода: иначе последний
    // пункт, над которым стоял указатель, остался бы подсвеченным навсегда.
    browser->GetHost()->SendMouseMoveEvent(state_->mouseAt(-1, -1), true);
}

void Browser::sendKey(KeyAction action, unsigned code, bool shift, bool control, bool alt) {
    if (state_ == nullptr) {
        return;
    }

    const CefRefPtr<CefBrowser> browser = state_->current();
    if (browser == nullptr) {
        return;
    }

    CefKeyEvent event;

    switch (action) {
    case KeyAction::Down:
        // RAWKEYDOWN, а не KEYDOWN: KEYDOWN Chromium порождает себе сам вместе
        // со знаком, разбирая сырое нажатие. Пошли мы его сами — набранное
        // удвоилось бы.
        event.type = KEYEVENT_RAWKEYDOWN;
        break;
    case KeyAction::Up:
        event.type = KEYEVENT_KEYUP;
        break;
    case KeyAction::Char:
        event.type = KEYEVENT_CHAR;
        break;
    }

    event.windows_key_code = static_cast<int>(code);

    if (action == KeyAction::Char) {
        event.character = static_cast<char16_t>(code);
        event.unmodified_character = event.character;
    }

    event.modifiers = (shift ? EVENTFLAG_SHIFT_DOWN : 0u) |
                      (control ? EVENTFLAG_CONTROL_DOWN : 0u) | (alt ? EVENTFLAG_ALT_DOWN : 0u);

    browser->GetHost()->SendKeyEvent(event);
}

void Browser::setFocus(bool focused) {
    if (state_ == nullptr) {
        return;
    }

    if (const CefRefPtr<CefBrowser> browser = state_->current(); browser != nullptr) {
        browser->GetHost()->SetFocus(focused);
    }
}

void Browser::onMessage(MessageHandler handler) {
    if (state_ != nullptr) {
        state_->onMessage = std::move(handler);
    }
}

void Browser::onEvent(EventHandler handler) {
    if (state_ != nullptr) {
        state_->onEvent = std::move(handler);
    }
}

} // namespace oxymp::cefui
