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
};

} // namespace oxymp::client::game
