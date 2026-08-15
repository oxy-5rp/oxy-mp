#pragma once

#include "native_table.hpp"

#include <cstdint>
#include <string>

namespace oxymp::client::game {

/// Цвет надписи.
struct Colour {
    std::uint8_t red = 255;
    std::uint8_t green = 255;
    std::uint8_t blue = 255;
    std::uint8_t alpha = 255;
};

/// Надписи поверх игрового кадра.
///
/// Рисует нативами самой игры, а не своим наложением поверх DirectX. Так надпись
/// живёт по правилам игры: масштабируется с разрешением, попадает в её порядок
/// отрисовки и исчезает вместе с кадром, — и не требует перехвата графики.
///
/// Вызывать можно только изнутри скриптового тика: нативы рисования требуют
/// действующего скриптового контекста, а вне кадра рисовать попросту негде.
class Hud {
public:
    /// Разыскивает нужные нативы. Готовность проверяется через ready().
    explicit Hud(const NativeTable& table) noexcept;

    /// Все ли нативы нашлись. Без этого рисовать нельзя: пустой обработчик
    /// означал бы вызов по нулевому адресу.
    [[nodiscard]] bool ready() const noexcept;

    /// Рисует строку. Координаты доли экрана: 0,0 — левый верхний угол.
    void drawText(const std::string& text, float x, float y, float scale, Colour colour,
                  bool centred) const;

    /// Рисует прямоугольник. Координаты задают его середину, а не угол — так
    /// устроен натив игры, и подменять это своей системой координат значило бы
    /// заводить два разных смысла у одних и тех же долей экрана.
    void drawRect(float centreX, float centreY, float width, float height, Colour colour) const;

    /// Прячет счётчик денег игры на этот кадр.
    ///
    /// Число, которое он показывает, — из сохранения настоящего GTA Online. Оно
    /// чужое: за ним стоят покупки игрока в настоящей игре, и к нашей сессии оно
    /// отношения не имеет. Показывать в мультиплеере чужой счёт хуже, чем не
    /// показывать никакого: игрок читает его как свой.
    ///
    /// Своего счётчика взамен клиент не рисует. Деньги принадлежат серверу, и
    /// показывать их — дело страницы игрового режима, а не клиента.
    ///
    /// Прячем, а не переписываем, и это принципиально. Переписать значило бы
    /// влезть в чужое сохранение — испортить человеку настоящую игру ради
    /// красивой цифры в нашей. Скрытый счётчик не меняет в сохранении ни байта.
    ///
    /// «На этот кадр» — так устроен натив: игра каждый кадр собирает HUD заново,
    /// и запрет живёт ровно до следующего. Поэтому зовётся он из тика, каждый
    /// раз.
    void hideMoney() const;

private:
    NativeHandler drawRect_ = nullptr;
    NativeHandler setFont_ = nullptr;
    NativeHandler setScale_ = nullptr;
    NativeHandler setColour_ = nullptr;
    NativeHandler setCentre_ = nullptr;
    NativeHandler setOutline_ = nullptr;
    NativeHandler beginText_ = nullptr;
    NativeHandler addSubstring_ = nullptr;
    NativeHandler endText_ = nullptr;
    NativeHandler hideComponent_ = nullptr;
};

} // namespace oxymp::client::game
