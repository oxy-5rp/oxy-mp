#include "launcher_window.hpp"

#include "page_text.hpp"
#include "skin_assets.hpp"

#include <oxymp/webui/browser.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include "launcher_page.hpp"

namespace oxymp::launcher {
namespace {

constexpr const wchar_t* kWindowClass = L"oxyMPLauncher";

/// Имя окна, когда оформления нет.
constexpr const wchar_t* kDefaultTitle = L"oxyMP";

/// Сообщение «в рабочем потоке что-то произошло».
///
/// Окном распоряжается только тот поток, который его создал, поэтому запуск игры
/// не может ни трогать страницу, ни ждать её. Он лишь кладёт готовую строку в
/// очередь окна, а разбирает её уже поток окна.
constexpr UINT kProgressMessage = WM_APP + 1;

/// Сообщение «игра поднялась и показала своё окно».
///
/// По нему лаунчер убирается с глаз: своё дело он сделал, а второе окно поверх
/// игры мешало бы. Совсем закрывать его нельзя — он ещё дождётся конца игры.
constexpr UINT kGameUpMessage = WM_APP + 2;

/// Сообщение «игра закончилась». По нему окно закрывается само.
constexpr UINT kGameGoneMessage = WM_APP + 3;

std::string_view describe(Progress progress) {
    switch (progress) {
    case Progress::Working:
        return "work";
    case Progress::Ready:
        return "ok";
    case Progress::Failed:
        return "bad";
    }
    return "idle";
}

/// Состояние окна. Одно на процесс: окон лаунчера не бывает двух.
struct Window {
    HWND handle = nullptr;
    std::unique_ptr<webui::Browser> browser;

    Paths paths;
    Session::Settings settings;

