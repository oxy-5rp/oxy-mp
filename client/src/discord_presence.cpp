#include "discord_presence.hpp"

#include "game/discord_block.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>
#include <format>

namespace oxymp::client {
namespace {

/// Сколько каналов перебирать.
///
/// Discord открывает канал с номером от нуля; при нескольких запущенных копиях
/// — несколько. Десять с запасом: столько же перебирают все известные
/// реализации, и больше их не бывает.
constexpr int kPipeCount = 10;

/// Коды посылок.
constexpr std::uint32_t kHandshake = 0;
constexpr std::uint32_t kFrame = 1;

/// Как часто пробовать соединение, если Discord не отвечает.
///
/// Редко и намеренно: Discord может быть не установлен вовсе, и долбиться в
/// отсутствующий канал каждую секунду — впустую жечь время сетевого потока.
constexpr auto kReconnectDelay = std::chrono::seconds{30};

/// Не чаще этого Discord принимает изменения.
///
/// У него предел пять посылок за двадцать секунд. Четыре секунды укладываются в
/// него с запасом, а на глаз задержка незаметна: число игроков меняется куда
/// реже.
constexpr auto kSendInterval = std::chrono::seconds{4};

/// Предел длины тела посылки. Больше Discord не принимает, да и нечего слать.
constexpr std::uint32_t kMaxPayload = 64 * 1024;

/// Экранирует то, что попадёт внутрь строки.
///
/// Нужно не для красоты: имя сервера приходит извне, и кавычка в нём иначе
/// развалила бы разбор у Discord.
std::string escape(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);

    for (const char symbol : text) {
        switch (symbol) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            // Управляющие символы Discord не принимает, а взяться они могут из
            // испорченного имени сервера.
            if (static_cast<unsigned char>(symbol) < 0x20) {
                result += ' ';
            } else {
                result += symbol;
            }
            break;
        }
    }

    return result;
}

std::int64_t nowInSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

DiscordPresence::DiscordPresence(std::string clientId) : clientId_(std::move(clientId)) {}

DiscordPresence::~DiscordPresence() {
    disconnect();
}

void DiscordPresence::disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool DiscordPresence::connect() {
    for (int index = 0; index < kPipeCount; ++index) {
        const std::wstring name = std::format(L"\\\\.\\pipe\\discord-ipc-{}", index);

        // Метка «свой»: показ игры в Discord мы затыкаем перехватом этого же
        // вызова, и без метки заткнули бы заодно и себя.
        const game::DiscordBlock::Ours ours;

        const HANDLE pipe = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            continue;
        }

        pipe_ = pipe;

        // Опознание обязательно и идёт первым: до него Discord не примет
        // ничего, а канал закроет.
        if (!send(kHandshake, std::format(R"({{"v":1,"client_id":"{}"}})", escape(clientId_)))) {
            disconnect();
            continue;
        }

        spdlog::debug("Discord найден на канале {}", index);
        return true;
    }

    return false;
}

void DiscordPresence::pump() {
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return;
    }

    for (;;) {
        DWORD available = 0;
        if (::PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            spdlog::info("Discord закрыл канал");
            disconnect();
            return;
        }

        // Заголовок ответа такой же, как у посылки: код и длина.
        if (available < sizeof(std::uint32_t) * 2) {
            return;
        }

        std::array<std::uint32_t, 2> header{};
        DWORD read = 0;
        if (::ReadFile(pipe_, header.data(), sizeof(header), &read, nullptr) == FALSE ||
            read != sizeof(header)) {
            disconnect();
            return;
        }

        const std::uint32_t length = header[1];
        if (length == 0 || length > kMaxPayload) {
            return;
        }

        std::string body;
        body.resize(length);

        if (::ReadFile(pipe_, body.data(), length, &read, nullptr) == FALSE || read != length) {
            disconnect();
            return;
        }

        // В журнал попадает только отказ и только первое подтверждение.
        //
        // Подтверждение приходит на каждую посылку, то есть раз в четыре
        // секунды, и содержит всё, что мы отправили. Записывать их подряд —
        // значит утопить журнал: за полчаса игры от него не остаётся ничего,
        // кроме Discord, а разбирать по нему приходится совсем другое.
        //
        // Первое всё же нужно: по нему видно, что показ вообще принят.
        if (body.find("\"evt\":\"ERROR\"") != std::string::npos ||
            body.find("\"code\"") != std::string::npos) {
            spdlog::warn("Discord отказал: {}", body);
        } else if (!confirmed_) {
            confirmed_ = true;
            spdlog::debug("Discord принял показ: {}", body);
        }
    }
}

