#pragma once

#include "game_locator.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace oxymp::launcher {

/// Способ запуска игры.
enum class LaunchMode {
    /// Через PlayGTAV.exe — штатный путь Rockstar Games Launcher.
    ///
    /// Обязателен для игры: запущенная в обход лаунчера, она немедленно
    /// закрывается с ошибкой ERR_NO_LAUNCHER. Побочный эффект — вместе с игрой
    /// поднимается защита сетевого режима.
    ViaRockstarLauncher,

    /// Прямой запуск GTA5.exe.
    ///
    /// Игра при этом закрывается с ERR_NO_LAUNCHER, поэтому играть так нельзя.
    /// Оставлено для опытов: процесс успевает подняться и развернуть код в
    /// памяти, а защита сетевого режима не включается.
    Direct,
};

/// Запущенный процесс игры.
class GameProcess {
public:
    ~GameProcess();

    GameProcess(const GameProcess&) = delete;
    GameProcess& operator=(const GameProcess&) = delete;

    /// Запускает игру выбранным способом и берёт её процесс под наблюдение.
    ///
    /// Настройки передаются переменными окружения: запускаемый процесс
    /// наследует окружение лаунчера, и ни файлов рядом с игрой, ни отдельного
    /// канала связи для этого не нужно.
    ///
    /// При запуске через лаунчер Rockstar процесс игры создаём не мы, поэтому
    /// он разыскивается по имени в течение отведённого времени.
    [[nodiscard]] static std::unique_ptr<GameProcess> launch(const GameLocation& location,
                                                             LaunchMode mode,
                                                             std::chrono::seconds appearTimeout,
                                                             std::string& error);

    /// Ждёт, пока игра создаст своё окно.
    ///
    /// Это признак того, что процесс поднялся достаточно, чтобы принимать
    /// внедрение. Раньше окна загрузчик модулей ещё занят собой.
    [[nodiscard]] bool waitUntilWindowAppears(std::chrono::seconds timeout);

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