    std::thread worker;
};

Window* g_window = nullptr;

/// Отправляет странице строку состояния. Только из потока окна.
void report(Progress progress, std::string_view text) {
    if (g_window == nullptr || !g_window->browser) {
        return;
    }

    g_window->browser->post(std::string{"{\"action\":\"status\",\"kind\":\""} +
                            std::string{describe(progress)} + "\",\"text\":\"" +
                            escapeJson(text) + "\"}");
}

/// Кладёт строку состояния в очередь окна. Можно из любого потока.
///
/// Окно берётся доводом, а не из g_window, и это не педантизм. Рабочий поток
/// переживает окно: игрок вправе закрыть лаунчер, пока идёт запуск, — и тогда
/// обращение к общей записи было бы обращением к тому, чего уже нет. Указатель
/// на окно после закрытия просто перестаёт работать, и это ровно то, что нужно.
void postProgress(HWND window, Progress progress, std::string_view text) {
    if (window == nullptr) {
        return;
    }

    // Строка переживает передачу в очередь только на куче: стек рабочего потока
    // к моменту разбора сообщения уже уйдёт дальше.
    auto* const carried = new std::string{text};

    if (::PostMessageW(window, kProgressMessage, static_cast<WPARAM>(progress),
                       reinterpret_cast<LPARAM>(carried)) == 0) {
        // Окна больше нет: сообщение не встанет в очередь, и разбирать его
        // некому. Освобождаем сами, иначе строка осталась бы висеть.
        delete carried;
    }
}

void startSession(Window& window) {
    // Запуск идёт в своём потоке: он занимает минуты, а окно всё это время
    // обязано отвечать и показывать, что происходит.
    window.worker = std::thread([settings = window.settings, handle = window.handle] {
        auto game = Session::run(settings, [handle](Progress progress, std::string_view text) {
            spdlog::info("{}", text);
            postProgress(handle, progress, text);
        });

        if (game == nullptr) {
            // Окно остаётся на экране с последней строкой: она и объясняет, что
            // пошло не так. Закрыть его игрок закроет сам.
            return;
        }

        ::PostMessageW(handle, kGameUpMessage, 0, 0);

        game->waitForExit();

        ::PostMessageW(handle, kGameGoneMessage, 0, 0);
    });
}

void handlePageMessage(std::string_view json) {
    // Строкой, а не видом на неё: field отдаёт строку по значению, и вид на
    // временный объект пережил бы сам объект. Ровно на этом сломались разом
    // запуск игры, перетаскивание окна и его кнопки — сравнивалась
    // освобождённая память, и не совпадало ничего.
    const std::string action = jsonField(json, "action");

    // Кнопки окна нарисованы на странице, а делает по ним всё равно окно: у
    // страницы своего окна нет, она живёт внутри нашего.
    if (action == "window") {
        if (g_window == nullptr || g_window->handle == nullptr) {
            return;
        }

        const std::string command = jsonField(json, "command");

        if (command == "close") {
            ::PostMessageW(g_window->handle, WM_CLOSE, 0, 0);
        } else if (command == "minimize") {
            ::ShowWindow(g_window->handle, SW_MINIMIZE);
        }

        return;
    }

    if (action == "drag") {
        // Перетаскивание за картинку. Окно само отпускает мышь и берёт ведение
        // на себя — так же, как это делает обычный заголовок Windows.
        if (g_window != nullptr && g_window->handle != nullptr) {
            ::ReleaseCapture();
            ::SendMessageW(g_window->handle, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kProgressMessage: {
        const std::unique_ptr<std::string> text{reinterpret_cast<std::string*>(lparam)};
        report(static_cast<Progress>(wparam), *text);
        return 0;
    }

    case kGameUpMessage:
        // Игра поднялась — лаунчеру больше нечего показывать. Он не закрывается,
        // а прячется: закрытое окно закрыло бы и очередь сообщений, а по ней
        // придёт весть о конце игры.
        ::ShowWindow(window, SW_HIDE);
        return 0;

    case kGameGoneMessage:
        ::PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;

    case WM_SIZE:
        if (g_window != nullptr && g_window->browser) {
            g_window->browser->resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int LauncherWindow::run(const Paths& paths, Session::Settings settings) {
    Window window;
    window.paths = paths;
    window.settings = std::move(settings);
    g_window = &window;

    // Оформление читается до окна: от него зависит и размер окна, и его имя, и
    // значок — всё то, что задаётся при создании и меняется потом с трудом.
    const SkinAssets skin = SkinAssets::load(paths.root / "skin.bin");

    const std::wstring title = skin.name.empty() ? std::wstring{kDefaultTitle} : widen(skin.name);

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &windowProcedure;
    description.hInstance = instance;
    description.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    description.hbrBackground = ::CreateSolidBrush(RGB(10, 12, 16));
    description.lpszClassName = kWindowClass;

    if (::RegisterClassExW(&description) == 0) {
        spdlog::error("the window class could not be registered");
        return 1;
    }

    // Окно без рамки и без заголовка Windows.
    //
    // Своя кнопка закрытия нарисована на странице, а рамка Windows дорисовала бы
    // сверху вторую — с чужими цветами и вторым набором тех же кнопок. WS_POPUP
    // убирает её целиком, и клиентская область становится равна окну: картинка
    // занимает его без остатка.
    const DWORD style = WS_POPUP | WS_CLIPCHILDREN;

    // Размер задан оформлением и не меняется: картинка нарисована ровно под
    // него. Растянутое окно показало бы её мыльной, а сжатое обрезало бы.
    const int width = skin.width;
    const int height = skin.height;

    // По середине того экрана, где сейчас курсор.
    POINT cursor{};
    ::GetCursorPos(&cursor);

    MONITORINFO screen{sizeof(screen)};
    ::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &screen);

    const int left = screen.rcWork.left + ((screen.rcWork.right - screen.rcWork.left) - width) / 2;
    const int top = screen.rcWork.top + ((screen.rcWork.bottom - screen.rcWork.top) - height) / 2;

    window.handle = ::CreateWindowExW(0, kWindowClass, title.c_str(), style, left, top, width,
                                      height, nullptr, nullptr, instance, nullptr);

    if (window.handle == nullptr) {
        spdlog::error("the window could not be created");
        return 1;
    }

    // Значок из оформления — тот же, что у alt:V: он виден на панели задач и в
    // переключателе окон. Своего у окна нет вовсе: рамки нет, и рисовать его
    // негде, — но панель задач берёт его отсюда.
    if (skin.largeIcon != nullptr) {
        ::SendMessageW(window.handle, WM_SETICON, ICON_BIG,
                       reinterpret_cast<LPARAM>(skin.largeIcon));
    }
    if (skin.smallIcon != nullptr) {
        ::SendMessageW(window.handle, WM_SETICON, ICON_SMALL,
                       reinterpret_cast<LPARAM>(skin.smallIcon));
    }

    std::string error;
    window.browser = webui::Browser::create(window.handle, paths.browserCache().wstring(), false,
                                            error);
    if (window.browser == nullptr) {
        ::MessageBoxW(window.handle, L"The interface engine could not be started.\n"
                                     L"Install the Microsoft Edge WebView2 Runtime.",
                      title.c_str(), MB_ICONERROR | MB_OK);
        spdlog::error("{}", error);
        return 1;
    }

    std::string page{ui::launcherPage};

    // Фона может не быть вовсе — тогда остаётся тёмная заливка страницы. `none`
    // здесь обязателен: пустая строка в `background-image` — это не «ничего», а
    // ошибка разбора, и вместе с ней пропало бы всё правило.
    page = fillPage(std::move(page), "{{background}}",
                    skin.background.empty() ? "none" : "url(\"" + skin.background + "\")");
    page = fillPage(std::move(page), "{{accent}}", skin.accent.empty() ? "#4f8ef7" : skin.accent);
    page = fillPage(std::move(page), "{{name}}", skin.name.empty() ? "oxyMP" : skin.name);
    page = fillPage(std::move(page), "{{version}}", OXYMP_VERSION);

    window.browser->onMessage(&handlePageMessage);
    window.browser->show(page);

    ::ShowWindow(window.handle, SW_SHOW);

    // Запуск начинается сразу за показом окна: нажимать в нём нечего, и ждать
    // нажатия было бы ожиданием неизвестно чего.
    startSession(window);

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    if (window.worker.joinable()) {
        window.worker.detach();
    }

    g_window = nullptr;
    return 0;
}

} // namespace oxymp::launcher
