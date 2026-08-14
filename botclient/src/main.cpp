// oxymp-botclient — клиент без игры.
//
// Линкует то же клиентское ядро, что и модуль внутри GTA, но работает в консоли.
// Благодаря этому протокол и сеть отлаживаются и проверяются на регрессии без
// запуска игры, а позже он же изображает второго игрока.

#include <oxymp/client/connection.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/// Круг, по которому ходит бот.
///
/// Середина взята там же, где появляется игрок, — у терминала аэропорта
/// Лос-Сантоса. Иначе бота не увидеть: клиент показывает чужих игроков
/// настоящими персонажами, а персонаж за километр от нас существует только в
/// числах.
constexpr oxymp::shared::Vec3 kCircleCentre{-1037.7F, -2738.0F, 20.2F};
constexpr float kCircleRadius = 12.0F;

/// Угловая скорость в радианах в секунду: полный круг примерно за 12 секунд.
constexpr float kAngularSpeed = 0.5F;

std::atomic<bool> g_stopRequested{false};

extern "C" void onInterrupt(int) {
    g_stopRequested.store(true);
}

void printUsage() {
    std::cerr << "Использование:\n"
                 "  oxymp-botclient [--address <адрес>] [--port <номер>] [--nickname <имя>]\n"
                 "                  [--seconds <сколько работать>]\n";
}

template<typename T>
bool parseNumber(std::string_view text, T& value) {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);

    return result.ec == std::errc{} && result.ptr == end;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    oxymp::client::Connection::Settings settings;
    settings.nickname = "bot";

    // Ноль означает «работать, пока не остановят».
    unsigned int seconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const bool hasValue = i + 1 < argc;

        if (argument == "--address" && hasValue) {
            settings.address = argv[++i];
        } else if (argument == "--port" && hasValue) {
            if (!parseNumber(argv[++i], settings.port)) {
                printUsage();
                return 2;
            }
        } else if (argument == "--nickname" && hasValue) {
            settings.nickname = argv[++i];
        } else if (argument == "--seconds" && hasValue) {
            if (!parseNumber(argv[++i], seconds)) {
                printUsage();
                return 2;
            }
        } else {
            printUsage();
            return 2;
        }
    }

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::debug);

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);

    oxymp::client::Connection connection{settings};

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds{seconds};

    auto lastReport = std::chrono::steady_clock::time_point{};
    bool everConnected = false;

    // Чем кончилась связь в прошлый раз, когда мы смотрели. Нужно, чтобы писать
    // о разрыве один раз, а не двадцать в секунду: признак держится до тех пор,
    // пока сервер не примет нас заново.
    auto lastDisconnect = oxymp::client::DisconnectReason::None;

    while (!g_stopRequested.load()) {
        connection.update(std::chrono::milliseconds{20});

        // То же самое, что в игре показывается окном во весь экран. Здесь —
        // строкой в журнале: другого экрана у бота нет, а проверять разрыв на
        // нём удобнее, чем в игре.
        if (const auto lost = connection.disconnectReason(); lost != lastDisconnect) {
            lastDisconnect = lost;

            switch (lost) {
            case oxymp::client::DisconnectReason::None:
                spdlog::info("разрыв: связь восстановлена, снова в сессии");
                break;
            case oxymp::client::DisconnectReason::Lost:
                spdlog::error("разрыв: связь с сервером потеряна");
                break;
            case oxymp::client::DisconnectReason::Refused:
                spdlog::error("разрыв: сервер отказал — {}",
                              connection.rejectReason()
                                  ? oxymp::client::describe(*connection.rejectReason())
                                  : std::string_view{"без объяснения"});
                break;
            }
        }

        if (connection.state() == oxymp::client::ConnectionState::Connected) {
            everConnected = true;

            // Бот ходит по кругу. Движение нужно настоящее: на неподвижном
            // игроке ни интерполяция, ни экстраполяция себя не проявят.
            const float elapsed =
                std::chrono::duration<float>{std::chrono::steady_clock::now() - started}.count();
            const float angle = elapsed * kAngularSpeed;

            oxymp::shared::PlayerState state;
            state.position =
                oxymp::shared::Vec3{kCircleCentre.x + kCircleRadius * std::cos(angle),
                                    kCircleCentre.y + kCircleRadius * std::sin(angle),
                                    kCircleCentre.z};
            state.velocity = oxymp::shared::Vec3{-kCircleRadius * kAngularSpeed * std::sin(angle),
                                                 kCircleRadius * kAngularSpeed * std::cos(angle),
                                                 0.0F};
            state.heading = angle * 180.0F / 3.14159265F;
            state.health = 200;

            connection.setLocalState(state);
        }

        if (connection.state() == oxymp::client::ConnectionState::Rejected) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();

        if (now - lastReport >= std::chrono::seconds{2}) {
            lastReport = now;

            const auto latency = connection.latency();
            spdlog::info("состояние: {}, id {}, игроков рядом {}, задержка {}",
                         oxymp::client::describe(connection.state()), connection.localPlayerId(),
                         connection.remotePlayers().size(),
                         latency ? std::to_string(latency->count()) + " мс"
                                 : std::string{"не измерена"});

            // Главное доказательство работы мультиплеера: мы видим, где сейчас
            // находятся другие игроки, и их положение меняется.
            for (const auto& [id, player] : connection.remotePlayers()) {
                const auto state = player.at(now);
                spdlog::info("  игрок \"{}\" (id {}) в точке {:.1f} {:.1f} {:.1f}, поворот {:.0f}",
                             player.nickname.empty() ? "?" : player.nickname, id, state.position.x,
                             state.position.y, state.position.z, state.heading);
            }
        }

        if (seconds != 0 && now >= deadline) {
            break;
        }
    }

    connection.disconnect();

    if (connection.rejectReason()) {
        spdlog::error("завершение: сервер отказал");
        return 1;
    }

    if (!everConnected) {
        spdlog::error("завершение: подключиться так и не удалось");
        return 1;
    }

    spdlog::info("завершение: сессия отработала штатно");
    return 0;
}
