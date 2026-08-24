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

        /// Писать ли в журнал подробности. Настройка `debug` из `oxymp.toml`.
        bool verbose = false;

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

    /// Номер процесса игры, если запустил её лаунчер Rockstar.
    ///
    /// Ноль означает, что перехват не сработал ни разу, — и это не поломка, а
    /// обычное дело у копии из Steam или Epic: там игру создаёт клиент площадки,
    /// а не лаунчер Rockstar, и наш перехват внутри Launcher.exe стоит в стороне.
    ///
    /// Ждать по нему игру нельзя — именно на этом ожидании oxyMP и застревал на
    /// три минуты у человека с копией из Epic. Процесс разыскивается по имени;
    /// здесь же остаётся одна-единственная надобность, зато настоящая: по
    /// журналу видно, сработал перехват или нет.
    [[nodiscard]] std::uint32_t startedGame() const;

private:
    LauncherPatch() = default;

    void* mapping_ = nullptr;
    void* handoff_ = nullptr;
};

} // namespace oxymp::launcher
