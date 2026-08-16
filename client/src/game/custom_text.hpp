#pragma once

#include "engine_addresses.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace oxymp::client::game {

/// Подмена надписей самой игры.
///
/// Все надписи GTA лежат в её словаре и достаются по хешу имени: `FE_THDR_GTAO`
/// — заголовок меню паузы, `PM_PANE_LEAVE` — строка выхода из сессии. Игра
/// спрашивает их одной функцией, и подменить ответ этой функции — единственный
/// способ переписать надпись, не трогая ни одного файла игры.
///
/// Так же поступает CitizenFX (`CustomText.cpp`), и оттуда же взяты обе точки
/// подмены: спрашивают словарь из двух мест, и уводить нужно оба — иначе
/// половина надписей осталась бы игровой.
///
/// Нужно это ради очевидного: в меню паузы у oxyMP не должно быть написано «GTA
/// ONLINE». Игрок в нашей сессии, а не в чужой.
class CustomText {
public:
    [[nodiscard]] static std::unique_ptr<CustomText> install(const EngineAddresses& addresses,
                                                             std::string& error);

    ~CustomText();

    CustomText(const CustomText&) = delete;
    CustomText& operator=(const CustomText&) = delete;

    /// Задаёт свою надпись под именем из словаря игры.
    ///
    /// Зовётся до того, как игра спросит надпись, и с любого потока: спрашивает
    /// она из своего, а список меняется под блокировкой.
    void set(std::string_view key, std::string value);

private:
    CustomText() = default;

    struct State;
    static State* active_;

    /// То, что игра зовёт вместо своего словаря.
    static const char* lookUp(void* dictionary, std::uint32_t hash);

    std::unique_ptr<State> state_;
};

} // namespace oxymp::client::game
