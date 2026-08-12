#include "game_session.hpp"

#include "game/native_table.hpp"

#include <spdlog/spdlog.h>

#include <format>
#include <string>
#include <vector>

#include <windows.h>

namespace oxymp::client {
namespace {

/// Точка появления: международный аэропорт Лос-Сантоса, у терминала.
constexpr shared::Vec3 kSpawnPoint{-1037.7F, -2738.0F, 20.2F};
constexpr float kSpawnHeading = 328.0F;

/// Радиус, в котором убирается население при появлении и после смерти.
constexpr float kClearRadius = 400.0F;

/// Клавиша переключения свободного полёта.
constexpr int kNoclipKey = VK_F5;

/// Клавиша чата и клавиша консоли.
constexpr int kChatKey = 'T';
constexpr int kConsoleKey = VK_F8;

/// Клавиша админ-меню — та, что над Tab: «ё» в русской раскладке, обратный
/// апостроф в латинской. Так же устроено во всех знакомых играх.
constexpr int kMenuKey = VK_OEM_3;

/// Как часто гасятся ненужные скрипты игры.
///
/// Однократной зачистки мало: сюжетные скрипты заводят друг друга, и убитый
/// пускатель миссий возвращается вместе со следующей попыткой игры продолжить
/// сюжет. Раз в секунду — достаточно редко, чтобы не быть заметным, и
/// достаточно часто, чтобы миссия не успела начаться.
constexpr auto kSweepInterval = std::chrono::seconds{1};

/// Сколько времени после появления гасить их каждый кадр.
constexpr auto kEagerSweepWindow = std::chrono::seconds{20};

/// Как часто отчитываться о том, что подавление сюжета живо.
///
/// Достаточно редко, чтобы не засорять журнал, и достаточно часто, чтобы по
/// нему было видно: молчание означает поломку, а не тишину.
constexpr auto kSweepReportInterval = std::chrono::seconds{30};

/// Сколько держать экран загрузки, дожидаясь сервера.
///
/// Ждать бесконечно нельзя: выключенный сервер оставил бы игрока на экране
/// загрузки навсегда, хотя сама игра к этому моменту уже готова. По истечении
/// срока игрок попадает в мир, а состояние соединения переезжает в уголок.
///
/// Отсчёт начинается там же, где и само подключение, — с появления игрока в
/// мире. Срок заметно больше того, после которого клиент называет молчание
/// бедой: игрок должен успеть прочитать «сервер не отвечает» на экране загрузки,
/// а не увидеть эту надпись мельком перед самым её уходом.
constexpr auto kConnectWaitLimit = std::chrono::seconds{12};

/// За сколько проявляется картинка игры, в миллисекундах.
constexpr int kFadeIn = 500;

/// Сколько выждать после того, как игрок оказался в мире.
///
/// Признаки готовности загораются раньше, чем игра доводит мир и персонажа до
/// рабочего состояния, и замена персонажа в этот промежуток её роняет.
constexpr auto kWorldSettle = std::chrono::seconds{3};

/// Сессия одна на процесс: перехват тика — простая функция без состояния, и
/// связать её с объектом иначе нельзя.
GameSession* g_session = nullptr;

void frameEntry(bool ownsResources) {
    if (g_session != nullptr) {
        g_session->onFrame(ownsResources);
    }
}

/// Сработало ли нажатие клавиши именно сейчас.
///
/// Игра опрашивается каждый кадр, а переключатель обязан срабатывать один раз
/// на нажатие, а не всё время, пока клавишу держат.
bool pressedOnce(int key) {
    static bool wasDown[256] = {};

    const bool down = (::GetAsyncKeyState(key) & 0x8000) != 0;
    const bool fired = down && !wasDown[key & 0xFF];

    wasDown[key & 0xFF] = down;
    return fired;
}

} // namespace

GameSession::GameSession(const game::EngineAddresses& addresses, const game::NativeTable& table,
                         Settings settings, const SessionStatus& status,
                         const RemoteRoster& roster, LocalState& localState, SessionMail& mail,
                         UiFeed& feed, UiMail& clicks)
    : settings_(std::move(settings)),
      status_(status),
      roster_(roster),
      localState_(localState),
      mail_(mail),
      feed_(feed),
      clicks_(clicks),
      hud_(table),
      player_(table),
      screen_(table),
      story_(table),
      world_(table),
      frontend_(table),
      controls_(table),
      onlineMap_(table),
      appearance_(table),
      respawn_(table),
      noclip_(table),
      streaming_(table),
      vehicles_(table),
      remotePlayers_(table, vehicles_),
      nameplates_(table, hud_),
      adminMenu_(table),
      sessionState_(addresses),
      networkGame_(addresses),
      netSession_(addresses, table) {
    // Попадания уходят в ту же почту, что и реплики чата: замечает их игровой
    // поток, а отправляет сетевой.
    remotePlayers_.reportDamageTo(
        [&mail = mail_](shared::PlayerId victim, std::uint16_t amount, std::uint32_t weapon) {
            mail.postDamage(victim, amount, weapon);
        });

    // Распоряжения меню уходят той же почтой: составляет их игровой поток, а
    // отправляет сетевой.
    adminMenu_.requestThrough(
        [&mail = mail_](shared::AdminCommand command, shared::PlayerId target,
                        shared::Vec3 position) { mail.postAdmin(command, target, position); });
}

std::unique_ptr<GameSession> GameSession::create(const game::EngineAddresses& addresses,
                                                 Settings settings, const SessionStatus& status,
                                                 const RemoteRoster& roster,
                                                 LocalState& localState, SessionMail& mail,
                                                 UiFeed& feed, UiMail& clicks,
                                                 std::string& error) {
    if (g_session != nullptr) {
        error = "игровая сессия уже создана";
        return nullptr;
    }

    const game::NativeTable table{addresses};
    if (!table.valid()) {
        error = "таблица нативов недоступна";
        return nullptr;
    }

    std::unique_ptr<GameSession> session{new GameSession{
        addresses, table, std::move(settings), status, roster, localState, mail, feed, clicks}};

    // Ни одна из частей не является обязательной для остальных, поэтому
    // ненайденные нативы не отменяют сессию, а лишь отключают своё. Молчать при
    // этом нельзя: беззвучно пропавшая часть выглядит как необъяснимая пропажа
    // возможности, а не как ошибка в хешах.
    if (!session->hud_.ready()) {
        spdlog::error("нативы рисования не найдены — своего интерфейса не будет");
    }
    if (!session->player_.ready()) {
        spdlog::error("нативы игрока не найдены — появления не будет");
    }
    if (!session->screen_.ready()) {
        spdlog::error("нативы экрана не найдены — заставка игры останется");
    }
    if (!session->story_.ready()) {
        spdlog::error("нативы сюжета не найдены — кат-сцены и Online не погасить");
    }
    if (!session->world_.ready()) {
        spdlog::error("нативы мира не найдены — прохожие и розыск останутся");
    }
    if (!session->appearance_.ready()) {
        spdlog::error("нативы внешности не найдены — персонаж останется сюжетным");
    }
    if (!session->respawn_.ready()) {
        spdlog::error("нативы смерти не найдены — игра будет обрабатывать её сама");
    }
    session->appearance_.describeHandlers();

    if (!session->frontend_.ready()) {
        spdlog::error("нативы меню не найдены — клиент не отличит меню от игры");
    }
    if (!session->controls_.ready()) {
        spdlog::error("нативы управления не найдены — читкоды останутся доступны");
    }
    if (!session->remotePlayers_.ready()) {
        spdlog::error("нативы персонажей не найдены — чужих игроков не будет видно");
    }
    if (!session->vehicles_.ready()) {
        spdlog::error("нативы машин не найдены — чужие машины показать не удастся");
    }
    if (!session->nameplates_.ready()) {
        spdlog::error("нативы рисования в мире не найдены — подписей над головами не будет");
    }
    if (session->settings_.hostSession) {
        if (!session->netSession_.ready()) {
            spdlog::error("поднятие сессии заказано, но её точки входа не разрешились");
        }

        // Перехват ставится здесь, задолго до первой просьбы поднять сессию, и
        // это не запас времени ради запаса: без него сессия ложится через доли
        // секунды после подъёма, и поставить перехват в тот же кадр — значит
        // опоздать.
        std::string bailError;

        session->networkBail_ = game::NetworkBail::install(addresses, bailError);
        if (session->networkBail_ == nullptr) {
            spdlog::error("выход из сессии не перехвачен, и сессия проживёт доли секунды: {}",
                          bailError);
        }
    }
    if (session->settings_.forceNetworkGame != game::NetworkGame::Fake::None &&
        !session->networkGame_.ready()) {
        spdlog::error("подделка сетевого состояния заказана, но признаков не нашлось — "
                      "разведочные сигнатуры не разрешились на этой сборке");
    }

    // Порядок обязателен: перехват вправе вызвать обработчик сразу же, поэтому
    // сессия должна быть видна до его постановки.
    g_session = session.get();

    session->tick_ = game::ScriptTick::install(addresses, &frameEntry, error);
    if (session->tick_ == nullptr) {
        g_session = nullptr;
        return nullptr;
    }

    return session;
}

GameSession::~GameSession() {
    // Сперва снятие перехвата, и только потом отвязывание сессии: пока перехват
    // стоит, обработчик обязан оставаться рабочим.
    tick_.reset();
    g_session = nullptr;

    // Признаки возвращаются игре уже без перехвата, и это можно: они лежат в
    // обычной памяти, нативы для них не нужны, а значит и скриптовый контекст —
    // единственное, чего здесь уже нет, — не требуется.
    networkGame_.endHolding();
}

void GameSession::onFrame(bool ownsResources) {
    // Работа с игрой — только от потока с обработчиком скрипта. Рисование же
    // идёт всегда: во время загрузки это единственное, что вообще можно
    // сделать, и ровно тогда свой экран и нужен.
    if (ownsResources) {
        // Первым делом и один раз за запуск. Позже переключать бессмысленно:
        // мир вокруг игрока к тому времени уже собран по одиночной разметке.
        if (settings_.onlineMap && !onlineMapEnabled_) {
            onlineMapEnabled_ = true;
            onlineMap_.enable();
        }

        suppressGame();
        advance();
    }

    // Признаки сетевого состояния — обычная память, скриптовый контекст им не
    // нужен, поэтому смотрим на них и на тех кадрах, где работать с игрой
    // нельзя. Это не мелочь: игра выставляет их как раз на загрузке, и момент
    // перехода виден только отсюда.
    networkGame_.reportChanges();

    // Отметка о живом кадре. По ней интерфейс понимает, что игра не стоит: при
    // открытом меню паузы тик не идёт вовсе, и уголок, нарисованный поверх меню,
    // застыл бы на последнем состоянии.
    feed_.beat();

    reportSessionState();
    reportPause();
    publishStage();
    draw();
}

void GameSession::reportPause() {
    const auto measurement = frontend_.measurePause();
    if (!measurement) {
        return;
    }

    if (!measurement->tickRan()) {
        spdlog::debug("меню паузы закрылось: наш тик за это время не пришёл ни разу — "
                      "снимать паузу изнутри тика некому");
        return;
    }

    spdlog::debug("меню паузы закрылось: наших кадров {}, игровое время продвинулось на {} мс — "
                  "мир {}",
                 measurement->frames, measurement->gameTime,
                 measurement->worldFroze() ? "стоял" : "шёл");
}

void GameSession::reportSessionState() {
    if (!sessionState_.ready()) {
        return;
    }

    // Пишется только изменение, а не значение каждый кадр. Признак меняется
    // считаное число раз за запуск, и именно моменты перехода — то, ради чего
    // всё это заведено: по ним видно, дошла ли игра до сессии вообще и когда
    // именно она это решила.
    const bool started = sessionState_.started();
    if (sessionKnown_ && started == sessionStarted_) {
        return;
    }

    sessionKnown_ = true;
    sessionStarted_ = started;

    spdlog::debug("игра считает, что сетевая сессия {}", started ? "начата" : "не начата");
}

void GameSession::publishStage() {
    // Слой интерфейса живёт отдельным процессом и о происходящем внутри игры не
    // знает ничего. Стадию он может узнать только отсюда.
    const shared::LoadStage stage = [this] {
        switch (stage_) {
        case Stage::WaitingForPlayer:
            // Не Booting: до сюда доходит только тот, у кого скриптовый тик уже
            // идёт, а значит движок опознан и скрипты работают. Откатить полосу
            // назад — значит показать игроку, что дело пошло вспять.
            return frontend_.showing() ? shared::LoadStage::Landing : shared::LoadStage::Scripts;
        case Stage::ReplacingModel:
        case Stage::Spawning:
            return shared::LoadStage::Preparing;
        case Stage::Playing:
            return shared::LoadStage::Spawned;
        }
        return shared::LoadStage::Scripts;
    }();

    feed_.setStage(stage);
}

void GameSession::suppressGame() {
    // Заставкой и затемнениями распоряжаемся мы: моментом появления игрока
    // тоже, и картинка не имеет права проявиться раньше.
    screen_.hideLoadingScreen();
    screen_.takeOverFades();

    // Затемнения экрана до появления игрока здесь нет, и это пока не вывод, а
    // осторожность.
    //
    // Закрыть эти секунды хочется: в них видны сюжетный персонаж и надпись
    // «задание не выполнено» от нашей же зачистки сюжета. Попытка была — вызов
    // затемнения каждый кадр, — и тот запуск действительно встал на «игрок в
    // мире». Но позже выяснилось, что вставал он по другой причине: модуль в
    // лаунчере Rockstar помнил ключ входа в сетевой режим с прошлого запуска и
    // уводил игру ждать сессию. Затемнение могло быть ни при чём.
    //
    // Поэтому здесь его нет, но и запрета на него в README не записано:
    // наблюдение, у которого нашлось другое объяснение, ничего не доказывает.
    // Проверять придётся заново и по одному.

    story_.clearMissionFlag();

    // Каждый кадр, а не вместе с зачисткой скриптов.
    //
    // Раньше это стояло в зачистке, то есть раз в секунду, и объяснялось тем,
    // что «сюжет успевает попросить снова между зачистками». Объяснение верное,
    // а вывод из него был сделан неполный: успевает — значит убирать надо чаще,
    // а не в том же ритме. Отсюда и «задание не выполнено», висящее на экране
    // сразу после появления: надпись эту заказывает умирающий сюжетный скрипт,
    // и до следующей зачистки она стоит целую секунду.
    //
    // Вызов дешёвый: это шесть нативов, которым нечего делать, когда убирать
    // нечего.
    story_.clearMessages();

    if (story_.cutsceneRunning()) {
        story_.stopCutscene();
    }

    respawn_.suppressGameHandling();
    world_.suppressPopulation();
    world_.suppressWanted(player_.id());

    // Счётчик денег игры прячется каждый кадр: он показывает баланс настоящего
    // GTA Online, а тот к нашей сессии отношения не имеет. Свои деньги считает
    // сервер, и показывает их наш интерфейс.
    hud_.hideMoney();

    // Ход времени возвращается каждый кадр. Игра замедляет его сама — на колесе
    // выбора персонажа, при смерти, при аресте, — и в одиночной игре это
    // украшение. В мультиплеере замедлившийся живёт в другом темпе, чем
    // остальные: его снимки приходят к ним вдвое реже, и со стороны он движется
    // рывками, ничего об этом не зная.
    world_.keepTimeFlowing();

    // Читкоды меняют мир только на этом компьютере, и остальные игроки увидят
    // что-то своё. В мультиплеере это не шалость, а расхождение состояний.
    controls_.suppressCheats();

    // Колесо выбора персонажа — сюжетная возможность, и переключать в
    // мультиплеере некого. Оно же — главный источник того самого замедления.
    controls_.suppressCharacterWheel();

    // Меню паузы игре больше не запрещается.
    //
    // Запрет здесь был, и от него пришлось отказаться. Он отбирал у игрока Esc
    // целиком, а взамен давал большую карту — не ту, к которой игрок привык, без
    // настроек, без сохранения, без ничего. Лечение оказалось хуже болезни: мир,
    // встающий на паузу, мешает в мультиплеере, но меню, которого нет, мешает
    // всегда.
    //
    // Настоящее лекарство одно — сетевая сессия: в ней игра не останавливает мир
    // по Esc сама. Ею и занимаемся.
}


void GameSession::advance() {
    switch (stage_) {
    case Stage::WaitingForPlayer: {
        // Пока на экране меню игры, трогать персонажа нельзя: игра уже считает
        // игрока играющим, хотя в мир он ещё не попал, и любое обращение к его
        // персонажу в этот момент роняет её.
        // Пока на экране меню игры, трогать персонажа нельзя, но и нажимать за
        // игрока больше не за чем: страницы выбора режима не существует, её
        // убирает правка в один байт ещё до первого кадра. Всё, что остаётся
        // здесь, — дождаться, пока меню уйдёт.
        if (frontend_.showing()) {
            worldReadyAt_ = Clock::time_point{};
            return;
        }

        if (!player_.playing() || player_.ped() == 0) {
            worldReadyAt_ = Clock::time_point{};
            return;
        }

        // Выдержка после того, как всё сошлось: признаки готовности загораются
        // раньше, чем игра доводит мир и персонажа до рабочего состояния.
        const Clock::time_point now = Clock::now();
        if (worldReadyAt_ == Clock::time_point{}) {
            worldReadyAt_ = now;
            spdlog::debug("игрок в мире, выжидаем");
            return;
        }
        if (now - worldReadyAt_ < kWorldSettle) {
            return;
        }

        stage_ = Stage::ReplacingModel;
        spdlog::debug("меняем модель игрока");
        return;
    }

    case Stage::ReplacingModel: {
        const game::Appearance::Progress progress = appearance_.advance(player_.id());
        if (progress == game::Appearance::Progress::Loading) {
            return;
        }

        ownModel_ = progress == game::Appearance::Progress::Done;
        if (!ownModel_) {
            spdlog::warn("модель не заменена — сюжет останется, иначе игрок исчезнет вместе с ним");
        }

        stage_ = Stage::Spawning;
        return;
    }

    case Stage::Spawning:
        spawn();
        stage_ = Stage::Playing;
        playingSince_ = Clock::now();

        // Отсюда начинается подключение: сетевой поток ждёт именно этой
        // отметки. Экран загрузки при этом остаётся — на нём теперь идёт ход
        // подключения, и уйдёт он по своему признаку, в draw.
        feed_.setWorldReady();
        return;

    case Stage::Playing:
        // Подделка сетевого состояния включается только здесь, позже всего
        // остального, и это не осторожность ради осторожности: на загрузке игра
        // распоряжается сессией сама, читает эти признаки чаще всего и ветвится
        // по ним в местах, где объекты сессии обязаны существовать. Раньше
        // этого места вмешательство — самый верный способ упасть.
        if (!networkGame_.holding()) {
            networkGame_.beginHolding(settings_.forceNetworkGame);
        }
        networkGame_.hold();

        // Просьба поднять сессию идёт здесь же и по той же причине, что и
        // подделка: раньше игра распоряжается сетью сама. Повторяется каждый
        // кадр не зря — распорядитель сети может появиться позже игрока, и
        // тогда первые просьбы уйдут в никуда, а эта дождётся.
        if (settings_.hostSession) {
            holdSession();
        }

        // Каждый кадр, а не один раз после смерти. Игра гасит интерфейс сама и
        // не всегда возвращает: на смерти она убирает и радар, и полосы, считая,
        // что дальше её собственный порядок разбора смерти включит их обратно, —
        // а этот порядок мы у неё отобрали. Однократного включения при подъёме
        // оказалось мало.
        screen_.keepInterfaceUp();

        sweepScripts();
        story_.restoreControl();

        ensureTextEntry();
        handleTyping();

        const int player = player_.id();
        const int ped = player_.ped();
        const bool dead = respawn_.dead(player);

        // Мир вокруг игрока держится подгруженным всегда, а не только после
        // переноса: игра вправе выгрузить то, чего он сейчас не касается, — и
        // тогда под ним пропадает земля.
        streaming_.keepCollisionAround(ped);

        // Пока мир вокруг точки переноса не появился, персонажа держат
        // замороженным: отпущенный, он полетит сквозь незагруженную землю.
        if (streaming_.loading()) {
            player_.freeze(ped, true);

            if (streaming_.advance()) {
                player_.freeze(ped, false);
            }
        }

        handleMenu(player, ped);
        applyOrders(ped);
        applyIncomingDamage(ped);
        handleDeath(player);
        handleInput(ped);
        publishLocalState(player, ped, dead);
        showRemotePlayers(ped);
        return;
    }
}

void GameSession::ensureTextEntry() {
    if (textEntry_ != nullptr || textEntryFailed_) {
        return;
    }

    const HWND window = game::Window::findOwnWindow();
    if (window == nullptr) {
        // Окна ещё нет. Это не беда: до сюда мы доходим уже в мире, но окно
        // игра пересоздаёт на переходах, и пропустить его на кадр — обычное
        // дело.
        return;
    }

    std::string error;

    textEntry_ = game::TextEntry::install(window, error);
    if (textEntry_ == nullptr) {
        // Один раз: без перехвата не будет чата, и повторять попытку каждый
        // кадр значит записать в журнал одну и ту же строку тысячу раз.
        textEntryFailed_ = true;
        spdlog::error("перехват клавиатуры не поставлен — чата не будет: {}", error);
    }
}

void GameSession::handleTyping() {
    if (pressedOnce(kConsoleKey)) {
        consoleVisible_ = !consoleVisible_;
        feed_.setConsoleVisible(consoleVisible_);
    }

    typingJustEnded_ = false;

    if (textEntry_ == nullptr) {
        return;
    }

    // Порядок здесь обязателен: сперва итог прошлого набора, и только потом
    // начало нового.
    //
    // Раньше было наоборот, и на этом терялось всё набранное для меню. Набор
    // заканчивается вводом, а меню до этого мгновения всё ещё просит строку:
    // видя просьбу, клиент начинал набор заново — и стирал итог, до которого не
    // дошёл. Со стороны это выглядело так, что название модели ввести нельзя:
    // строка набирается, ввод нажимается, и ничего не происходит.
    if (!textEntry_->active()) {
        std::string typed;
        const game::TextEntry::Outcome outcome = textEntry_->takeOutcome(typed);

        if (outcome != game::TextEntry::Outcome::Typing) {
            if (outcome == game::TextEntry::Outcome::Submitted) {
                if (typingFor_ == Typing::Menu) {
                    adminMenu_.supplyText(std::move(typed));
                } else {
                    mail_.postChat(std::move(typed));
                }
            } else if (typingFor_ == Typing::Menu) {
                adminMenu_.cancelText();
            }

            typingFor_ = Typing::Chat;
            typingJustEnded_ = true;
            feed_.setInput(false, {});
        }
    }

    if (!textEntry_->active()) {
        // Строку может попросить меню — например, для названия машины. Его
        // просьба идёт первой: раз меню открыто, чат сейчас не при чём.
        if (adminMenu_.wantsText()) {
            typingFor_ = Typing::Menu;

            // Название модели набирается латиницей по местам клавиш: раскладку
            // в GTA не переключить, а `adder` в русской раскладке набирается
            // как `фввук`.
            textEntry_->begin(game::TextEntry::Mode::Latin);
        } else if (pressedOnce(kChatKey)) {
            typingFor_ = Typing::Chat;
            textEntry_->begin(game::TextEntry::Mode::Free);
        }
    }

    if (textEntry_->active()) {
        // Пока набирается текст, игра не должна видеть ни одного нажатия.
        // Съеденных сообщений окна для этого мало: клавиатуру GTA читает
        // напрямую, мимо них.
        controls_.suppressEverything();
        feed_.setInput(true, textEntry_->text());
    }
}

void GameSession::applyClicks(int player, int ped) {
    for (const UiClick& click : clicks_.take()) {
        // Нажатие мышью где угодно, кроме самого поля, прекращает набор: игрок
        // ушёл из поля, и держать клавиатуру за ним значит отбирать её у того,
        // чем он занялся вместо этого.
        if (click.kind != UiClick::Kind::Point && click.kind != UiClick::Kind::Ask &&
            typingFor_ == Typing::Menu && textEntry_ != nullptr && textEntry_->active()) {
            textEntry_->cancel();
        }

        switch (click.kind) {
        case UiClick::Kind::Point:
            adminMenu_.select(click.index);
            break;

        case UiClick::Kind::Press:
            adminMenu_.select(click.index);
            adminMenu_.press(game::AdminMenu::Key::Enter, player, ped);
            break;

        case UiClick::Kind::Alternate:
            adminMenu_.select(click.index);
            adminMenu_.press(game::AdminMenu::Key::Alternate, player, ped);
            break;

        case UiClick::Kind::Back:
            adminMenu_.press(game::AdminMenu::Key::Back, player, ped);
            break;

        case UiClick::Kind::Close:
            adminMenu_.close();
            break;

        case UiClick::Kind::Ask:
            adminMenu_.askForModel();
            break;
        }
    }
}

void GameSession::handleMenu(int player, int ped) {
    applyClicks(player, ped);

    // Пока набирается текст, клавиши принадлежат чату: «ё» в реплике не должна
    // открывать меню. Кадр, в котором набор кончился, — тоже: клавиша, которой
    // его закончили, ещё зажата, и меню приняло бы её за своё нажатие.
    const bool typing = (textEntry_ != nullptr && textEntry_->active()) || typingJustEnded_;

    // Клавиши опрашиваются каждый кадр, даже когда меню их не слушает.
    //
    // Опрос ведёт счёт нажатиям, и пропущенный кадр оставляет в нём вчерашнее
    // состояние: клавиша, нажатая во время набора и всё ещё зажатая после него,
    // выглядела бы нажатой заново. Ввод в чате отзывался бы в меню.
    const bool menuKey = pressedOnce(kMenuKey);
    const bool up = pressedOnce(VK_UP);
    const bool down = pressedOnce(VK_DOWN);
    const bool enter = pressedOnce(VK_RETURN);
    const bool alternate = pressedOnce(VK_RIGHT);
    const bool left = pressedOnce(VK_LEFT);
    const bool backspace = pressedOnce(VK_BACK);
    const bool escape = pressedOnce(VK_ESCAPE);

    if (!typing && menuKey) {
        adminMenu_.toggle();
    }

    if (adminMenu_.open() && !typing) {
        // Пока меню открыто, игра не должна видеть ни одного нажатия: стрелки
        // ведут по пунктам, а не персонажа.
        controls_.suppressEverything();

        if (up) {
            adminMenu_.press(game::AdminMenu::Key::Up, player, ped);
        }
        if (down) {
            adminMenu_.press(game::AdminMenu::Key::Down, player, ped);
        }
        if (enter) {
            adminMenu_.press(game::AdminMenu::Key::Enter, player, ped);
        }
        if (alternate) {
            adminMenu_.press(game::AdminMenu::Key::Alternate, player, ped);
        }
        if (left || backspace) {
            adminMenu_.press(game::AdminMenu::Key::Back, player, ped);
        }

        // Escape закрывает меню целиком, а не на шаг назад.
        //
        // Без этого выйти было нечем, кроме стрелки влево и Backspace, а
        // человек жмёт Escape — это первое, что приходит в голову, и до сих пор
        // оно не делало ничего.
        if (escape) {
            adminMenu_.close();
        }
    }

    // Список игроков берётся тот же, что и для показа их в мире: меню
    // переносит к ним и вызывает их к себе, и точка нужна свежая.
    std::vector<game::AdminMenu::Participant> participants;

    for (const RemoteView& remote : roster_.snapshot()) {
        participants.push_back(game::AdminMenu::Participant{
            .id = remote.id,
            .nickname = remote.nickname,
            .position = remote.state.position,
        });
    }

    adminMenu_.update(std::move(participants), player, ped);

    const game::AdminMenu::View view = adminMenu_.view();

    std::vector<UiFeed::MenuItem> items;
    items.reserve(view.items.size());

    for (const game::AdminMenu::Item& item : view.items) {
        items.push_back(UiFeed::MenuItem{
            .label = item.label,
            .value = item.value,
            .kind = static_cast<unsigned int>(item.kind),
            .on = item.on,
        });
    }

    feed_.setMenu(UiFeed::Menu{
        .open = view.open,
        .title = view.title,
        .items = std::move(items),
        .selected = view.selected,
        .note = view.note,
        // Набор для меню виден странице как живое поле ввода: и пока меню ждёт
        // строку, и пока игрок её набирает. Строка чата в это время молчит —
        // набранное показывается в самом меню.
        .asking = adminMenu_.wantsText() ||
                  (typingFor_ == Typing::Menu && textEntry_ != nullptr && textEntry_->active()),
    });
}

void GameSession::applyOrders(int ped) {
    if (ped == 0) {
        return;
    }

    for (const shared::AdminOrder& order : mail_.takeIncomingAdmin()) {
        switch (order.command) {
        case shared::AdminCommand::Summon:
            player_.teleport(ped, order.position);
            spdlog::info("игрок {} перенёс нас к себе", order.issuer);
            break;
        }
    }
}

void GameSession::applyIncomingDamage(int ped) {
    if (ped == 0) {
        return;
    }

    for (const shared::DamageTaken& taken : mail_.takeIncomingDamage()) {
        player_.applyDamage(ped, taken.amount);

        spdlog::info("игрок {} попал по нам на {}", taken.attacker, taken.amount);
    }
}

void GameSession::teleportSafely(int ped, shared::Vec3 destination, float heading) {
    if (ped == 0) {
        return;
    }

    player_.teleport(ped, destination);
    player_.setHeading(ped, heading);

    // Подгрузка начинается после переноса, а не до: сфера строится вокруг точки,
    // где игрок уже стоит, и игра подгружает её в первую очередь.
    streaming_.beginLoad(destination);
}

void GameSession::holdSession() {
    // Держать начинаем до первой просьбы: игра выходит из сессии сама, и первый
    // же её выход случается раньше, чем мы успеем спросить, поднялась ли она.
    if (networkBail_ != nullptr) {
        networkBail_->hold(true);
    }

    netSession_.host(settings_.sessionMode);

    // Повторная просьба, если сессия всё-таки легла. Раньше она была главным
    // средством и работала против себя: сессию просили поднять каждый кадр, пока
    // та поднималась, и от этого она и разваливалась. С выдержкой она стала тем,
    // чем должна быть, — запасным выходом, который почти никогда не нужен.
    netSession_.rehostIfDropped(settings_.sessionMode, sessionState_.started());
}

void GameSession::spawn() {
    const int ped = player_.ped();
    if (ped == 0) {
        return;
    }

    // Персонаж достаётся нам из сюжетной сцены замороженным и с её заданиями.
    // Пока это не снято, он стоит на месте, что бы игрок ни нажимал.
    player_.release(ped);

    teleportSafely(ped, kSpawnPoint, kSpawnHeading);

    // Запрет населения касается только новых прохожих и машин: созданные до
    // него остаются на местах, и убрать их нужно отдельно.
    world_.clearArea(kSpawnPoint, kClearRadius);

    spdlog::info("игрок появился в аэропорту: {:.1f} {:.1f} {:.1f}", kSpawnPoint.x, kSpawnPoint.y,
                 kSpawnPoint.z);
}

void GameSession::sweepScripts() {
    // Два условия, и оба обязательны.
    //
    // Своя модель — потому что сюжетный персонаж принадлежит сюжетным скриптам
    // и уходит вместе с ними.
    //
    // Обычная игра — потому что до неё завершать нечего, а вред есть: на старте
    // игра сама распоряжается своими скриптами, и вмешательство в этот момент
    // роняет её по 0xC0000005. Проверено дважды, оба раза через несколько
    // секунд после запуска стартовых скриптов.
    if (!ownModel_) {
        return;
    }

    const Clock::time_point now = Clock::now();

    // Первое время — каждый кадр. Сюжет в эти секунды ещё сопротивляется:
    // погашенный пускатель миссий заводится заново, и за секунду между
    // зачистками миссия успевает начаться и показать своё название. Дальше
    // такой частоты не нужно, а тратить на неё кадр постоянно незачем.
    const bool settling = now - playingSince_ < kEagerSweepWindow;

    if (!settling && sweptAt_ != Clock::time_point{} && now - sweptAt_ < kSweepInterval) {
        return;
    }
    sweptAt_ = now;

    ++sweeps_;

    // Отчёт время от времени, а не однажды.
    //
    // Раньше здесь была одна строка на весь запуск, и по журналу нельзя было
    // отличить работающее подавление от давно отвалившегося: и там и там
    // молчание. А вопрос «сюжет вернулся — а гасим ли мы его ещё» возникает
    // ровно тогда, когда ответ нужен немедленно.
    if (!sweepLogged_ || now - sweepReportedAt_ >= kSweepReportInterval) {
        sweepLogged_ = true;
        sweepReportedAt_ = now;

        spdlog::debug("подавление работает: зачисток {}, гасим сюжет и телефон{}", sweeps_,
                     settings_.hostSession ? "; сетевые оставлены — они ведут сессию"
                                           : " и GTA Online");
    }

    story_.terminateStory();

    // Сетевые скрипты гасятся, только пока мы не поднимаем свою сессию.
    //
    // Их гашение заводилось по делу: изменённому клиенту в GTA Online делать
    // нечего. Но с поднятием сессии довод переворачивается — сессию ведут ровно
    // эти скрипты, и maintransition прежде всего. Погасив их, мы убиваем то, что
    // сами же попросили начать.
    //
    // Так и вышло на первой пробе, и это видно по журналу поминутно: сессия
    // поднялась, игра выставила оба признака и ответила «начата», а через 336
    // миллисекунд всё легло обратно. Между этими двумя записями стояла наша
    // зачистка, идущая каждый кадр первые двадцать секунд. Со стороны это
    // выглядело как улетевшая висеть над городом камера — вступительная камера
    // сетевой сессии, оставшаяся без скрипта, который её вёл.
    if (!settings_.hostSession) {
        story_.terminateOnline();
    }

    story_.terminatePhone();

}

void GameSession::handleDeath(int player) {
    const bool isDead = respawn_.dead(player);
    if (!respawn_.due(isDead)) {
        return;
    }

    // Свободный полёт снимается до подъёма: он держит выключенной физику
    // персонажа, которого сейчас не станет.
    if (noclip_.active()) {
        noclip_.setActive(player_.ped(), false);
    }

    respawn_.resurrect(kSpawnPoint, kSpawnHeading, player);
    streaming_.beginLoad(kSpawnPoint);
    world_.clearArea(kSpawnPoint, kClearRadius);

    player_.release(player_.ped());

    // Игра гасит экран и убирает интерфейс на смерти, а возвращать их положено
    // тому порядку разбора смерти, который мы у неё отобрали.
    screen_.fadeIn(kFadeIn);
    screen_.keepInterfaceUp();

    spdlog::info("игрок поднят в точке появления");
}

void GameSession::publishLocalState(int player, int ped, bool dead) {
    if (ped == 0) {
        return;
    }

    // Отсюда положение уходит на сервер, а с него — остальным игрокам. Без
    // этого мы для них не существуем: сервер рассылает лишь то, что ему
    // прислали.
    shared::PlayerState state = player_.snapshot(player, ped, dead);

    std::optional<shared::VehicleState> vehicle;

    if (const auto seat = vehicles_.seatOf(ped)) {
        state.flags |= static_cast<std::uint32_t>(shared::PlayerFlag::InVehicle);
        state.seat = seat->index;

        // Хозяин машины — её водитель. Пассажир узнаёт его по персонажу за
        // рулём, и другого пути нет: сервер о машинах знает ровно то, что ему
        // рассказали, а рассказывает о них водитель.
        state.vehicleOwner = ownerOfVehicle(*seat);

        if (seat->index == shared::kDriverSeat) {
            // За рулём мы сами — значит и снимок машины рассылать нам.
            vehicle = vehicles_.describe(seat->vehicle);
        }
    }

    localState_.set(state, vehicle);
}

shared::PlayerId GameSession::ownerOfVehicle(const game::Vehicles::Seat& seat) const {
    if (seat.driverPed == 0) {
        // Машина без водителя. Числить её за собой нельзя — уедем на ней вдвоём
        // с тем, кто сядет за руль позже.
        return shared::kInvalidPlayerId;
    }

    if (seat.driverPed == player_.ped()) {
        return status_.snapshot().playerId;
    }

    return remotePlayers_.ownerOf(seat.driverPed);
}

void GameSession::showRemotePlayers(int ped) {
    // Машины идут первыми: игроков в них сажать, а посадить некуда, пока машины
    // нет. Порядок здесь — не вкус, а зависимость.
    std::vector<shared::VehicleState> vehicles;
    for (const RemoteVehicleView& vehicle : roster_.vehicles()) {
        vehicles.push_back(vehicle.state);
    }

    vehicles_.sync(vehicles);

    const std::vector<RemoteView> players = roster_.snapshot();

    std::vector<game::RemotePlayerView> views;
    views.reserve(players.size());

    for (const RemoteView& player : players) {
        views.push_back(game::RemotePlayerView{
            .id = player.id,
            .nickname = player.nickname,
            .state = player.state,
        });
    }

    remotePlayers_.sync(views, ped);
    nameplates_.draw(views, player_.coords(ped));
}

void GameSession::handleInput(int ped) {
    if (ped == 0) {
        return;
    }

    // Пока набирается текст или открыто меню, клавиши принадлежат им: буква F5
    // в реплике не должна поднимать игрока в воздух.
    if ((textEntry_ != nullptr && textEntry_->active()) || adminMenu_.open()) {
        return;
    }

    if (pressedOnce(kNoclipKey)) {
        noclip_.setActive(ped, !noclip_.active());
        spdlog::info("свободный полёт: {}", noclip_.active() ? "включён" : "выключен");
    }

    noclip_.update(ped);
}

bool GameSession::running() const noexcept {
    return tick_ != nullptr && tick_->frames() != 0;
}

bool GameSession::loadingScreenDone() const {
    if (stage_ != Stage::Playing) {
        return false;
    }

    if (status_.snapshot().inSession()) {
        return true;
    }

    return Clock::now() - playingSince_ >= kConnectWaitLimit;
}

void GameSession::draw() {
    // Интерфейс рисует не игра, а слой поверх неё, и он же рисовал экран
    // загрузки. Изнутри игры экран загрузки появился бы уже после заставки
    // Rockstar и страницы выбора режима — то есть после всего, что должен был
    // собой закрыть; а уголок с состоянием сервера, который раньше рисовался
    // нативами игры, переехал туда же — к чату и консоли, в один правый угол.
    //
    // Здесь остаётся только решить, когда слою пора менять экран загрузки на
    // игровой интерфейс.
    if (!loadingScreenDone()) {
        return;
    }

    if (!readyReported_) {
        readyReported_ = true;

        // Проявить картинку обязаны мы: игре это запрещено — иначе она показала
        // бы недогруженный мир и чужого персонажа раньше, чем мы успеем
        // заменить одно и перенести другое. Забыть про это — значит оставить
        // игрока перед вечным чёрным экраном.
        screen_.fadeIn(kFadeIn);
        feed_.setReady();

        spdlog::info("игрок в игре, слой загрузки может уходить");
    }
}

} // namespace oxymp::client
