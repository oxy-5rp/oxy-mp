#pragma once

#include "game/appearance.hpp"
#include "game/controls.hpp"
#include "game/engine_addresses.hpp"
#include "game/frontend.hpp"
#include "game/hud.hpp"
#include "game/nameplates.hpp"
#include "game/net_session.hpp"
#include "game/network_bail.hpp"
#include "game/network_game.hpp"
#include "game/objects.hpp"
#include "game/online_map.hpp"
#include "game/player.hpp"
#include "game/remote_players.hpp"
#include "game/respawn.hpp"
#include "game/screen.hpp"
#include "game/session_state.hpp"
#include "game/script_tick.hpp"
#include "game/story.hpp"
#include "game/streaming.hpp"
#include "game/text_entry.hpp"
#include "game/vehicles.hpp"
#include "game/window.hpp"
#include "game/world.hpp"
#include "session_mail.hpp"
#include "session_status.hpp"
#include "ui_feed.hpp"

#include <oxymp/shared/math/vec3.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace oxymp::client {

/// Всё, что клиент делает внутри кадра игры.
///
/// Единственное место, где живёт игровая часть клиента: сеть про игру не знает
/// вовсе, а игра про сеть — только через снимок состояния. Разделение не
/// формальное: сетевой поток не имеет права задерживать кадр, а кадр не имеет
/// права ждать сеть.
///
/// Создание ставит перехват тика, разрушение его снимает. Пока объект жив, игра
/// вызывает его onFrame раз в кадр из своего потока.
class GameSession {
public:
    struct Settings {
        /// Куда подключаемся, в виде «адрес:порт» — для показа игроку.
        std::string serverAddress;
        std::string nickname;

        /// Какие признаки сетевой игры подделывать.
        ///
        /// Разведка, а не рабочая возможность: признак выставляется без объектов
        /// сессии, которые к нему прилагаются в настоящей сетевой игре, и любая
        /// ветка, которая за ними полезет, уронит игру. Поэтому по умолчанию
        /// выключено и включается ключом лаунчера, без пересборки.
        game::NetworkGame::Fake forceNetworkGame = game::NetworkGame::Fake::None;

        /// Просить ли игру поднять настоящую сетевую сессию.
        ///
        /// В отличие от подделки признака — путь правильный: признак выставит
        /// сама игра, вместе с объектами, которых подделке не хватало. Но вызов
        /// пока вслепую, и по умолчанию выключен.
        bool hostSession = false;

        /// Каким способом поднимать сессию, если поднимать.
        game::NetSession::Mode sessionMode = game::NetSession::Mode::Solo;

        /// Переключать ли мир на разметку сетевого режима.
        ///
        /// По умолчанию нет, и это перемена: раньше переключали всегда. Карта
        /// сетевого режима — не другая карта, а другой набор кусков мира, и
        /// какие из них игра подгрузит, зависит от того, во что она себя
        /// считает играющей. Разошлось — и под ногами дыра.
        ///
        /// Ключом, а не насовсем, потому что доказательства ни за, ни против
        /// нет: дыры видел игрок, а не журнал. Выключенная по умолчанию, она
        /// даёт этому доказательству появиться.
        bool onlineMap = false;
    };

    /// Собирает игровую часть и ставит перехват тика.
    ///
    /// status, roster, mail и feed обязаны пережить сессию: они принадлежат
    /// вызывающему. В status и roster пишет сетевой поток, а читает игровой; в
    /// feed сессия рассказывает интерфейсу обо всём, что показывает игроку.
    [[nodiscard]] static std::unique_ptr<GameSession> create(const game::EngineAddresses& addresses,
                                                             Settings settings,
                                                             const SessionStatus& status,
                                                             const RemoteRoster& roster,
                                                             LocalState& localState,
                                                             SessionMail& mail,
                                                             UiFeed& feed,
                                                             std::string& error);

    ~GameSession();

    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    /// Вызывается игрой раз в кадр. Наружу — только для перехвата тика.
    ///
    /// ownsResources — можно ли сейчас заказывать модели и завершать чужие
    /// скрипты. Во время загрузки игры нельзя: подходящего скриптового потока
    /// ещё нет, зато рисовать уже можно и нужно.
    void onFrame(bool ownsResources);

    /// Дошёл ли клиент до первого кадра внутри игры.
    ///
    /// Тик доходит до нас только с началом сюжетного режима, поэтому это же
    /// служит признаком того, что страница выбора позади.
    [[nodiscard]] bool running() const noexcept;

private:
    GameSession(const game::EngineAddresses& addresses, const game::NativeTable& table,
                Settings settings, const SessionStatus& status, const RemoteRoster& roster,
                LocalState& localState, SessionMail& mail, UiFeed& feed);

    /// Чем клиент занят между запуском игры и полноценной игрой.
    ///
    /// Порядок шагов задан не удобством, а зависимостями: своя модель обязана
    /// появиться раньше, чем гасится сюжет, иначе сюжетные скрипты унесут
    /// персонажа игрока с собой.
    enum class Stage {
        /// Игра ещё не создала играющего персонажа.
        WaitingForPlayer,

        /// Идёт замена сюжетного персонажа на своего.
        ReplacingModel,

        /// Персонаж переносится в точку появления, сюжет гасится.
        Spawning,

        /// Обычная игра.
        Playing,
    };

    using Clock = std::chrono::steady_clock;

    void suppressGame();
    void advance();
    void spawn();
    void sweepScripts();
    void handleDeath(int player);
    void showRemotePlayers(int ped);
    void publishLocalState(int player, int ped, bool dead);
    void draw();
    void publishStage();
    void reportSessionState();

