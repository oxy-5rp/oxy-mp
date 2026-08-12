#include "connect_window.hpp"

#include <oxymp/webui/browser.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include "connect.hpp"

namespace oxymp::launcher {
namespace {

constexpr const wchar_t* kWindowClass = L"oxyMPConnect";
constexpr const wchar_t* kWindowTitle = L"oxyMP";

/// Размер окна в точках при обычном масштабе. Подобран под содержимое страницы.
constexpr int kWidth = 560;
constexpr int kHeight = 700;

/// Сообщение «в рабочем потоке что-то произошло».
///
/// Окном распоряжается только тот поток, который его создал, поэтому запуск игры
/// не может ни трогать страницу, ни ждать её. Он лишь кладёт готовую строку в
/// очередь окна, а разбирает её уже поток окна.
constexpr UINT kProgressMessage = WM_APP + 1;

/// Экранирует строку для вставки в JSON.
///
/// Своими силами, без библиотеки: наружу уходят два поля, и заводить ради них
/// зависимость — это менять понятный десяток строк на непонятную сотню
/// килобайт. Внутрь при этом не попадает ничего чужого: все строки наши.
std::string escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 16);

    for (const char symbol : text) {
        switch (symbol) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += symbol;
            break;
        }
    }

    return escaped;
}

/// Достаёт строковое поле из JSON.
///
/// Разбор нарочно поверхностный: сообщения приходят с нашей же страницы и
/// состоят из двух полей. Полноценный разбор здесь решал бы задачу, которой нет.
std::string field(std::string_view json, std::string_view name) {
    const std::string key = '"' + std::string{name} + "\"";

    const std::size_t at = json.find(key);
    if (at == std::string_view::npos) {
        return {};
    }

    const std::size_t colon = json.find(':', at + key.size());
    if (colon == std::string_view::npos) {
        return {};
    }

    const std::size_t open = json.find('"', colon);
    if (open == std::string_view::npos) {
        return {};
    }

    std::string value;
    for (std::size_t i = open + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            value += json[++i];
            continue;
        }
        if (json[i] == '"') {
            break;
        }
        value += json[i];
    }

    return value;
}

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

/// Состояние окна. Одно на процесс: окон подключения не бывает двух.
struct Window {
    HWND handle = nullptr;
    std::unique_ptr<webui::Browser> browser;

    Paths paths;
    Session::Settings settings;

    std::thread worker;
    std::atomic<bool> busy{false};
};

Window* g_window = nullptr;

/// Отправляет странице строку состояния. Только из потока окна.
void report(Progress progress, std::string_view text) {
    if (g_window == nullptr || !g_window->browser) {
        return;
    }

    g_window->browser->post(std::string{"{\"action\":\"status\",\"kind\":\""} +
                            std::string{describe(progress)} + "\",\"text\":\"" +
                            escape(text) + "\"}");
}

/// Кладёт строку состояния в очередь окна. Можно из любого потока.
void postProgress(Progress progress, std::string_view text) {
    if (g_window == nullptr) {
        return;
    }

    // Строка переживает передачу в очередь только на куче: стек рабочего потока
    // к моменту разбора сообщения уже уйдёт дальше.
    auto* const carried = new std::string{text};

    ::PostMessageW(g_window->handle, kProgressMessage, static_cast<WPARAM>(progress),
                   reinterpret_cast<LPARAM>(carried));
}

void startSession(std::string address, std::string nickname) {
    if (g_window->busy.exchange(true)) {
        return;
    }

    g_window->settings.server = std::move(address);
    g_window->settings.nickname = std::move(nickname);

    if (g_window->worker.joinable()) {
        g_window->worker.join();
    }

    // Запуск идёт в своём потоке: он занимает минуты, а окно всё это время
    // обязано отвечать и показывать, что происходит.
    g_window->worker = std::thread([settings = g_window->settings] {
        auto game = Session::run(settings, [](Progress progress, std::string_view text) {
            spdlog::info("{}", text);
            postProgress(progress, text);
        });

        if (game == nullptr) {
            g_window->busy.store(false);
            return;
        }

        game->waitForExit();

        postProgress(Progress::Failed, "Игра завершилась.");
        g_window->busy.store(false);
    });
}

void handlePageMessage(std::string_view json) {
    if (field(json, "action") != "connect") {
        return;
    }

    startSession(field(json, "address"), field(json, "nickname"));
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kProgressMessage: {
        const std::unique_ptr<std::string> text{reinterpret_cast<std::string*>(lparam)};
        report(static_cast<Progress>(wparam), *text);
        return 0;
    }

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

int ConnectWindow::run(const Paths& paths, Session::Settings settings) {
    Window window;
    window.paths = paths;
    window.settings = std::move(settings);
    g_window = &window;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &windowProcedure;
    description.hInstance = instance;
    description.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    description.hbrBackground = ::CreateSolidBrush(RGB(10, 12, 16));
    description.lpszClassName = kWindowClass;

    if (::RegisterClassExW(&description) == 0) {
        spdlog::error("не удалось завести класс окна");
        return 1;
    }

    // Размер задаётся клиентской области, а не всему окну: иначе рамка и
    // заголовок съедят часть страницы, и вёрстка окажется обрезанной.
    RECT bounds{0, 0, kWidth, kHeight};
    ::AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);

    window.handle = ::CreateWindowExW(
        0, kWindowClass, kWindowTitle, (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME) & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);

    if (window.handle == nullptr) {
        spdlog::error("не удалось создать окно");
        return 1;
    }

    std::string error;
    window.browser = webui::Browser::create(window.handle, paths.browserCache().wstring(), false, error);
    if (window.browser == nullptr) {
        ::MessageBoxW(window.handle, L"Не удалось запустить движок интерфейса.\n"
                                     L"Установите Microsoft Edge WebView2 Runtime.",
                      kWindowTitle, MB_ICONERROR | MB_OK);
        spdlog::error("{}", error);
        return 1;
    }

    window.browser->onMessage(&handlePageMessage);
    window.browser->show(ui::connectPage);

    ::ShowWindow(window.handle, SW_SHOW);

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
