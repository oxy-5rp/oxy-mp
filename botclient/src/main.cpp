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
#include <thread>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

/// Круг, по которому ходит бот.
///
/// Середина взята там же, где появляется игрок, — у терминала аэропорта
/// Лос-Сантоса. Иначе бота не увидеть: клиент показывает чужих игроков
/// настоящими персонажами, а персонаж за километр от нас существует только в
/// числах.
oxymp::shared::Vec3 g_circleCentre{-1037.7F, -2738.0F, 20.2F};
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
                 "                  [--seconds <сколько работать>] [--offset <метров>]\n"
                 "                  [--bots <сколько>] [--spread <метров>]\n\n"
                 "  --offset  на сколько метров к востоку отнести круг, по которому ходит\n"
                 "            бот. Нужно, чтобы разводить ботов по карте: сбившиеся в одну\n"
                 "            точку, они не покажут, во что обходится сессия, где люди\n"
                 "            разошлись.\n"
                 "  --bots    сколько соединений вести из одного процесса. Двести отдельных\n"
                 "            процессов машина поднимет, но мерить ими нагрузку сервера\n"
                 "            бессмысленно: половина её уйдёт на сами процессы.\n"
                 "  --spread  сторона квадрата, по которому разводятся круги ботов. Ноль\n"
                 "            означает, что все ходят вокруг одной точки: так проверяется\n"
                 "            худший случай, когда каждый видит каждого.\n";
}

/// Один бот: соединение и круг, по которому он ходит.
///
/// Круг у каждого свой, и в этом весь смысл разведения. Сбившись в одну точку,
/// боты показывают худший случай — каждый видит каждого; разойдясь по карте,
/// показывают обычный, ради которого рассылка и раскладывается по расстояниям.
struct Bot {
    std::unique_ptr<oxymp::client::Connection> connection;
    oxymp::shared::Vec3 centre;
    bool everConnected = false;
};

/// Раскладывает номер бота по клеткам квадрата со стороной side.
///
/// Решёткой, а не вразнобой: случайная раскладка даёт разные замеры на разных
/// запусках, а сравнивать приходится замер с замером.
oxymp::shared::Vec3 spreadCentre(const oxymp::shared::Vec3& origin, unsigned int index,
                                 unsigned int total, float side) {
    if (side <= 0.0F || total <= 1) {
        return origin;
    }

    // Сторона решётки — корень из числа ботов, округлённый вверх: так клетки
    // выходят близкими к квадратным при любом их числе.
    auto columns = static_cast<unsigned int>(std::ceil(std::sqrt(static_cast<double>(total))));
    if (columns == 0) {
        columns = 1;
    }

    const float step = side / static_cast<float>(columns);
    const auto column = static_cast<float>(index % columns);
    const auto row = static_cast<float>(index / columns);
    const auto half = static_cast<float>(columns) / 2.0F;

    return oxymp::shared::Vec3{
        origin.x + (column - half) * step,
        origin.y + (row - half) * step,
        origin.z,
    };
}

template<typename T>
bool parseNumber(std::string_view text, T& value) {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);

    return result.ec == std::errc{} && result.ptr == end;
}

/// Доводы командной строки в UTF-8.
///
/// Обычный argv на Windows приходит в кодовой странице консоли, а не в UTF-8:
/// имя игрока кириллицей превращалось в вопросительные знаки ещё до отправки, и
/// сервер получал их как есть. Проверено на живой игре — над головой персонажа
/// стояло «?????».
///
/// Поэтому строка запуска берётся широкой и переводится сама. На остальных
/// платформах argv уже в UTF-8, и переводить нечего.
[[nodiscard]] std::vector<std::string> arguments(int argc, char** argv) {
    std::vector<std::string> collected;

#ifdef _WIN32
    (void)argc;
    (void)argv;

    int count = 0;
    wchar_t** wide = ::CommandLineToArgvW(::GetCommandLineW(), &count);

    if (wide == nullptr) {
        return collected;
    }

    collected.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr,
                                               nullptr);
        if (size <= 1) {
            collected.emplace_back();
            continue;
        }

        // Минус один: размер посчитан вместе с завершающим нулём, а строке он не
        // нужен — она знает свою длину сама.
        std::string value(static_cast<std::size_t>(size - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), size, nullptr, nullptr);

        collected.push_back(std::move(value));
    }

    ::LocalFree(wide);
