#pragma once

#include "page_message.hpp"

#include <include/cef_app.h>
#include <include/cef_v8.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

/// Объект alt в окружении страницы.
///
/// Страница меню — это alt:V'шный altv-ui, и разговаривает она ровно так, как
/// научена: alt.on(имя, обработчик) слушает клиента, alt.emit(имя, ...доводы)
/// говорит клиенту. Своего названия здесь заводить нельзя — не потому, что чужое
/// красивее, а потому, что страница правится дальше по своим правилам, и всякое
/// расхождение с ними придётся чинить в ней самой при каждом обновлении.
///
/// Доводы ходят строкой JSON, а превращают их в неё и обратно средства самой
/// страницы: JSON.stringify и JSON.parse взяты из её окружения. Своего разбора
/// JSON здесь нет намеренно — он уже есть у той, кто эти значения породил, и
/// написанный заново разошёлся бы с ней на первом же необычном значении.
class AltBridge : public CefV8Handler {
public:
    explicit AltBridge(CefRefPtr<CefV8Context> context) : context_{std::move(context)} {
        const CefRefPtr<CefV8Value> json = context_->GetGlobal()->GetValue("JSON");
        if (json != nullptr && json->IsObject()) {
            stringify_ = json->GetValue("stringify");
            parse_ = json->GetValue("parse");
        }
    }

    /// Заводит alt в окружении страницы. Зовётся один раз на каждую её загрузку.
    void install() {
        const CefRefPtr<CefV8Value> alt = CefV8Value::CreateObject(nullptr, nullptr);

        for (const char* name : {"on", "once", "off", "emit"}) {
            alt->SetValue(name, CefV8Value::CreateFunction(name, this),
                          V8_PROPERTY_ATTRIBUTE_READONLY);
        }

        context_->GetGlobal()->SetValue("alt", alt, V8_PROPERTY_ATTRIBUTE_READONLY);
    }

    /// Отдаёт странице событие от клиента.
    ///
    /// Обработчики снимаются в копию до вызова: обработчик вправе позвать
    /// alt.off — в том числе на себя самого, — и обход изменяемого под собой
    /// списка кончился бы обращением по освобождённой памяти.
    void dispatch(const std::string& name, const std::string& jsonArguments) {
        // Вход в контекст страницы обязателен, и обойтись без него нельзя.
        //
        // Сюда попадают из сообщения между процессами, а не из вызова со
        // страницы: текущего контекста V8 в этот миг нет вовсе. Без входа
        // CefV8Value::CreateString возвращает пустое значение, JSON.parse не
        // зовётся, и обработчик получает undefined вместо доводов — а выглядит
        // это так, будто событие дошло, но пустым. Стоило разбора: страница
        // ругалась «Cannot use 'in' operator to search for … in undefined»,
        // и искать причину пришлось в мосте, а не в ней.
        const ContextGuard guard{context_};

        const auto found = listeners_.find(name);
        if (found == listeners_.end() || found->second.empty()) {
            return;
        }

        const std::vector<Listener> called = found->second;

        // Одноразовые снимаются до вызова, а не после: обработчик вправе
        // подписаться заново, и снятие после стёрло бы новую подписку.
        std::erase_if(found->second, [](const Listener& listener) { return listener.once; });

        const CefV8ValueList arguments = fromJson(jsonArguments);

        for (const Listener& listener : called) {
            listener.function->ExecuteFunctionWithContext(context_, nullptr, arguments);
        }
    }

    bool Execute(const CefString& name, CefRefPtr<CefV8Value> /*object*/,
                 const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& /*retval*/,
                 CefString& exception) override {
        const std::string called = name.ToString();

        if (called == "emit") {
            return emit(arguments, exception);
        }

        if (arguments.size() < 2 || !arguments[0]->IsString() || !arguments[1]->IsFunction()) {
            exception = "alt." + called + " ждёт имя события и обработчик";
            return true;
        }

        const std::string event = arguments[0]->GetStringValue().ToString();

        if (called == "off") {
            std::erase_if(listeners_[event], [&arguments](const Listener& listener) {
                return listener.function->IsSame(arguments[1]);
            });
            return true;
        }

        listeners_[event].push_back(Listener{arguments[1], called == "once"});
        return true;
    }

private:
    struct Listener {
        CefRefPtr<CefV8Value> function;

        /// Снять после первого же вызова: это alt.once.
        bool once = false;
    };

    /// Держит контекст страницы входящим, пока живёт.
    ///
    /// Парой Enter/Exit вручную обойтись нельзя: между ними зовётся код
    /// страницы, а он вправе бросить — и невыполненный Exit оставил бы V8 в
    /// чужом контексте навсегда.
    class ContextGuard {
    public:
        explicit ContextGuard(const CefRefPtr<CefV8Context>& context) : context_{context} {
            entered_ = context_ != nullptr && context_->Enter();
        }

