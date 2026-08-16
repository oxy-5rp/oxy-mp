#include "menu_input.hpp"

#include <oxymp/cefui/browser.hpp>

#include <spdlog/spdlog.h>

#include <utility>

namespace oxymp::client::game {
namespace {

/// Единственный перехват на процесс.
///
/// Оконный обработчик — свободная функция без своего состояния: Windows зовёт её
/// по указателю и ничего своего с ней не передаёт. Окно игры одно, и перехват на
/// нём тоже один.
MenuInput* g_input = nullptr;

/// Достаёт из сообщения точку, в которой оно случилось.
///
/// Windows кладёт её в младшее и старшее слово одного числа, и знак при этом
/// теряется: указатель, уведённый выше окна, приходит как очень большое
/// положительное. Приведение к short возвращает знак на место.
[[nodiscard]] int mouseX(LPARAM lparam) {
    return static_cast<short>(LOWORD(lparam));
}

[[nodiscard]] int mouseY(LPARAM lparam) {
    return static_cast<short>(HIWORD(lparam));
}

[[nodiscard]] bool held(int key) {
    return (::GetKeyState(key) & 0x8000) != 0;
}

} // namespace

std::unique_ptr<MenuInput> MenuInput::install(HWND window, cefui::Browser& browser,
                                              std::function<bool()> wanted,
                                              std::function<void()> toggle, std::string& error) {
    if (window == nullptr) {
        error = "окна игры нет: ввод для меню не перехватить";
        return nullptr;
    }

    if (g_input != nullptr) {
        error = "перехват ввода для меню уже стоит";
        return nullptr;
    }

    std::unique_ptr<MenuInput> input{new MenuInput};

    input->window_ = window;
    input->browser_ = &browser;
    input->wanted_ = std::move(wanted);
    input->toggle_ = std::move(toggle);

    // Указатель ставится раньше подмены: подменённый обработчик вправе получить
    // сообщение в тот же миг, и застать он должен уже готовое.
    g_input = input.get();

    input->previous_ = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuInput::proc)));

    if (input->previous_ == nullptr) {
        g_input = nullptr;

        error = "не удалось подменить обработчик окна, ошибка " + std::to_string(::GetLastError());
        return nullptr;
    }

    spdlog::debug("ввод для меню перехвачен");
    return input;
}

MenuInput::~MenuInput() {
    if (window_ != nullptr && previous_ != nullptr) {
        ::SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_));
    }

    if (wasOpen_) {
        ::ShowCursor(FALSE);
    }

    g_input = nullptr;
}

LRESULT CALLBACK MenuInput::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    MenuInput* const input = g_input;

    if (input != nullptr && input->handle(message, wparam, lparam)) {
        return 0;
    }

    if (input != nullptr && input->previous_ != nullptr) {
        return ::CallWindowProcW(input->previous_, window, message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

bool MenuInput::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    const bool open = wanted_ && wanted_();

    // Escape разбирается прежде всего остального и в обе стороны: закрытым меню
    // он открывает, открытым — закрывает. Иначе закрытое меню было бы уже не
    // достать, а открытое — не убрать: своей клавиши у страницы нет.
    //
    // Только нажатие: на отпускании игра открыла бы своё меню паузы поверх нашего.
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        if (toggle_) {
            toggle_();
        }

        return true;
    }

    if (message == WM_KEYUP && wparam == VK_ESCAPE) {
        return true;
    }

    if (open != wasOpen_) {
        wasOpen_ = open;

        // Внимание отдаётся странице прямо: окна у неё нет, и Windows не может
        // отнять его в её пользу сама. Без этого поля ввода не показывают
        // курсора и не принимают набранного.
        browser_->setFocus(open);

        // Указатель показывается, пока меню открыто. Счётчик у ShowCursor
        // накопительный, поэтому на каждое открытие приходится ровно одно
        // закрытие — и то же самое в разрушителе, если меню осталось открытым.
        ::ShowCursor(open ? TRUE : FALSE);

        if (!open) {
            browser_->releaseMouse();
        }
    }

    if (!open) {
        return false;
    }

    switch (message) {
    case WM_MOUSEMOVE:
        browser_->moveMouse(mouseX(lparam), mouseY(lparam));
        return true;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        browser_->clickMouse(mouseX(lparam), mouseY(lparam), cefui::Browser::MouseButton::Left,
                             message == WM_LBUTTONDOWN);
        return true;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        browser_->clickMouse(mouseX(lparam), mouseY(lparam), cefui::Browser::MouseButton::Right,
                             message == WM_RBUTTONDOWN);
        return true;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        browser_->clickMouse(mouseX(lparam), mouseY(lparam), cefui::Browser::MouseButton::Middle,
                             message == WM_MBUTTONDOWN);
        return true;

    case WM_MOUSEWHEEL: {
        // Колесо приходит в координатах экрана, а не окна: так устроено это
        // сообщение, в отличие от всех остальных мышиных. Странице нужны
        // оконные.
        POINT point{mouseX(lparam), mouseY(lparam)};
        ::ScreenToClient(window_, &point);

        browser_->scrollMouse(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wparam));
        return true;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        browser_->sendKey(cefui::Browser::KeyAction::Down, static_cast<unsigned>(wparam),
                          held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));
        return true;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        browser_->sendKey(cefui::Browser::KeyAction::Up, static_cast<unsigned>(wparam),
                          held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));
        return true;

    case WM_CHAR:
        // Знак, а не код клавиши: какую букву даёт клавиша, решают раскладка,
        // регистр и мёртвые знаки, и решает это Windows. Собрать то же самое из
        // кодов нельзя — вышла бы латиница и ничего больше.
        browser_->sendKey(cefui::Browser::KeyAction::Char, static_cast<unsigned>(wparam),
                          held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));
        return true;

    case WM_INPUTLANGCHANGEREQUEST:
        // Смену раскладки исполняет тот, кто передаст просьбу дальше. Игра её
        // съедает — оттого раскладка в ней «переключается и возвращается
        // обратно». Здесь она нужна: имя игрока набирают буквами.
        return false;

    default:
        return false;
    }
}

} // namespace oxymp::client::game