    /// Ставит перехват клавиатуры, когда окно игры наконец появится.
    void ensureTextEntry();

    /// Ведёт чат и консоль: открытие, набор, отправку.
    void handleTyping();

    /// Исполняет то, что велел сервер: перенос игрока и именованные события.
    void applyServerEvents(int ped);

    /// Держит игру в поднятой сетевой сессии.
    void holdSession();

    /// Переносит игрока и дожидается, пока вокруг точки появится мир.
    ///
    /// Пока мир не появился, персонаж держится замороженным: под ним нет земли,
    /// и отпущенный он полетит сквозь неё.
    void teleportSafely(int ped, shared::Vec3 destination, float heading);


    /// Применяет к своему персонажу урон, о котором сообщил сервер.
    ///
    /// Здоровье при этом не меняется: его назначает сервер отдельным сообщением.
    /// Отсюда — только то, что видно и слышно: вспышка на экране и строка о том,
    /// кто попал.
    void applyIncomingDamage(int ped);

    /// Принимает от сервера здоровье, снаряжение и предметы.
    void applyServerState(int ped);

    /// Где появляться. Названное сервером, а пока он молчит — запасное.
    [[nodiscard]] shared::Vec3 spawnPoint() const;

    /// Пора ли снимать внешность машины, за рулём которой мы сидим.
    ///
    /// Кто какую машину ведёт, здесь больше не решается: это решает сервер. А вот
    /// как часто перечитывать внешность — забота наша: полсотни вызовов нативов
    /// ради цвета, который не менялся с начала сессии, дороги за кадр и дёшевы
    /// раз в пару секунд.
    ///
    /// Не const: вызов сдвигает отсчёт до следующего раза.
    [[nodiscard]] bool appearanceDue();

    /// Ведёт замер меню паузы и записывает результат каждого открытия.
    void reportPause();

    /// Пора ли убирать экран загрузки.
    [[nodiscard]] bool loadingScreenDone() const;

    Settings settings_;
    const SessionStatus& status_;
    const RemoteRoster& roster_;
    LocalState& localState_;
    SessionMail& mail_;

    /// Куда уходит всё, что показывает игровой интерфейс: от хода загрузки до
    /// строк чата. Он живёт внутри этого же процесса и рисуется прямо в кадр.
    UiFeed& feed_;

    game::Hud hud_;
    game::Player player_;
    game::Screen screen_;
    game::Story story_;
    game::World world_;
    game::Frontend frontend_;
    game::Controls controls_;
    game::OnlineMap onlineMap_;
    game::Appearance appearance_;
    game::Respawn respawn_;
    game::Streaming streaming_;

    /// Объявлены в этом порядке не случайно: машины строятся раньше игроков,
    /// потому что игроки на них ссылаются.
    game::Vehicles vehicles_;
    game::RemotePlayers remotePlayers_;

    /// Предметы, расставленные в мире. Ни от кого не зависят: у них нет ведущего
    /// и они не двигаются — оттого и стоят особняком от машин и людей.
    game::Objects objects_;
    game::Nameplates nameplates_;

    game::SessionState sessionState_;
    game::NetworkGame networkGame_;
    game::NetSession netSession_;

    /// Перехват клавиатуры. Ставится не сразу: окна игры в первые секунды ещё
    /// нет, а без окна перехватывать нечего.
    std::unique_ptr<game::TextEntry> textEntry_;

    /// Запертая дверь, через которую игра уходит из сессии.
    ///
    /// Ставится задолго до первой просьбы поднять сессию: сессия ложится через
    /// доли секунды после подъёма, и поставить перехват в тот же кадр — значит
    /// опоздать.
    std::unique_ptr<game::NetworkBail> networkBail_;

    /// Перехват тика. Снимается первым при разрушении: пока он стоит, игра
    /// может вызвать onFrame в любое мгновение.
    std::unique_ptr<game::ScriptTick> tick_;


    Stage stage_ = Stage::WaitingForPlayer;

    /// Досталась ли игроку своя модель.
    ///
    /// От этого зависит, можно ли гасить сюжет: пока персонаж сюжетный, его
    /// унесёт вместе со скриптом, которому он принадлежит.
    bool ownModel_ = false;

    /// Время отсчитывается по монотонным часам, а не по игровому таймеру:
    /// игровой стоит на нуле, пока не началась сессия, — то есть ровно тогда,
    /// когда экран загрузки и нужен.
    /// Когда внешность машин снимали в прошлый раз.
    Clock::time_point vehicleAppearanceAt_{};

    Clock::time_point worldReadyAt_{};
    Clock::time_point playingSince_{};
    Clock::time_point sweptAt_{};

    /// Что игра в прошлый раз ответила про сетевую сессию. Нужно, чтобы писать
    /// в журнал переходы, а не одно и то же по тридцать раз в секунду.
    bool sessionKnown_ = false;
    bool sessionStarted_ = false;

    bool sweepLogged_ = false;

    /// Сколько зачисток сюжета сделано и когда о них отчитывались.
    std::uint64_t sweeps_ = 0;
    Clock::time_point sweepReportedAt_{};
    bool readyReported_ = false;
    bool onlineMapEnabled_ = false;

    /// Показана ли консоль. Переключается по F8.
    bool consoleVisible_ = false;


    /// Жаловались ли уже, что перехват клавиатуры не встал.
    bool textEntryFailed_ = false;
};

} // namespace oxymp::client