        ~ContextGuard() {
            if (entered_) {
                context_->Exit();
            }
        }

        ContextGuard(const ContextGuard&) = delete;
        ContextGuard& operator=(const ContextGuard&) = delete;

    private:
        const CefRefPtr<CefV8Context>& context_;
        bool entered_ = false;
    };

    bool emit(const CefV8ValueList& arguments, CefString& exception) {
        if (arguments.empty() || !arguments[0]->IsString()) {
            exception = "alt.emit ждёт имя события";
            return true;
        }

        if (context_->GetFrame() == nullptr) {
            return true;
        }

        const CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kAltEvent);
        message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
        message->GetArgumentList()->SetString(
            1, toJson(CefV8ValueList{arguments.begin() + 1, arguments.end()}));

        context_->GetFrame()->SendProcessMessage(PID_BROWSER, message);
        return true;
    }

    /// Складывает доводы в массив и просит страницу превратить его в строку.
    std::string toJson(const CefV8ValueList& values) const {
        if (stringify_ == nullptr) {
            return "[]";
        }

        const CefRefPtr<CefV8Value> array = CefV8Value::CreateArray(static_cast<int>(values.size()));
        for (std::size_t index = 0; index < values.size(); ++index) {
            array->SetValue(static_cast<int>(index), values[index]);
        }

        const CefRefPtr<CefV8Value> text =
            stringify_->ExecuteFunctionWithContext(context_, nullptr, CefV8ValueList{array});

        // Неопределённое значение JSON.stringify превращает не в строку, а в
        // undefined: доводы, которые нельзя записать, лучше отдать пустыми, чем
        // разбирать потом строку "undefined" на том конце.
        if (text == nullptr || !text->IsString()) {
            return "[]";
        }

        return text->GetStringValue().ToString();
    }

    /// Разбирает строку обратно в доводы вызова.
    CefV8ValueList fromJson(const std::string& text) const {
        if (parse_ == nullptr || text.empty()) {
            return {};
        }

        const CefRefPtr<CefV8Value> array = parse_->ExecuteFunctionWithContext(
            context_, nullptr, CefV8ValueList{CefV8Value::CreateString(text)});

        if (array == nullptr || !array->IsArray()) {
            return {};
        }

        CefV8ValueList values;
        values.reserve(static_cast<std::size_t>(array->GetArrayLength()));

        for (int index = 0; index < array->GetArrayLength(); ++index) {
            values.push_back(array->GetValue(index));
        }

        return values;
    }

    CefRefPtr<CefV8Context> context_;

    /// JSON.stringify и JSON.parse из окружения самой страницы.
    CefRefPtr<CefV8Value> stringify_;
    CefRefPtr<CefV8Value> parse_;

    std::unordered_map<std::string, std::vector<Listener>> listeners_;

    IMPLEMENT_REFCOUNTING(AltBridge);
};

/// Настройки процесса отрисовки страницы.
///
/// Вся его работа здесь — дать странице голос. Ничего другого этому процессу от
/// нас не нужно: он запускается в нескольких экземплярах, и всё лишнее в нём
/// умножается на их число.
class RenderApp : public CefApp, public CefRenderProcessHandler {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        // При каждой загрузке страницы заново: окружение страницы создаётся
        // вместе с ней, и добавленное в прежнее не переживает перехода.
        context->GetGlobal()->SetValue(
            "oxympSend", CefV8Value::CreateFunction("oxympSend", new PageVoice{}),
            V8_PROPERTY_ATTRIBUTE_READONLY);

        // alt заводится только в главном кадре. Страница меню живёт в нём одном,
        // а вложенным кадрам — если они когда-нибудь появятся — говорить с
        // клиентом не полагается.
        if (!frame->IsMain()) {
            return;
        }

        const CefRefPtr<AltBridge> bridge = new AltBridge{context};
        bridge->install();

        bridges_[browser->GetIdentifier()] = bridge;
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> /*context*/) override {
        if (frame->IsMain()) {
            bridges_.erase(browser->GetIdentifier());
        }
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> /*frame*/,
                                  CefProcessId /*source*/,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (message->GetName() != kAltEvent) {
            return false;
        }

        const auto found = bridges_.find(browser->GetIdentifier());
        if (found == bridges_.end()) {
            return true;
        }

        const CefRefPtr<CefListValue> arguments = message->GetArgumentList();

        found->second->dispatch(arguments->GetString(0).ToString(),
                                arguments->GetString(1).ToString());
        return true;
    }

private:
    /// По мосту на страницу. Не по одному на процесс: Chromium вправе поселить в
    /// одном процессе отрисовки несколько страниц, и общий мост отдавал бы
    /// события обеих каждой из них.
    std::unordered_map<int, CefRefPtr<AltBridge>> bridges_;

    IMPLEMENT_REFCOUNTING(RenderApp);
};

} // namespace oxymp::cefui
