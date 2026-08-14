#include "world_clock.hpp"

#include <algorithm>
#include <cmath>

namespace oxymp::server {
namespace {

/// Сколько секунд в сутках и в часе.
constexpr double kSecondsPerDay = 24.0 * 60.0 * 60.0;
constexpr double kSecondsPerHour = 60.0 * 60.0;
constexpr double kSecondsPerMinute = 60.0;

/// Годится ли название погоды для отправки в игру.
///
/// Названия у игры заглавными латинскими буквами и без пробелов: EXTRASUNNY,
/// THUNDER, SNOWLIGHT. Всё остальное отвергается здесь, а не в игре: строка,
/// собранная из чего угодно, уходит в нативы, и лучше ей туда не попадать.
[[nodiscard]] bool plausibleWeather(std::string_view weather) {
    if (weather.empty() || weather.size() > shared::kMaxWeatherLength) {
        return false;
    }

    return std::ranges::all_of(weather, [](char symbol) {
        return (symbol >= 'A' && symbol <= 'Z') || (symbol >= '0' && symbol <= '9') ||
               symbol == '_';
    });
}

} // namespace

WorldClock::WorldClock(std::string weather, std::uint8_t hour, std::uint8_t minute)
    : weather_(std::move(weather)),
      advancedAt_(Clock::now()),
      broadcastAt_(Clock::now()) {
    secondsOfDay_ = (static_cast<double>(hour) * kSecondsPerHour) +
                    (static_cast<double>(minute) * kSecondsPerMinute);
}

bool WorldClock::advance() {
    const auto now = Clock::now();

    const double elapsed = std::chrono::duration<double>{now - advancedAt_}.count();
    advancedAt_ = now;

    // Игровых секунд за настоящую: темп задан в минутах, потому что так о нём
    // привычнее думать, а считается в секундах — часы хранятся в них.
    secondsOfDay_ += elapsed * kGameMinutesPerSecond * kSecondsPerMinute;
    secondsOfDay_ = std::fmod(secondsOfDay_, kSecondsPerDay);

    if (now - broadcastAt_ < kBroadcastInterval) {
        return false;
    }

    broadcastAt_ = now;
    return true;
}

bool WorldClock::setWeather(std::string weather) {
    if (!plausibleWeather(weather)) {
        return false;
    }

    weather_ = std::move(weather);
    return true;
}

bool WorldClock::setTime(std::uint8_t hour, std::uint8_t minute, std::uint8_t second) {
    if (hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    secondsOfDay_ = (static_cast<double>(hour) * kSecondsPerHour) +
                    (static_cast<double>(minute) * kSecondsPerMinute) +
                    static_cast<double>(second);

    // Разослать немедленно: перевод часов виден игроку сразу, и ждать до
    // очередной рассылки — значит показать ему, что распоряжение не сработало.
    broadcastAt_ = Clock::time_point{};

    return true;
}

shared::WorldState WorldClock::snapshot() const {
    const auto total = static_cast<int>(secondsOfDay_);

    shared::WorldState state;
    state.weather = weather_;
    state.hour = static_cast<std::uint8_t>(total / static_cast<int>(kSecondsPerHour));
    state.minute = static_cast<std::uint8_t>((total / 60) % 60);
    state.second = static_cast<std::uint8_t>(total % 60);

    return state;
}

} // namespace oxymp::server
