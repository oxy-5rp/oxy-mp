#pragma once

#include <functional>
#include <memory>
#include <string>

struct IDXGISwapChain;

namespace oxymp::client::game {

/// Перехват показа кадра.
///
/// Единственная точка, откуда можно нарисовать что-то поверх игры так, чтобы это
/// оказалось внутри её кадра, а не отдельным окном сверху. Разница не
/// косметическая: окно поверх игры принадлежит рабочему столу — оно исчезает при
/// сворачивании, спорит с полноэкранным режимом и живёт по чужим правилам. То,
/// что нарисовано в Present, — часть кадра, и никаких своих правил у него нет.
///
/// Так устроен интерфейс и у RAGE MP, и у alt:V.
///
/// Перехват ставится подменой записи в таблице методов интерфейса swapchain.
/// Саму таблицу приходится добывать окольным путём: у нас нет ни устройства
/// игры, ни её swapchain, поэтому заводится свой, одноразовый, — таблица у всех
/// реализаций одного интерфейса общая.
class PresentHook {
public:
    /// Что делать перед показом кадра. Вызывается из потока отрисовки игры.
    using Callback = std::function<void(IDXGISwapChain* swapchain)>;

    /// Что делать перед пересозданием буферов кадра.
    ///
    /// Приходит при смене разрешения и при переключении полноэкранного режима.
    /// Всё, что ссылается на прежние буферы, обязано отпустить их здесь: иначе
    /// пересоздание не удастся, и игра останется с прежним разрешением либо
    /// упадёт.
    using ResizeCallback = std::function<void()>;

    [[nodiscard]] static std::unique_ptr<PresentHook> install(Callback onPresent,
                                                              ResizeCallback onResize,
                                                              std::string& error);

    ~PresentHook();

    PresentHook(const PresentHook&) = delete;
    PresentHook& operator=(const PresentHook&) = delete;

private:
    PresentHook() = default;
};

} // namespace oxymp::client::game
