#include "net_session.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

constexpr std::string_view kHostId = "session_host";
constexpr std::string_view kIsHostId = "session_is_host";
constexpr std::string_view kManagerId = "network_manager";

/// Видимость поднимаемой сессии.
///
/// Ноль — закрытая, никому не объявляемая. Другого нам и не надо: игроков
/// приводит наш сервер, а не список сессий Rockstar, и объявлять себя там
/// изменённому клиенту незачем.
constexpr int kPrivateVisibility = 0;

/// Предел игроков в сессии.
///
/// Тридцать два — столько же, сколько в обычной сессии GTA Online. Взято ровно
/// поэтому: значение, с которым игра заведомо умеет работать, а подбирать своё,
/// не зная её пределов, — лишний повод для отказа.
constexpr int kMaxPlayers = 32;

/// Признаки поднятия. Ноль — без особых условий.
constexpr int kPlainFlags = 0;

/// Сколько раз поднимать сессию заново, прежде чем признать затею бесплодной.
///
/// Предел обязателен: без него мы получили бы бесконечное дёрганье машины
/// состояний игры. Пять, а не сорок: сорок были рассчитаны на сессию, живущую
/// доли секунды, — а такой она была ровно потому, что её и дёргали.
constexpr std::uint32_t kMaxAttempts = 5;

/// Сколько времени давать игре на поднятие сессии, прежде чем просить заново.
///
/// Замеряно на живой игре: от просьбы до выставленного признака проходит около
/// семисот миллисекунд. Пятнадцать секунд — с запасом на медленную машину и на
/// то, что первые просьбы приходятся на ещё не готовый распорядитель сети.
constexpr auto kStartupGrace = std::chrono::seconds{15};

} // namespace

NetSession::NetSession(const EngineAddresses& addresses, const NativeTable& table) noexcept
    : host_(addresses.pointerTo<HostFunction>(kHostId)),
      isHost_(addresses.pointerTo<IsHostFunction>(kIsHostId)),
      hostSolo_(table.handlerFor(natives::kNetworkSessionHostSinglePlayer)),
      leaveSolo_(table.handlerFor(natives::kNetworkSessionLeaveSinglePlayer)),
      manager_(addresses.pointerTo<void* const*>(kManagerId)) {}

bool NetSession::ready() const noexcept {
    return (host_ != nullptr && manager_ != nullptr) || hostSolo_ != nullptr;
}

bool NetSession::managerReady() const {
    return manager_ != nullptr && *manager_ != nullptr;
}

bool NetSession::isHost() const {
    return isHost_ != nullptr && isHost_();
}

void NetSession::rehostIfDropped(Mode mode, bool sessionStarted) {
    if (sessionStarted) {
        everStarted_ = true;
        return;
    }

    if (!asked_ || attempts_ >= kMaxAttempts) {
        return;
    }

    // Сессии нет — но это ещё ничего не значит.
    //
    // Пока она ни разу не вставала и время на поднятие не вышло, её отсутствие
    // означает «поднимается», а не «упала». Здесь и была ошибка, стоившая игре
    // краха: просьба поднять сессию, пришедшая в уже поднимающуюся, ломает её.
    if (!everStarted_ && std::chrono::steady_clock::now() - askedAt_ < kStartupGrace) {
        return;
    }

    spdlog::warn("сессии нет {}: просим заново",
                 everStarted_ ? "после того, как она встала" : "и время вышло");

    asked_ = false;
    host(mode);
}

bool NetSession::host(Mode mode) {
    if (asked_) {
        return true;
    }

    ++attempts_;

    if (mode == Mode::Solo) {
        if (hostSolo_ == nullptr) {
            spdlog::error("натив одиночной сессии не найден");
            return false;
        }

        asked_ = true;
        askedAt_ = std::chrono::steady_clock::now();

        spdlog::debug("просим игру поднять одиночную сетевую сессию (попытка {})", attempts_);

        // Довод — режим сессии. Ноль означает обычный, без особых условий:
        // тот же, которым игра пользуется сама, уводя игрока в сессию на одного.
        invokeNative<void>(hostSolo_, 0);

        spdlog::debug("вызов одиночной сессии вернул управление");
        return true;
    }

    if (!ready()) {
        spdlog::error("поднять сессию нечем: разведочные сигнатуры не разрешились");
        return false;
    }

    // Распорядитель сети проверяется до вызова, а не после отказа. Он заводится
    // по ходу запуска игры, и попроси мы раньше — просьба ушла бы в никуда, а
    // выглядело бы это как «функция не работает».
    if (!managerReady()) {
        spdlog::debug("распорядителя сети ещё нет — поднимать сессию рано");
        return false;
    }

    asked_ = true;
    askedAt_ = std::chrono::steady_clock::now();

    spdlog::debug("просим игру поднять сессию: видимость {}, до {} игроков, признаки {}",
                 kPrivateVisibility, kMaxPlayers, kPlainFlags);

    // Дальше — вызов вслепую. Что игра сделает без живого слоя Rockstar Online,
    // заранее не известно, и если этой записи в журнале не последует ничего, то
    // ответ — «повисла внутри».
    host_(kPrivateVisibility, kMaxPlayers, kPlainFlags);

    spdlog::debug("вызов поднятия сессии вернул управление");

    return true;
}

} // namespace oxymp::client::game
