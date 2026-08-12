#include "pointer.hpp"

#include "window.hpp"

#include <oxymp/cefui/browser.hpp>

#include <algorithm>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Нажата ли кнопка мыши прямо сейчас.
///
/// Спрашивается у системы, а не у сообщений окна, и это выбор по необходимости:
/// сообщения окна игра забирает себе, а положение кнопок доступно всем.
bool held(int button) {
    return (::GetAsyncKeyState(button) & 0x8000) != 0;
}

/// Переставлены ли кнопки мыши местами в настройках Windows.
///
/// Спрашивается каждый раз, а не однажды: настройка меняется на ходу, и левша,
/// поменявший кнопки посреди игры, иначе остался бы с меню, которое не
/// нажимается.
bool swapped() {
    return ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
}

} // namespace

void Pointer::follow(cefui::Browser& page, int pageWidth, int pageHeight) {
    const HWND game = Window::findOwnWindow();
    if (game == nullptr || pageWidth <= 0 || pageHeight <= 0) {
        return;
    }

    // Чужое окно впереди — значит игрок сейчас не в игре, а мышь его
    // принадлежит не нам. Без этой проверки нажатие в другом окне пришло бы в
    // меню: положение кнопок система рассказывает всем, независимо от того, кому
    // они предназначались.
    if (::GetForegroundWindow() != game) {
        release(page);
        return;
    }

    POINT cursor{};
    if (::GetCursorPos(&cursor) == FALSE || ::ScreenToClient(game, &cursor) == FALSE) {
        return;
    }

    RECT bounds{};
    ::GetClientRect(game, &bounds);

    const int frameWidth = bounds.right - bounds.left;
    const int frameHeight = bounds.bottom - bounds.top;

    if (frameWidth <= 0 || frameHeight <= 0) {
        return;
    }

    // Пересчёт в точки страницы: она может быть мельче кадра, и тогда указатель
    // на её краю должен приходиться на край кадра, а не на его середину.
    const int x = std::clamp(static_cast<int>(cursor.x) * pageWidth / frameWidth, 0, pageWidth - 1);
    const int y =
        std::clamp(static_cast<int>(cursor.y) * pageHeight / frameHeight, 0, pageHeight - 1);

    if (x != x_ || y != y_) {
        x_ = x;
        y_ = y;
        page.moveMouse(x, y);
    }

    following_ = true;

    const bool exchange = swapped();
    const bool left = held(exchange ? VK_RBUTTON : VK_LBUTTON);
    const bool right = held(exchange ? VK_LBUTTON : VK_RBUTTON);

    // Отправляется только перемена: страница ждёт нажатия и отпускания, а не
    // рассказа о том, что кнопку всё ещё держат.
    if (left != leftDown_) {
        leftDown_ = left;
        page.clickMouse(x, y, cefui::Browser::MouseButton::Left, left);
    }

    if (right != rightDown_) {
        rightDown_ = right;
        page.clickMouse(x, y, cefui::Browser::MouseButton::Right, right);
    }
}

void Pointer::release(cefui::Browser& page) {
    if (!following_) {
        return;
    }

    following_ = false;

    // Нажатое отпускается по-настоящему, а не забывается: страница считает
    // кнопку зажатой до тех пор, пока ей не скажут обратного.
    if (leftDown_) {
        leftDown_ = false;
        page.clickMouse(x_, y_, cefui::Browser::MouseButton::Left, false);
    }

    if (rightDown_) {
        rightDown_ = false;
        page.clickMouse(x_, y_, cefui::Browser::MouseButton::Right, false);
    }

    x_ = -1;
    y_ = -1;

    page.releaseMouse();
}

} // namespace oxymp::client::game