bool DiscordPresence::send(std::uint32_t opcode, const std::string& payload) {
    if (pipe_ == INVALID_HANDLE_VALUE || payload.size() > kMaxPayload) {
        return false;
    }

    // Заголовок — два четырёхбайтных числа: код посылки и длина тела. Пишется
    // вместе с телом одним вызовом: канал сообщениями не разделён, и раздельная
    // запись позволила бы прерваться между заголовком и телом.
    std::string frame;
    frame.resize(sizeof(std::uint32_t) * 2 + payload.size());

    const auto length = static_cast<std::uint32_t>(payload.size());
    std::memcpy(frame.data(), &opcode, sizeof(opcode));
    std::memcpy(frame.data() + sizeof(opcode), &length, sizeof(length));
    std::memcpy(frame.data() + sizeof(opcode) + sizeof(length), payload.data(), payload.size());

    DWORD written = 0;
    const BOOL ok = ::WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written,
                                nullptr);

    return ok != FALSE && written == frame.size();
}

std::string DiscordPresence::describe(const Presence& presence) const {
    // Первая строка — где играет, вторая — кто и сколько народу. Порядок не
    // случаен: Discord показывает первую крупнее, а на вопрос «куда зайти, чтобы
    // играть вместе» отвечает именно адрес.
    //
    // Показ идёт и до всякого сервера: игрок, выбирающий его в меню, уже
    // запустил oxyMP. Прежде показ начинался с первой попытки подключения, и
    // человек, стоящий в меню, для Discord не играл ни во что.
    const std::string details =
        presence.inMenu ? std::string{"В меню"}
                        : std::format("Сервер {}", presence.server.empty()
                                                       ? std::string{"неизвестно"}
                                                       : escape(presence.server));

    const std::string state =
        presence.inMenu ? std::string{"Выбирает сервер"}
        : presence.connected
            ? std::format("ID {} · Игроков: {}", presence.playerId, presence.players)
            : std::string{"Подключение…"};

    // Картинки здесь намеренно нет.
    //
    // Discord принимает ссылку на картинку только если она загружена в
    // настройках приложения, а на незагруженную отвечает отказом — и отвергает
    // при этом всю посылку целиком, вместе с текстом. Пока картинка не залита,
    // просить её значит остаться вовсе без показа.
    return std::format(
        R"({{"cmd":"SET_ACTIVITY","nonce":"{}","args":{{"pid":{},"activity":{{)"
        R"("details":"{}","state":"{}","timestamps":{{"start":{}}}}}}}}})",
        nonce_, ::GetCurrentProcessId(), details, state, startedAt_);
}

void DiscordPresence::update(const Presence& presence) {
    const Clock::time_point now = Clock::now();

    if (startedAt_ == 0) {
        startedAt_ = nowInSeconds();
    }

    // Ответы разбираются до отправки нового: там могут лежать и отказ на
    // прошлую посылку, и известие о том, что Discord закрыли.
    pump();

    if (pipe_ == INVALID_HANDLE_VALUE) {
        if (now < nextAttemptAt_) {
            return;
        }

        // Срок следующей попытки назначается до неё самой: неудача не должна
        // приводить к попытке на каждом обороте.
        nextAttemptAt_ = now + kReconnectDelay;

        if (!connect()) {
            return;
        }
    }

    std::string payload = describe(presence);

    // Не изменилось — не шлём. Discord показывает то же самое, а предел посылок
    // лучше поберечь на случай, когда изменится что-то важное.
    if (payload == sent_) {
        return;
    }

    if (now < nextSendAt_) {
        return;
    }

    if (!send(kFrame, payload)) {
        // Discord закрыли. Это не ошибка: канал просто исчез, и через положенное
        // время мы поищем его снова.
        spdlog::info("Discord отключился");
        disconnect();
        nextAttemptAt_ = now + kReconnectDelay;
        return;
    }

    sent_ = std::move(payload);
    nextSendAt_ = now + kSendInterval;
    ++nonce_;
}

} // namespace oxymp::client
