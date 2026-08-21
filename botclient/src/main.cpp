// oxymp-botclient — клиент без игры.
//
// Линкует то же клиентское ядро, что и модуль внутри GTA, но работает в консоли.
// Благодаря этому протокол и сеть отлаживаются и проверяются на регрессии без
// запуска игры, а позже он же изображает второго игрока.

#include <oxymp/client/connection.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <set>
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
/// Что сервер нарисовал этому боту.
///
/// Считается, а не показывается: рисовать боту нечем — игры у него нет. Но
/// узнать, дошло ли нарисованное, можно только здесь: в игре его видно глазами,
/// а глаз у проверки на регрессии не бывает.
///
/// Забирать пришедшее нужно в любом случае, даже не считая: непрочитанное
/// копится в соединении до конца сессии.
struct Drawn {
    std::size_t blips = 0;
    std::size_t markers = 0;
    std::size_t checkpoints = 0;

    /// Сколько движений велел сыграть сервер. Играть их боту нечем — тела у
    /// него нет, — но по счётчику видно, что распоряжение дошло.
    std::size_t animations = 0;

    /// Сколько раз по боту попали и сколько у него здоровья по мнению сервера.
    ///
    /// Здоровье принадлежит серверу целиком, и бот его не ведёт: он и не может
    /// — стреляют в него, а не он. По этим двум числам и видно, что попадания
    /// доходят до сервера и что он их считает.
    std::size_t hits = 0;
    std::uint16_t health = 200;
    std::uint16_t armour = 0;

    /// Сколько раз пришла внешность машины и сколько из них несли тюнинг.
    ///
    /// Второе число здесь и есть проверка обвесов: заводскую внешность машина
    /// объявляет и сама, а места тюнинга заводскими не бывают — их назначает
    /// только сервер.
    std::size_t appearances = 0;
    std::size_t tuned = 0;

    /// Сколько привязок сервер объявил и сколько из них — отвязки.
    ///
    /// Отвязка приходит тем же сообщением с пустой целью: получателю важно не
    /// «убери привязку», а «вот как эта сущность привязана теперь».
    std::size_t attachments = 0;
    std::size_t detachments = 0;

    /// Сколько прохожих сервер объявил и сколько сейчас на виду.
    ///
    /// Второе число меньше первого, когда куклу поправили: заведение и правка
    /// приходят одним сообщением, и первое число считает оба.
    std::size_t pedMessages = 0;
    std::set<oxymp::shared::PedId> peds;

    void collect(oxymp::client::Connection& connection) {
        blips += connection.takeBlips().size();
        markers += connection.takeMarkers().size();
        checkpoints += connection.takeCheckpoints().size();
        animations += connection.takeAnimations().size();
        hits += connection.takeDamage().size();

        if (const auto changed = connection.takeHealth()) {
            health = changed->health;
            armour = changed->armour;
        }

        for (const auto& ped : connection.takePeds()) {
            ++pedMessages;
            peds.insert(ped.id);
        }

        for (const oxymp::shared::PedId id : connection.takeRemovedPeds()) {
            peds.erase(id);
        }

        for (const auto& attachment : connection.takeAttachments()) {
            ++attachments;

            if (attachment.targetKind == oxymp::shared::EntityKind::None) {
                ++detachments;
            }
        }

        for (const auto& appearance : connection.takeVehicleAppearances()) {
            ++appearances;

            const bool anyMod = std::ranges::any_of(appearance.mods, [](std::int8_t mod) {
                return mod != oxymp::shared::kStockMod;
            });

            if (anyMod || appearance.toggleMods != 0) {
                ++tuned;
            }
        }

        // Снятое вычитается: метка, поставленная и убранная, у игрока не
        // осталась бы, и счётчик, который об этом не знает, врёт.
        blips -= std::min(blips, connection.takeRemovedBlips().size());
        markers -= std::min(markers, connection.takeRemovedMarkers().size());
        checkpoints -= std::min(checkpoints, connection.takeRemovedCheckpoints().size());
    }
};

