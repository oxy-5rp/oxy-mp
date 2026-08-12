#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace oxymp::cefui {

/// Готова ли работа CEF в этом процессе.
///
/// Chromium поднимается один раз на процесс и живёт до его конца — остановить и
/// запустить его заново нельзя. Отсюда и вид: не объект, а две свободные
/// функции.
///
/// root — каталог с libcef.dll и остальным хозяйством CEF. Он же ищется рядом
/// подпроцессом. Указывать его приходится явно: клиент внедряется в чужой
/// процесс, и «рядом с исполняемым файлом» для него означает папку игры, куда
/// oxyMP не кладёт ничего.
[[nodiscard]] bool startRuntime(const std::wstring& root, std::string& error);

/// Останавливает Chromium. Вызывать один раз, перед выгрузкой модуля.
void stopRuntime();

/// Страница, нарисованная в память.
///
/// Ради этого CEF и заведён. Обычный браузерный движок отдаёт страницу в окно, а
/// окно поверх игры принадлежит рабочему столу: исчезает при сворачивании,
/// спорит с полноэкранным режимом, живёт по чужим правилам. CEF умеет отдавать
/// готовые точки — и они ложатся в кадр игры текстурой, становясь его частью.
///
/// Точки приходят с настоящей прозрачностью: там, где страница ничего не
/// нарисовала, четвёртый канал равен нулю. Никаких порогов яркости и чёрных
/// фонов для этого больше не нужно.
class Browser {
public:
    /// Что делать с новым кадром страницы.
    ///
    /// Вызывается из потока CEF, а не из потока игры: перекладывать точки в своё
    /// место и уходить — вся работа, которую здесь позволено делать.
    ///
    /// Точки идут в порядке BGRA, строка к строке без выравнивания.
    using PaintHandler = std::function<void(const std::uint8_t* pixels, int width, int height)>;

    /// Что делать с сообщением от страницы. Приходит уже как строка JSON.
    using MessageHandler = std::function<void(std::string_view json)>;

    /// Заводит страницу заданного размера.
    ///
    /// Работа CEF должна быть уже начата: без неё создавать нечего.
    [[nodiscard]] static std::unique_ptr<Browser> create(int width, int height,
                                                         PaintHandler onPaint,
                                                         std::string& error);

    ~Browser();

    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    /// Показывает страницу. Содержимое передаётся целиком, файла нет.
    void show(std::string_view html);

    /// Отправляет странице сообщение. Ожидается строка JSON.
    void post(std::string_view json);

    /// Меняет размер страницы. Следующий кадр придёт уже новым.
    void resize(int width, int height);

    void onMessage(MessageHandler handler);

private:
    Browser();

    struct State;
    std::shared_ptr<State> state_;
};

} // namespace oxymp::cefui
