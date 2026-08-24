#pragma once

#include "game_locator.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace oxymp::launcher {

/// Запущенный процесс игры.
///
/// Запускать игру умеет не только он. Обычный путь oxyMP — прямой запуск
/// GTA5.exe, и тогда процесс создаём мы сами. Есть и второй, через Rockstar
/// Games Launcher с подменой звена BattlEye: там процесс создаёт лаунчер
/// Rockstar, а нам остаётся взять его под наблюдение по номеру.
class GameProcess {
public:
    ~GameProcess();

    GameProcess(const GameProcess&) = delete;
    GameProcess& operator=(const GameProcess&) = delete;

    /// Чем дополнить командную строку прямого запуска.
    ///
    /// При запуске через лаунчер Rockstar то же самое делает подмена звена
    /// BattlEye: командную строку составляет он, и дополнить её можно только
    /// там. При прямом запуске строку составляем мы сами, и дополнять её проще —
    /// но состав от этого не меняется, иначе игра вела бы себя двумя разными
    /// способами в зависимости от пути запуска.
    struct Arguments {
        /// На каком языке запускать игру: `ru-RU`, `en-US`. Пусто — не называть
        /// язык вовсе, и тогда игра возьмёт тот, что выбран в лаунчере Rockstar.
        std::wstring language;

        /// Вести игру сразу в сетевой свободный режим, минуя сюжет.
        bool straightIntoFreemode = false;
    };

    /// Запускает GTA5.exe напрямую, минуя лаунчер Rockstar.
    ///
    /// Основной путь, и это выяснено дорого. Запуск через Rockstar Games Launcher
    /// работает ровно для копии, купленной у Rockstar: копию из Steam или Epic
    /// его лаунчер не создаёт сам, а перепоручает клиенту площадки — и процесс
    /// игры появляется у того. Подмена звена BattlEye стоит в лаунчере Rockstar,
    /// её обработчик в этом случае не вызывается вовсе, и человек получает
    /// обычную GTA V без единого нашего байта внутри.
    ///
    /// Ключ `-fromRGL` — это признак «меня запустил Rockstar Games Launcher»;
    /// получив его, игра спрашивает права у работающего лаунчера по именованному
    /// каналу. Без ключа она закрывается с ERR_NO_LAUNCHER, а с ключом, но при
    /// выключенном лаунчере, — тоже.
    ///
    /// Защита сетевого режима при этом не поднимается, и внедрение работает. Цена
    /// одна: лаунчер Rockstar о запущенной игре не знает и не показывает её
    /// запущенной — он следит за тем процессом, который создал сам, а этот создали
    /// мы. Так же поступает alt:V.
    [[nodiscard]] static std::unique_ptr<GameProcess> launchDirectly(const GameLocation& location,
                                                                     const Arguments& arguments,
                                                                     std::string& error);

    /// Берёт под наблюдение процесс игры по его номеру.
    [[nodiscard]] static std::unique_ptr<GameProcess> attachTo(std::uint32_t processId,
                                                               std::string& error);

    /// Разыскивает уже запущенную игру и берёт её под наблюдение.
    ///
    /// Игру при этом не запускаем: кто и как её поднял — не наше дело. Так oxyMP
    /// вносит мультиплеер в игру, которую пользователь держит живой собственными
    /// средствами.
    ///
    /// waitTimeout > 0 означает подождать появления процесса; ноль — искать один
    /// раз и сразу вернуть результат.
    [[nodiscard]] static std::unique_ptr<GameProcess> attach(std::chrono::seconds waitTimeout,
                                                             std::string& error);

    /// Внедряет модуль в процесс игры.
    [[nodiscard]] bool inject(const std::filesystem::path& module, std::string& error);

    /// Внедряет модуль, повторяя попытки, пока процесс жив.
    ///
    /// Сразу после запуска процесс ещё занят собой и удалённый поток создать в
    /// нём не удаётся. Ждать при этом нечего конкретного: окна у игры может не
    /// появиться вовсе, поэтому единственный надёжный признак готовности — это
    /// удавшееся внедрение.
    [[nodiscard]] bool injectWithRetries(const std::filesystem::path& module,
                                         std::chrono::seconds timeout, std::string& error);

    /// Ждёт завершения игры.
    void waitForExit();

    [[nodiscard]] bool isRunning() const;

    [[nodiscard]] std::uint32_t id() const noexcept { return processId_; }

private:
    GameProcess() = default;

    void* process_ = nullptr;
    void* thread_ = nullptr;
    std::uint32_t processId_ = 0;
};

} // namespace oxymp::launcher
