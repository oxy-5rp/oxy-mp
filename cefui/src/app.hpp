#pragma once

#include <include/cef_app.h>

namespace oxymp::cefui {

/// Настройки Chromium, общие на весь процесс.
///
/// Из всего, что умеет CefApp, здесь нужна одна вещь — доводы командной строки,
/// с которыми поднимается Chromium. Остальное оставлено ему самому.
class App : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(const CefString& processType,
                                       CefRefPtr<CefCommandLine> commandLine) override {
        // Общие для всех процессов.
        if (!processType.empty()) {
            return;
        }

        // Отрисовка страницы силами процессора, а не видеокарты.
        //
        // Иначе CEF отдаёт кадр общей текстурой, а не точками, и OnPaint не
        // вызывается вовсе. Страница у нас — панели и текст, ей ускорение и не
        // нужно; а вот делить видеокарту с игрой, которой она нужна вся, ни к
        // чему.
        commandLine->AppendSwitch("disable-gpu");
        commandLine->AppendSwitch("disable-gpu-compositing");

        // Chromium умеет сам решать, что окно закрыто чем-то другим, и
        // переставать рисовать. Окна у нас нет вовсе, и решение это всегда
        // будет неверным.
        commandLine->AppendSwitchWithValue("disable-features",
                                           "CalculateNativeWinOcclusion,HardwareMediaKeyHandling");

        // Звук страницы не нужен: звуки в игре свои.
        commandLine->AppendSwitch("mute-audio");
    }

private:
    IMPLEMENT_REFCOUNTING(App);
};

} // namespace oxymp::cefui
