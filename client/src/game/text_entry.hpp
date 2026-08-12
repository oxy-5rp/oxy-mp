#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

namespace oxymp::client::game {

/// Набор текста внутри игры: чат и всё, где нужны буквы, а не нажатия.
///
/// Клавиатуру приходится перехватывать самим, и другого пути нет. Интерфейс
/// oxyMP живёт в чужом окне поверх игры, которое намеренно не забирает у неё ни
/// фокуса, ни ввода: забрав, оно отняло бы у игры управление целиком. Значит,
/// буквы обязана получать сама игра — а мы их у неё перехватываем.
///
/// Перехват — это подмена оконного обработчика игры: WM_CHAR приносит уже
/// готовый символ, с учётом раскладки, регистра и мёртвых клавиш. Собирать то же
/// самое из кодов клавиш нельзя — получилась бы латиница и ничего больше.
///
/// Одного перехвата, впрочем, мало: клавиатуру GTA читает не сообщениями окна, а
/// напрямую, и съеденное нами сообщение её не остановит. Поэтому набор текста
/// всегда идёт вместе с запретом игрового ввода — см. Controls::suppressEverything.
class TextEntry {
public:
    /// Подменяет обработчик окна игры.
    ///
    /// Вызывать можно откуда угодно: обработчик после подмены всё равно будет
    /// работать в потоке, которому окно принадлежит.
    [[nodiscard]] static std::unique_ptr<TextEntry> install(HWND window, std::string& error);

    ~TextEntry();

    TextEntry(const TextEntry&) = delete;
    TextEntry& operator=(const TextEntry&) = delete;

    /// Чем закончился набор.
    enum class Outcome {
        /// Набор продолжается.
        Typing,

        /// Игрок нажал ввод.
        Submitted,

        /// Игрок отказался.
        Cancelled,
    };

    /// Начинает набор с чистой строки.
    void begin();

    /// Прекращает набор, что бы в нём ни было.
    void cancel();

    [[nodiscard]] bool active() const;

    /// Что набрано на сейчас.
    [[nodiscard]] std::string text() const;

    /// Забирает исход набора. Отдаётся один раз: прочитав Submitted, вызывающий
    /// становится единственным владельцем набранного.
    [[nodiscard]] Outcome takeOutcome(std::string& text);

private:
    TextEntry() = default;

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    /// Разбирает сообщение. true — сообщение наше и игре его отдавать не нужно.
    bool handle(UINT message, WPARAM wparam);

    /// Дописывает символ UTF-16 в строку UTF-8.
    void append(wchar_t symbol);

    HWND window_ = nullptr;
    WNDPROC previous_ = nullptr;

    mutable std::mutex mutex_;

    bool active_ = false;
    std::string text_;
    Outcome outcome_ = Outcome::Typing;

    /// Первая половина суррогатной пары, если она пришла.
    ///
    /// Символы вне основной плоскости приходят двумя сообщениями, и по одному их
    /// не перевести: половина пары сама по себе не значит ничего.
    wchar_t pendingSurrogate_ = 0;
};

} // namespace oxymp::client::game