struct Bot {
    std::unique_ptr<oxymp::client::Connection> connection;
    oxymp::shared::Vec3 centre;
    bool everConnected = false;
    Drawn drawn;
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

            bot.drawn.collect(connection);

            if (connection.state() == oxymp::client::ConnectionState::Connected) {
                if (!bot.everConnected) {
                    // Клиент говорит серверу, что поднял свою половину ресурсов,
                    // и до этого слова сервер держит вход: обработчик
                    // playerConnect не объявляется, метки и фигуры не уходят.
                    //
                    // Боту поднимать нечего — игры у него нет, — и потому он
                    // готов сразу. Без этой строки он был бы игроком наполовину:
                    // в списке сессии есть, а для режима не входил.
                    connection.emit(std::string{oxymp::shared::kClientReadyEvent}, {});
                }

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

                // Куда бот смотрит. Заполнять обязательно: незаполненная точка
                // уходит нулевой, и кукла бота уставилась бы в начало
                // координат — то есть в море под Лос-Сантосом.
                //
                // Смотрит он в середину своего круга, а не перед собой, и это
                // не прихоть: перед собой смотрит и без того всякий идущий, и
                // по такому взгляду не отличить работающий поворот головы от
                // неработающего. А обходя круг с повёрнутой к середине головой,
                // кукла показывает это сразу.
                state.aimAt = oxymp::shared::Vec3{bot.centre.x, bot.centre.y,
                                                  bot.centre.z + oxymp::shared::kLookHeight};

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

                const Drawn& drawn = herd.front().drawn;

                spdlog::info("  сервер нарисовал: меток {}, маркеров {}, точек {}; "
                             "движений велел {}",
                             drawn.blips, drawn.markers, drawn.checkpoints, drawn.animations);

                spdlog::info("  попаданий по нам {}, здоровье {}, броня {}", drawn.hits,
                             drawn.health, drawn.armour);

                // Машины пересказываются поимённо, а не числом: главное здесь
                // не сколько их, а движутся ли они. Стоящая машина снимков не
                // шлёт вовсе, и «ведёт никто» — её обычное состояние; а вот у
                // машины под игроком положение обязано меняться от строки к
                // строке. Без этого проверить сеть машин было нечем: в игре
                // видно чужую машину, но не видно, откуда она там взялась.
                for (const auto& [id, vehicle] : connection.vehicles()) {
                    const auto state = vehicle.at(now);

                    spdlog::info("  машина {} в точке {:.1f} {:.1f} {:.1f}, ведёт {}, "
                                 "снимков {}",
                                 id, state.position.x, state.position.y, state.position.z,
                                 vehicle.owner == oxymp::shared::kInvalidPlayerId
                                     ? std::string{"никто"}
                                     : std::to_string(vehicle.owner),
                                 vehicle.snapshots);
                }

                spdlog::info("  внешностей машин {}, из них с тюнингом {}", drawn.appearances,
                             drawn.tuned);

                spdlog::info("  привязок {}, из них отвязок {}", drawn.attachments,
                             drawn.detachments);

                spdlog::info("  о прохожих сказано {} раз, на виду {}", drawn.pedMessages,
                             drawn.peds.size());

                // Главное доказательство работы мультиплеера: мы видим, где
                // сейчас находятся другие игроки, и их положение меняется.
                for (const auto& [id, player] : connection.remotePlayers()) {
                    const auto state = player.at(now);
                    // Расстояние до точки взгляда, а не сама точка: у неё три
                    // числа и никакого смысла по отдельности, а вот её удаление
                    // говорит всё сразу. Двадцать метров — взгляд, сто —
                    // прицел, ноль — клиент точку не заполняет вовсе, и тогда
                    // кукла уставилась бы в начало координат.
                    const float looks = std::sqrt(
                        oxymp::shared::distanceSquared(state.position, state.aimAt));

                    spdlog::info(
                        "  игрок \"{}\" (id {}) в точке {:.1f} {:.1f} {:.1f}, поворот {:.0f}, "
                        "смотрит на {:.0f} м",
                        player.nickname.empty() ? "?" : player.nickname, id, state.position.x,
                        state.position.y, state.position.z, state.heading, looks);
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