#else
    collected.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        collected.emplace_back(argv[i]);
    }
#endif

    return collected;
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

    // Сколько соединений вести и на какой квадрат их разложить.
    unsigned int bots = 1;
    float spread = 0.0F;

    const std::vector<std::string> args = arguments(argc, argv);

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view argument = args[i];
        const bool hasValue = i + 1 < args.size();

        if (argument == "--address" && hasValue) {
            settings.address = args[++i];
        } else if (argument == "--port" && hasValue) {
            if (!parseNumber(args[++i], settings.port)) {
                printUsage();
                return 2;
            }
        } else if (argument == "--nickname" && hasValue) {
            settings.nickname = args[++i];
        } else if (argument == "--offset" && hasValue) {
            unsigned int offset = 0;
            if (!parseNumber(args[++i], offset)) {
                std::cerr << "смещение должно быть числом\n";
                return 2;
            }

            g_circleCentre.x += static_cast<float>(offset);
        } else if (argument == "--seconds" && hasValue) {
            if (!parseNumber(args[++i], seconds)) {
                printUsage();
                return 2;
            }
        } else if (argument == "--bots" && hasValue) {
            if (!parseNumber(args[++i], bots) || bots == 0) {
                std::cerr << "ботов должно быть числом, и не меньше одного\n";
                return 2;
            }
        } else if (argument == "--spread" && hasValue) {
            unsigned int metres = 0;
            if (!parseNumber(args[++i], metres)) {
                std::cerr << "разведение должно быть числом\n";
                return 2;
            }

            spread = static_cast<float>(metres);
        } else {
            printUsage();
            return 2;
        }
    }

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::debug);

    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);

    // Соединения заводятся все разом, но представляются серверу по мере того,
    // как транспорт их установит: рукопожатие идёт своим чередом внутри
    // Connection, и торопить его отсюда нечем.
    std::vector<Bot> herd;
    herd.reserve(bots);

    for (unsigned int i = 0; i < bots; ++i) {
        oxymp::client::Connection::Settings own = settings;

        // Имя должно быть своим у каждого: сервер отказывает второму с тем же
        // именем, и вся стая, кроме первого, осталась бы за дверью. Одиночному
        // боту имя не трогаем — под ним его и ищут в журнале сервера.
        if (bots > 1) {
            own.nickname = settings.nickname + std::to_string(i);
        }

        herd.push_back(Bot{
            .connection = std::make_unique<oxymp::client::Connection>(std::move(own)),
            .centre = spreadCentre(g_circleCentre, i, bots, spread),
        });
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds{seconds};

    auto lastReport = std::chrono::steady_clock::time_point{};

    // Чем кончилась связь в прошлый раз, когда мы смотрели. Нужно, чтобы писать
    // о разрыве один раз, а не двадцать в секунду: признак держится до тех пор,
    // пока сервер не примет нас заново. Ведётся только для одиночного бота: у
    // стаи разрывы считаются числом, а не пересказываются поимённо.
    auto lastDisconnect = oxymp::client::DisconnectReason::None;

    while (!g_stopRequested.load()) {
        // Ожидание событий — ноль, а не двадцать миллисекунд, и это не мелочь.
        // Ждать внутри каждого соединения означало бы двадцать миллисекунд,
        // умноженные на число ботов: стая из двухсот обошла бы круг за четыре
        // секунды и не прислала бы за это время ни одного снимка. Ждём один раз
        // за оборот и снаружи.
        const auto budget = std::chrono::milliseconds{bots == 1 ? 20 : 0};

        const float elapsed =
            std::chrono::duration<float>{std::chrono::steady_clock::now() - started}.count();

        for (Bot& bot : herd) {
            oxymp::client::Connection& connection = *bot.connection;

            connection.update(budget);

            // То же самое, что в игре показывается окном во весь экран. Здесь —
            // строкой в журнале: другого экрана у бота нет, а проверять разрыв
            // на нём удобнее, чем в игре.
            if (bots == 1) {
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
            }

            if (connection.state() == oxymp::client::ConnectionState::Connected) {
                bot.everConnected = true;

                // Бот ходит по кругу. Движение нужно настоящее: на неподвижном
                // игроке ни интерполяция, ни экстраполяция себя не проявят.
                const float angle = elapsed * kAngularSpeed;

                oxymp::shared::PlayerState state;
                state.position =
                    oxymp::shared::Vec3{bot.centre.x + kCircleRadius * std::cos(angle),
                                        bot.centre.y + kCircleRadius * std::sin(angle),
                                        bot.centre.z};
                state.velocity =
                    oxymp::shared::Vec3{-kCircleRadius * kAngularSpeed * std::sin(angle),
                                        kCircleRadius * kAngularSpeed * std::cos(angle), 0.0F};
                state.heading = angle * 180.0F / 3.14159265F;
                state.health = 200;

                connection.setLocalState(state);
            }
        }

        // Стая ждёт один раз за оборот, а не в каждом соединении. Двадцать
        // миллисекунд — тот же такт, с которым снимки уходят у настоящего
        // клиента.
        if (bots > 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }

        const auto now = std::chrono::steady_clock::now();

        if (now - lastReport >= std::chrono::seconds{2}) {
            lastReport = now;

            if (bots == 1) {
                const oxymp::client::Connection& connection = *herd.front().connection;
                const auto latency = connection.latency();

                spdlog::info("состояние: {}, id {}, игроков рядом {}, задержка {}",
                             oxymp::client::describe(connection.state()),
                             connection.localPlayerId(), connection.remotePlayers().size(),
                             latency ? std::to_string(latency->count()) + " мс"
                                     : std::string{"не измерена"});

                // Главное доказательство работы мультиплеера: мы видим, где
                // сейчас находятся другие игроки, и их положение меняется.
                for (const auto& [id, player] : connection.remotePlayers()) {
                    const auto state = player.at(now);
                    spdlog::info(
                        "  игрок \"{}\" (id {}) в точке {:.1f} {:.1f} {:.1f}, поворот {:.0f}",
                        player.nickname.empty() ? "?" : player.nickname, id, state.position.x,
                        state.position.y, state.position.z, state.heading);
                }
            } else {
                // У стаи поимённого пересказа нет: двести строк в секунду не
                // читает никто. Считается то, ради чего стая и заведена, —
                // сколько соединений держится и сколько соседей видит каждый.
                std::size_t connected = 0;
                std::size_t neighbours = 0;
                std::size_t latencySum = 0;
                std::size_t measured = 0;

                for (const Bot& bot : herd) {
                    if (bot.connection->state() != oxymp::client::ConnectionState::Connected) {
                        continue;
                    }

                    ++connected;
                    neighbours += bot.connection->remotePlayers().size();

                    if (const auto latency = bot.connection->latency()) {
                        latencySum += static_cast<std::size_t>(latency->count());
                        ++measured;
                    }
                }

                spdlog::info(
                    "в сессии {} из {}, соседей у каждого в среднем {:.1f}, задержка {}",
                    connected, bots,
                    connected == 0
                        ? 0.0
                        : static_cast<double>(neighbours) / static_cast<double>(connected),
                    measured == 0 ? std::string{"не измерена"}
                                  : std::to_string(latencySum / measured) + " мс");
            }
        }

        if (seconds != 0 && now >= deadline) {
            break;
        }
    }

    std::size_t everConnected = 0;

    for (Bot& bot : herd) {
        bot.connection->disconnect();

        if (bot.everConnected) {
            ++everConnected;
        }
    }

    if (everConnected == 0) {
        spdlog::error("завершение: подключиться так и не удалось");
        return 1;
    }

    if (everConnected < bots) {
        spdlog::error("завершение: в сессию попали {} из {}", everConnected, bots);
        return 1;
    }

    spdlog::info("завершение: сессия отработала штатно");
    return 0;
}
