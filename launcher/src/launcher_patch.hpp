#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace oxymp::launcher {

/// Подмена звена BattlEye внутри Rockstar Games Launcher.
///
/// Игру поднимает лаунчер Rockstar, и поднимает он не её, а GTA5_BE.exe —
/// промежуточное звено, вместе с которым встаёт защита сетевого режима. Она
/// закрывает память процесса игры: проверено на живой игре, ReadProcessMemory
/// отказывает с ошибкой 5 даже в одиночном режиме.
///
/// Обойти звено, запустив GTA5.exe самим, можно — так oxyMP и делал, — но тогда
/// лаунчер о запущенной игре не знает и не показывает её запущенной. Поэтому
/// звено подменяется прямо в лаунчере: игру запускает он сам, своей же
/// командной строкой, и своим потомком видит настоящую GTA5.exe.
///
/// Владеет общим блоком, через который разговаривает с внедрённым модулем, и
/// живёт, пока идёт запуск.
class LauncherPatch {
public:
    ~LauncherPatch();

    LauncherPatch(const LauncherPatch&) = delete;
    LauncherPatch& operator=(const LauncherPatch&) = delete;

    /// Что подмене делать с ближайшим запуском игры.
    struct Order {
        /// Куда внедрённый модуль пишет свой журнал.
        ///
        /// Передаётся ему до внедрения: изнутри чужого процесса взять этот путь
        /// неоткуда.
        std::filesystem::path logDirectory;

        /// Добавить ли игре ключ входа сразу в сетевой свободный режим.
        bool straightIntoFreemode = false;

        /// На каком языке запускать игру: `ru-RU`, `en-US` и так далее.
        ///
        /// Пусто — как решит лаунчер Rockstar. Язык называется здесь, потому что
        /// внутри сессии игра сменить его не даёт: меню паузы там сетевое, и
        /// выбранную строку оно возвращает обратно.
        std::wstring gameLanguage;
    };

    /// Внедряет подмену в работающий Rockstar Games Launcher и дожидается её
    /// готовности.
    ///
    /// Лаунчер должен быть запущен: поднимать его — не наше дело, этим
    /// занимается ensureRockstarLauncherReady.
    [[nodiscard]] static std::unique_ptr<LauncherPatch> install(const std::filesystem::path& module,
                                                                const Order& order,
                                                                std::chrono::seconds timeout,
                                                                std::string& error);

    /// Ждёт, пока лаунчер запустит игру, и отдаёт номер её процесса.
    ///
    /// Ноль означает, что игра так и не появилась; причина — в error.
    [[nodiscard]] std::uint32_t waitForGame(std::chrono::seconds timeout, std::string& error);

private:
    LauncherPatch() = default;

    void* mapping_ = nullptr;
    void* handoff_ = nullptr;
};

} // namespace oxymp::launcher
