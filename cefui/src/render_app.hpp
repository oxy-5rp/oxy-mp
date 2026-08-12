#pragma once

#include "page_message.hpp"

#include <include/cef_app.h>
#include <include/cef_v8.h>

namespace oxymp::cefui {

/// Голос страницы: window.oxympSend(строка).
///
/// Обратного пути у страницы иначе нет. К клиенту она попадает через
/// window.oxymp.receive — тот вызывает сам клиент, выполняя код в её окружении.
/// А наружу выполнять нечего: страница живёт в чужом процессе, и всё, что ей
/// доступно, — послать процессу браузера сообщение. Отсюда и эта единственная
/// добавленная ей функция.
class PageVoice : public CefV8Handler {
public:
    bool Execute(const CefString& /*name*/, CefRefPtr<CefV8Value> /*object*/,
                 const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& /*retval*/,
                 CefString& exception) override {
        if (arguments.empty() || !arguments[0]->IsString()) {
            exception = "oxympSend ждёт строку";
            return true;
        }

        const CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        if (context == nullptr || context->GetFrame() == nullptr) {
            return true;
        }

        const CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kMessageFromPage);
        message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());

        context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(PageVoice);
};

/// Настройки процесса отрисовки страницы.
///
/// Вся его работа здесь — дать странице голос. Ничего другого этому процессу от
/// нас не нужно: он запускается в нескольких экземплярах, и всё лишнее в нём
/// умножается на их число.
class RenderApp : public CefApp, public CefRenderProcessHandler {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnContextCreated(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/,
                          CefRefPtr<CefV8Context> context) override {
        // При каждой загрузке страницы заново: окружение страницы создаётся
        // вместе с ней, и добавленное в прежнее не переживает перехода.
        context->GetGlobal()->SetValue(
            "oxympSend", CefV8Value::CreateFunction("oxympSend", new PageVoice{}),
            V8_PROPERTY_ATTRIBUTE_READONLY);
    }

private:
    IMPLEMENT_REFCOUNTING(RenderApp);
};

} // namespace oxymp::cefui
