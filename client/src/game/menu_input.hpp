#pragma once

#include <functional>
#include <memory>
#include <string>

#include <windows.h>

namespace oxymp::cefui {
class Browser;
}

namespace oxymp::client::game {

/// Мышь и клавиатура для меню.
///
/// Пока меню открыто, ввод принадлежит ему целиком: игра его не видит вовсе.
/// Иначе набранное в поле имени заодно вело бы машину, а щелчок по кнопке
/// стрелял бы.
///
/// Перехват — подмена оконного обработчика игры, тем же способом, что и у набора
/// текста в чате (см. text_entry.hpp). Двое их не спорят: подмены выстраиваются в
/// цепочку, и каждая зовёт предыдущую. Порядок снятия обязан быть обратным
/// порядку установки — это условие соблюдается тем, что слой интерфейса
/// заводится раньше сессии и уходит позже неё.
///
/// Своего окна у страницы нет, а значит нет и мыши, которая по нему ходит:
/// всё, что страница знает о мыши, приходит отсюда.
class MenuInput {
public:
    /// Ставит перехват на окно игры.
    ///
    /// wanted спрашивается на каждое сообщение и отвечает, открыто ли меню.
    /// Спрашивается, а не запоминается: открывает и закрывает себя сама
    /// страница, и узнать об этом можно только у неё.
    ///
    /// browser обязан пережить перехват: в него уходят все сообщения.
    /// toggle зовётся по Escape и означает «игрок просит открыть или закрыть
    /// меню». Ту же клавишу игра тратит на своё меню паузы, и здесь она у неё
    /// отбирается — ровно так же, как это делает alt:V.
    [[nodiscard]] static std::unique_ptr<MenuInput> install(HWND window,
                                                            cefui::Browser& browser,
                                                            std::function<bool()> wanted,
                                                            std::function<void()> toggle,
                                                            std::string& error);

    ~MenuInput();

    MenuInput(const MenuInput&) = delete;
    MenuInput& operator=(const MenuInput&) = delete;

private:
    MenuInput() = default;

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    /// Разбирает сообщение. true означает «съедено, игре не показывать».
    [[nodiscard]] bool handle(UINT message, WPARAM wparam, LPARAM lparam);

    HWND window_ = nullptr;
    WNDPROC previous_ = nullptr;

    cefui::Browser* browser_ = nullptr;
    std::function<bool()> wanted_;
    std::function<void()> toggle_;

    /// Было ли меню открыто в прошлое сообщение.
    ///
    /// По переходу видно, когда отдавать странице внимание и когда показывать
    /// указатель: спрашивать об этом каждое сообщение значило бы дёргать Windows
    /// сотни раз в секунду.
    bool wasOpen_ = false;
};

} // namespace oxymp::client::game
