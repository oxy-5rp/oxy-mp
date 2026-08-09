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
/// Игра запускается напрямую, минуя PlayGTAV.exe: тот путь поднимает защиту
/// сетевого режима, которая нужна только для GTA Online, и делает процесс
/// недоступным для внедрения. Одиночной игре она не требуется.
class GameProcess {
public:
    ~GameProcess();

    GameProcess(const GameProcess&) = delete;
    GameProcess& operator=(const GameProcess&) = delete;

    /// Запускает игру.
    ///
    /// Дочерний процесс наследует окружение запускающего, поэтому настройки
    /// передаются переменными окружения: ни файлов рядом с игрой, ни отдельного
    /// канала связи для этого не нужно.
    [[nodiscard]] static std::unique_ptr<GameProcess> launch(const GameLocation& location,
                                                             std::string& error);

    /// Ждёт, пока игра создаст своё окно.
    ///
    /// Это признак того, что процесс поднялся достаточно, чтобы принимать
    /// внедрение. Раньше окна загрузчик модулей ещё занят собой.
    [[nodiscard]] bool waitUntilWindowAppears(std::chrono::seconds timeout);

    /// Внедряет модуль в процесс игры.
    [[nodiscard]] bool inject(const std::filesystem::path& module, std::string& error);

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
