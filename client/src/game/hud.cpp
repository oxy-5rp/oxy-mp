#include "hud.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

namespace oxymp::client::game {
namespace {

/// Шрифт игры. Ноль — обычный рубленый, читаемый на любом фоне.
constexpr int kFont = 0;

/// Тип текстовой команды. Пустая строка означает «вывести подставленный текст
/// как есть», без обращения к таблице переводов игры.
constexpr const char* kRawStringCommand = "STRING";

} // namespace

Hud::Hud(const NativeTable& table) noexcept
    : drawRect_(table.handlerFor(natives::kDrawRect)),
      setFont_(table.handlerFor(natives::kSetTextFont)),
      setScale_(table.handlerFor(natives::kSetTextScale)),
      setColour_(table.handlerFor(natives::kSetTextColour)),
      setCentre_(table.handlerFor(natives::kSetTextCentre)),
      setOutline_(table.handlerFor(natives::kSetTextOutline)),
      beginText_(table.handlerFor(natives::kBeginTextCommandDisplayText)),
      addSubstring_(table.handlerFor(natives::kAddTextComponentSubstringPlayerName)),
      endText_(table.handlerFor(natives::kEndTextCommandDisplayText)) {}

bool Hud::ready() const noexcept {
    return drawRect_ != nullptr && setFont_ != nullptr && setScale_ != nullptr &&
           setColour_ != nullptr && setCentre_ != nullptr && setOutline_ != nullptr &&
           beginText_ != nullptr && addSubstring_ != nullptr && endText_ != nullptr;
}

void Hud::drawRect(float centreX, float centreY, float width, float height, Colour colour) const {
    if (drawRect_ == nullptr) {
        return;
    }

    // Последний довод в свежих сборках отвечает за учёт безопасной зоны экрана.
    // Он передаётся явно: полагаться на то, что игра прочитает ноль из
    // необъявленной ячейки, — значит зависеть от нашей же реализации контекста.
    invokeNative<void>(drawRect_, centreX, centreY, width, height, static_cast<int>(colour.red),
                       static_cast<int>(colour.green), static_cast<int>(colour.blue),
                       static_cast<int>(colour.alpha), false);
}

void Hud::drawText(const std::string& text, float x, float y, float scale, Colour colour,
                   bool centred) const {
    if (!ready()) {
        return;
    }

    // Порядок обязателен: игра накапливает настройки, а затем применяет их к
    // строке, которую собирает текстовая команда. Настройки, выставленные после
    // начала команды, к этой строке уже не относятся.
    invokeNative<void>(setFont_, kFont);
    invokeNative<void>(setScale_, 0.0F, scale);
    invokeNative<void>(setColour_, static_cast<int>(colour.red), static_cast<int>(colour.green),
                       static_cast<int>(colour.blue), static_cast<int>(colour.alpha));
    invokeNative<void>(setCentre_, centred);

    // Обводка нужна не для красоты: без неё светлый текст теряется на светлом
    // кадре, а тёмный — на тёмном.
    invokeNative<void>(setOutline_);

    invokeNative<void>(beginText_, kRawStringCommand);
    invokeNative<void>(addSubstring_, text.c_str());
    invokeNative<void>(endText_, x, y, 0);
}

} // namespace oxymp::client::game
