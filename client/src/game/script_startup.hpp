#pragma once

#include "engine_addresses.hpp"
#include "hook.hpp"

#include <memory>
#include <string>

namespace oxymp::client::game {

/// Перехват точки, с которой игра запускает свои стартовые скрипты.
///
/// Это ключевое место для мультиплеера: отсюда начинается всё, что игрок видит
/// в одиночной игре, — заставка, выбор режима, пролог. И это же единственный
/// момент, когда можно поднять собственный скриптовый поток так, чтобы он
/// пережил перезагрузку сессии.
///
/// Что делать со стартовыми скриптами игры.
enum class GameScripts {
    /// Пустить как обычно. Игра идёт своим чередом: пролог, кат-сцены, сюжет.
    Run,

    /// Не пускать вовсе.
    ///
    /// Отсюда растёт весь одиночный режим, поэтому не запустив их, мы получаем
    /// мир без сюжета — то, что и нужно мультиплееру. Расплата в том, что
    /// вместе с сюжетом не запускается ничего другого, и всё нужное придётся
    /// делать самим.
    Block,
};

/// Перехват ставится один раз и живёт всю сессию.
class ScriptStartup {
public:
    /// Ставит перехват. Механизм перехвата должен быть подготовлен заранее.
    [[nodiscard]] static std::unique_ptr<ScriptStartup> install(const EngineAddresses& addresses,
                                                                GameScripts scripts,
                                                                std::string& error);

    ~ScriptStartup();

    ScriptStartup(const ScriptStartup&) = delete;
    ScriptStartup& operator=(const ScriptStartup&) = delete;

    /// Сколько раз игра доходила до запуска стартовых скриптов.
    ///
    /// Больше одного означает перезагрузку сессии — именно её наш поток и
    /// обязан пережить.
    [[nodiscard]] unsigned int invocations() const noexcept;

private:
    ScriptStartup() = default;

    Hook hook_;
};

} // namespace oxymp::client::game
