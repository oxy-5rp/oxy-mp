#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <windows.h>

namespace oxymp::webui {

/// Страница в окне: наш интерфейс, нарисованный движком Edge.
///
/// Свой интерфейс строится не средствами игры, и это не прихоть. Замеряно на
/// живой игре: скриптовый тик — единственная точка, откуда доступны нативы
/// рисования, — доходит до клиента через шесть секунд после запуска стартовых
/// скриптов, когда заставка Rockstar, реклама GTA Online и страница выбора
/// режима уже позади. Нарисовать поверх них изнутри игры нельзя ничем.
///
/// Живёт вне процесса игры. Браузер — большой чужой механизм, и его падение не
/// должно уносить с собой GTA; так же устроено и у alt:V с его отдельным
/// altv-webengine.exe.
///
/// Создание идёт не мгновенно: движок поднимается сам, своим чередом, и до этого
/// момента страницы нет. Поэтому конструктор лишь запускает подготовку, а о
/// готовности сообщает обратный вызов.
class Browser {
public:
    /// Что делать с сообщением от страницы. Приходит уже как строка JSON.
    using MessageHandler = std::function<void(std::string_view json)>;

    /// Заводит движок и вешает страницу в указанное окно.
    ///
    /// userDataDirectory — куда движку складывать своё хозяйство. Каталог его
    /// собственный и к нашим данным отношения не имеет, но задать его нужно:
    /// по умолчанию движок кладёт всё рядом с исполняемым файлом.
    /// transparent — просвечивает ли страница насквозь там, где ничего не
    /// нарисовано. Нужно интерфейсу внутри игры: он лежит поверх кадра, и
    /// собственный фон закрыл бы собой игру.
    [[nodiscard]] static std::unique_ptr<Browser> create(HWND window,
                                                         const std::wstring& userDataDirectory,
                                                         bool transparent, std::string& error);

    ~Browser();

    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;

    /// Готова ли страница принимать работу.
    [[nodiscard]] bool ready() const noexcept;

    /// Показывает страницу. Содержимое передаётся целиком, файла нет.
    void show(std::string_view html);

    /// Отправляет странице сообщение. Ожидается строка JSON.
    void post(std::string_view json);

    /// Растягивает страницу по размеру окна. Вызывать при изменении размера.
    void resize(int width, int height);

    void onMessage(MessageHandler handler);

private:
    Browser();

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace oxymp::webui
