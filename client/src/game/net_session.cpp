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

/// Сколько раз поднимать сессию, прежде чем признать затею бесплодной.
///
/// Две: одна попытка и одна повторная. Больше не имеет смысла — если игра вышла
/// из сессии дважды, она выйдет и в третий раз, а каждый подъём это мигание
/// признака сетевой игры и всего, что от него зависит. Лучше остаться без
/// сессии, чем в мигающей.
constexpr std::uint32_t kMaxAttempts = 2;

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

        // Счёт попыток обнуляется удавшейся: предел стоит против попыток,
        // идущих одна за другой без толку, а не против самих подъёмов.
        //
        // Подъёмов теперь много и они ожидаемы: дверь выхода из сессии
        // пропускает один вызов раз в несколько минут — иначе игра умирает от
        // запертой навсегда, — и каждый пропуск роняет сессию. Не обнуляй мы
        // счёт, сессия поднялась бы дважды и на третий раз осталась бы лежать.
        attempts_ = 0;
        gaveUpReported_ = false;
        return;
    }

    if (!asked_ || attempts_ >= kMaxAttempts) {
        // Отказ объявляется не тогда, когда кончились попытки, а когда кончилась
        // выдержка после последней из них. Разница стоила сессии целиком.
        //
        // На отказ смотрит удержание в сессии (`GameSession::holdSession`):
        // отказались — отпускаем дверь. Объявленный же в тот самый кадр, в
        // котором сделана последняя попытка, отказ снимал удержание с ещё
        // поднимающейся сессии: она встаёт около секунды, а удержание уходило
        // через шестнадцать миллисекунд. Дальше игра выходила из сессии
        // беспрепятственно, и вторая попытка не могла удаться никогда — сессия
        // жила ровно до первого своего мигания и больше не возвращалась.
        //
        // Уровень здесь `debug`, а не `warning`, и прежняя строка вдобавок
        // говорила неправду: «мир будет вести себя как в одиночной игре».
        //
        // Не будет. Сессионным мир держит не сетевая сессия, а подавление
        // (`world_.suppressPopulation`, `story_`, `respawn_`), и оно работает
        // каждый кадр независимо от неё: сюжет погашен, телефон погашен,
        // прохожие и уличные машины не заводятся. Проверено живой игрой — мир,
        // потерявший сессию, ничем не отличается от мира с ней.
        //
        // Сама сессия нам и не нужна: за ней шли ради поведения, а поведение
        // получено иначе. Оставлять же в журнале пугало о том, чего не
        // происходит, нельзя — по нему станут искать беду там, где её нет.
        //
        // Один раз сказать и замолчать: повторять это каждый кадр незачем.
        if (asked_ && everStarted_ && !gaveUpReported_ &&
            std::chrono::steady_clock::now() - askedAt_ >= kStartupGrace) {
            gaveUpReported_ = true;

            spdlog::debug("the game dropped the network session and did not take it back; "
                          "the world stays session-like on its own");
        }
        return;
    }

    // Выдержка после каждой просьбы, а не только после первой.
    //
    // Сессии нет — но это ещё ничего не значит: она поднимается около секунды.
    // Просьба, пришедшая в уже поднимающуюся сессию, ломает её машину состояний;
    // однажды это стоило игре краха, а после — четырёх просьб подряд за
    // полсекунды, потому что выдержка применялась только к первой попытке.
    if (std::chrono::steady_clock::now() - askedAt_ < kStartupGrace) {
        return;
    }

    if (everStarted_) {
        spdlog::debug("игра вышла из сетевой сессии, поднимаем ещё раз");
    } else {
        spdlog::debug("сессия так и не поднялась за {} с, просим заново",
                     std::chrono::duration_cast<std::chrono::seconds>(kStartupGrace).count());
    }

    // Выход из сессии перед подъёмом здесь не зовётся, и это проверено, а не
    // упущено: `NETWORK_SESSION_LEAVE_SINGLE_PLAYER` перед подъёмом не меняет
    // ничего — игра всё равно остаётся в переходной камере, показывающей город
    // сверху. Догадка была в том, что подъём ложится поверх недоразобранной
    // сессии; не подтвердилась.
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
            spdlog::error("the solo session native was not found");
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
        spdlog::error("nothing to host a session with: the probe signatures did not resolve");
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
