#pragma once

#include <oxymp/shared/protocol/messages.hpp>
#include <oxymp/shared/status/load_stage.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace oxymp::client {

/// Всё, что показывает игровой интерфейс.
///
/// Раньше это ходило через разделяемую память: интерфейс жил отдельным
/// процессом, и другого пути к нему не было. Теперь он живёт внутри игры, в том
/// же процессе, — и гонять состояние через страницу памяти, чтобы прочитать
/// собственную запись, стало незачем.
///
/// Пишут сюда трое: сетевой поток (состояние сервера, список игроков, чат),
/// игровой (ввод, консоль) и журнал (все сообщения клиента). Читает один — тот,
/// что рисует кадр. Отсюда и блокировка: она коротка и никем не оспаривается.
class UiFeed {
public:
    /// Строка ленты.
    struct Line {
        /// ChatKind для чата, уровень важности spdlog для консоли.
        unsigned int kind = 0;
        std::string text;
    };

    /// Игрок в списке участников сессии.
    struct Participant {
        shared::PlayerId id = shared::kInvalidPlayerId;
        std::string nickname;
    };

    /// Состояние соединения, каким его видит игрок.
    struct Connection {
        unsigned int state = 0;
        std::size_t players = 0;
        int latencyMilliseconds = -1;
        shared::PlayerId playerId = shared::kInvalidPlayerId;

        /// Стоит ли показывать молчание сервера как беду. Первые секунды после
        /// входа соединения ещё нет, и это обычный ход дела.
        bool troubled = false;

        /// Деньги игрока по счёту сервера. Пусто — сервер ещё не сказал, и тогда
        /// показывается прочерк: ноль на этом месте выглядел бы как разорение.
        std::optional<std::int64_t> money;

        /// Остался ли игрок без сервера и почему: DisconnectReason числом.
        ///
        /// Не выводится из state, и это существенно. Состояние Waiting значит
        /// одновременно «ещё подключаемся» и «выпали из сессии»: первое — обычный
        /// ход дела, второе — то, ради чего игроку показывают окно во весь экран.
        unsigned int disconnect = 0;

        /// Чем сервер объяснил отказ. Пусто — объяснять нечего.
        std::string disconnectDetail;
    };

    void describeSession(std::string address, std::string nickname);

    /// На какой стадии вход в мир.
    ///
    /// Экран загрузки живёт на той же странице, что и интерфейс, и меняется на
    /// него по признаку готовности. Отдельным процессом он был ровно до тех пор,
    /// пока рисовать в кадр игры было нечем.
    void setStage(shared::LoadStage stage);

    /// Игрок появился в мире и стоит на земле.
    ///
    /// Не то же самое, что уход экрана загрузки, и разделены они намеренно.
    /// Появление в мире — это отметка для сети: с него начинается подключение к
    /// серверу. Экран загрузки после этого держится ещё сколько-то — ровно
    /// столько, сколько идёт подключение, — и уходит по setReady.
    ///
    /// Слитые в один признак, они запирали друг друга: сеть ждала ухода экрана,
    /// а экран ждал сети.
    void setWorldReady();

    /// Экран загрузки может уходить: игрок в мире, а с сервером всё решилось —
    /// либо соединение есть, либо ждать его дольше незачем.
    void setReady();

    /// Появился ли игрок в мире.
    ///
    /// Спрашивает сетевой поток, а отвечает игровой — через ту же общую запись,
    /// что и всё остальное здесь. По этому признаку сеть решает, пора ли вообще
    /// подключаться: до появления игрока отправлять нечего, а числиться в сессии
    /// не за кем.
    [[nodiscard]] bool playerInWorld() const;

    /// Отметка о том, что игровой кадр отработал.
    ///
    /// Ею интерфейс узнаёт, что игра жива. Узнать это иначе он не может, а знать
    /// обязан: пока открыто меню паузы, скриптовый тик не идёт вовсе — и уголок,
    /// нарисованный поверх меню, застыл бы на последнем состоянии. По давности
    /// последней отметки видно, что игра остановилась, и интерфейс убирается сам.
    void beat();
    void setConnection(const Connection& connection);
    void setRoster(std::vector<Participant> roster);

    void pushChat(shared::ChatKind kind, std::string text);
    void pushConsole(unsigned int level, std::string text);

    /// Строка ввода: открыта ли и что в ней набрано.
    ///
    /// Подписи здесь нет и не нужно: просит строку один только чат, и набранное
    /// показывается там же, где его набирают.
    void setInput(bool active, std::string text);

    void setConsoleVisible(bool visible);

    /// Собирает для страницы всё, что изменилось с прошлого раза.
    ///
    /// Ленты отдаются приростом, а не целиком: строки не меняются, меняется их
    /// число, и пересылать всю историю двадцать раз в секунду незачем. Список
    /// игроков — целиком, но только когда сменился: он короток, а собирать его
    /// разницу пришлось бы на обеих сторонах.
    [[nodiscard]] std::string takeUpdate();

    /// Забывает, что страница уже видела.
    ///
    /// Нужно при её перезагрузке: новая страница не знает ничего из того, что
    /// было отдано прежней.
    void forgetDelivered();

private:
    /// Сколько строк хранится, пока их не забрали.
    ///
    /// Ограничение на случай, когда страница ещё не поднялась, а журнал уже
    /// пишет: без него первые секунды запуска копились бы в памяти целиком.
    static constexpr std::size_t kPending = 400;

    static void trim(std::vector<Line>& lines);

    mutable std::mutex mutex_;

    std::string address_;
    std::string nickname_;
    bool sessionChanged_ = true;

    Connection connection_;

    std::vector<Participant> roster_;
    bool rosterChanged_ = true;

    std::vector<Line> chat_;
    std::vector<Line> console_;

    bool inputActive_ = false;
    std::string inputText_;

    bool consoleVisible_ = false;

    shared::LoadStage stage_ = shared::LoadStage::Booting;
    bool worldReady_ = false;
    bool ready_ = false;

    /// Когда игровой кадр отработал в последний раз.
    std::chrono::steady_clock::time_point beatAt_{};
};

} // namespace oxymp::client
