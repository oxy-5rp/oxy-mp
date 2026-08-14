#include "ui_feed.hpp"

#include <format>

namespace oxymp::client {
namespace {

/// Обезвреживает строку для JSON.
///
/// Управляющие символы в JSON запрещены как есть: строка с переводом строки
/// посреди неё не разобралась бы вовсе, и лента молча остановилась бы на ней
/// навсегда.
std::string escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());

    for (const char symbol : text) {
        if (symbol == '"' || symbol == '\\') {
            escaped += '\\';
            escaped += symbol;
        } else if (static_cast<unsigned char>(symbol) < 0x20) {
            escaped += std::format("\\u{:04x}", static_cast<unsigned int>(symbol));
        } else {
            escaped += symbol;
        }
    }

    return escaped;
}

std::string linesToJson(const std::vector<UiFeed::Line>& lines) {
    std::string collected = "[";

    for (const UiFeed::Line& line : lines) {
        if (collected.size() > 1) {
            collected += ',';
        }

        collected += std::format(R"({{"kind":{},"text":"{}"}})", line.kind, escape(line.text));
    }

    collected += ']';
    return collected;
}

/// Насколько давно должен был отработать кадр, чтобы счесть игру остановленной.
constexpr auto kTickTimeout = std::chrono::milliseconds{250};

} // namespace

void UiFeed::trim(std::vector<Line>& lines) {
    if (lines.size() <= kPending) {
        return;
    }

    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - kPending));
}

void UiFeed::describeSession(std::string address, std::string nickname) {
    const std::lock_guard guard{mutex_};

    address_ = std::move(address);
    nickname_ = std::move(nickname);
    sessionChanged_ = true;
}

void UiFeed::setStage(shared::LoadStage stage) {
    const std::lock_guard guard{mutex_};
    stage_ = stage;
}

void UiFeed::setWorldReady() {
    const std::lock_guard guard{mutex_};
    worldReady_ = true;
}

void UiFeed::setReady() {
    const std::lock_guard guard{mutex_};
    ready_ = true;
}

bool UiFeed::playerInWorld() const {
    const std::lock_guard guard{mutex_};
    return worldReady_;
}

void UiFeed::beat() {
    const std::lock_guard guard{mutex_};
    beatAt_ = std::chrono::steady_clock::now();
}

void UiFeed::setConnection(const Connection& connection) {
    const std::lock_guard guard{mutex_};
    connection_ = connection;
}

void UiFeed::setRoster(std::vector<Participant> roster) {
    const std::lock_guard guard{mutex_};

    // Сравнение, а не безусловная замена: список приходит каждый оборот
    // сетевого потока, а меняется считаные разы за сессию. Отдавать его
    // странице заново двадцать раз в секунду — значит перестраивать её разметку
    // на ровном месте.
    if (roster.size() == roster_.size()) {
        bool same = true;

        for (std::size_t i = 0; i < roster.size(); ++i) {
            if (roster[i].id != roster_[i].id || roster[i].nickname != roster_[i].nickname) {
                same = false;
                break;
            }
        }

        if (same) {
            return;
        }
    }

    roster_ = std::move(roster);
    rosterChanged_ = true;
}

void UiFeed::pushChat(shared::ChatKind kind, std::string text) {
    const std::lock_guard guard{mutex_};

    chat_.push_back(Line{.kind = static_cast<unsigned int>(kind), .text = std::move(text)});
    trim(chat_);
}

void UiFeed::pushConsole(unsigned int level, std::string text) {
    const std::lock_guard guard{mutex_};

    console_.push_back(Line{.kind = level, .text = std::move(text)});
    trim(console_);
}

void UiFeed::setInput(bool active, std::string text) {
    const std::lock_guard guard{mutex_};

    inputActive_ = active;
    inputText_ = std::move(text);
}

void UiFeed::setConsoleVisible(bool visible) {
    const std::lock_guard guard{mutex_};
    consoleVisible_ = visible;
}

void UiFeed::forgetDelivered() {
    const std::lock_guard guard{mutex_};

    sessionChanged_ = true;
    rosterChanged_ = true;
}

std::string UiFeed::takeUpdate() {
    const std::lock_guard guard{mutex_};

    // Жива ли игра, видно по давности последнего кадра. Четверть секунды — это
    // пятнадцать пропущенных кадров: столько не пропускает даже самая тяжёлая
    // подгрузка, а меню паузы останавливает тик насовсем.
    const bool alive = beatAt_ != std::chrono::steady_clock::time_point{} &&
                       std::chrono::steady_clock::now() - beatAt_ < kTickTimeout;

    std::string message = std::format(
        R"({{"connection":{},"players":{},"latency":{},"playerId":{},"troubled":{},)"
        R"("consoleVisible":{},"inputActive":{},"inputText":"{}",)"
        R"("stage":{},"ready":{},"alive":{},"money":{},)"
        R"("disconnect":{},"disconnectDetail":"{}")",
        connection_.state, connection_.players, connection_.latencyMilliseconds,
        // Отсутствие номера доходит до страницы отрицательным числом, а не
        // огромным: наибольшее беззнаковое выглядит как настоящий номер игрока,
        // и страница показала бы «id 4294967295».
        connection_.playerId == shared::kInvalidPlayerId
            ? -1
            : static_cast<std::int64_t>(connection_.playerId),
        connection_.troubled ? 1 : 0, consoleVisible_ ? 1 : 0, inputActive_ ? 1 : 0,
        escape(inputText_), static_cast<unsigned int>(stage_),
        ready_ ? 1 : 0, alive ? 1 : 0,
        // Неизвестные деньги доходят до страницы как null, а не как ноль: ноль —
        // это разорение, а нам нужно «сервер ещё не сказал».
        connection_.money.has_value() ? std::format("{}", *connection_.money) : "null",
        connection_.disconnect, escape(connection_.disconnectDetail));

    if (sessionChanged_) {
        sessionChanged_ = false;
        message += std::format(R"(,"address":"{}","nickname":"{}")", escape(address_),
                               escape(nickname_));
    }

    if (rosterChanged_) {
        rosterChanged_ = false;

        message += R"(,"roster":[)";

        for (std::size_t i = 0; i < roster_.size(); ++i) {
            if (i != 0) {
                message += ',';
            }

            message += std::format(R"({{"id":{},"name":"{}"}})", roster_[i].id,
                                   escape(roster_[i].nickname));
        }

        message += ']';
    }

    if (!chat_.empty()) {
        message += std::format(R"(,"chat":{})", linesToJson(chat_));
        chat_.clear();
    }

    if (!console_.empty()) {
        message += std::format(R"(,"console":{})", linesToJson(console_));
        console_.clear();
    }

    message += '}';
    return message;
}

} // namespace oxymp::client
