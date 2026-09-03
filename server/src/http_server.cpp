#include "http_server.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <format>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace oxymp::server {
namespace {

/// Начало пути, по которому раздаются ресурсы.
constexpr std::string_view kResourcePath = "/resources/dlcpacks/";

/// Расширение, под которым они раздаются.
constexpr std::string_view kResourceSuffix = ".resource";

/// Сколько байт запроса вообще читать.
///
/// Запрос за это не выходит: строка запроса плюс несколько заголовков. Предел
/// нужен, чтобы бесконечный поток от кого угодно не съел память сервера.
constexpr std::size_t kMaxRequestLength = 8192;

void closeSocket(SocketHandle socket) {
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

/// Сколько ждать чтения или отправки одному клиенту, в секундах.
///
/// Без предела recv/send ждут вечно: клиент, подключившийся и не сказавший ни
/// слова (или подтвердивший приём и не читающий дальше), держит их
/// заблокированными до своего отключения — а раздача при этом одна на всех и
/// однопоточная, так что зависший чужой сокет останавливает скачивание всем
/// остальным до того же мгновения. Оно же держит и остановку сервера:
/// деструктор ждёт этот самый поток через join().
constexpr int kClientTimeoutSeconds = 10;

/// Ставит предел на recv/send для одного клиентского сокета.
///
/// Молча — отказ здесь не повод не принять клиента: без предела соединение
/// просто вернётся к поведению по умолчанию, то есть к тому, что уже было.
void applyClientTimeout(SocketHandle client) {
#ifdef _WIN32
    const DWORD timeoutMs = kClientTimeoutSeconds * 1000;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
                 sizeof(timeoutMs));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs),
                 sizeof(timeoutMs));
#else
    timeval timeout{.tv_sec = kClientTimeoutSeconds, .tv_usec = 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool sendAll(SocketHandle socket, const char* data, std::size_t length) {
    std::size_t sent = 0;

    while (sent < length) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(length - sent, 64 * 1024));

#ifdef _WIN32
        const int written = ::send(socket, data + sent, chunk, 0);
#else
        const ssize_t written = ::send(socket, data + sent, static_cast<std::size_t>(chunk), 0);
#endif

        if (written <= 0) {
            return false;
        }

        sent += static_cast<std::size_t>(written);
    }

    return true;
}

/// Отвечает коротким текстом. Тело нужно даже на отказ: без него браузер
/// показывает пустую страницу и человек не понимает, что произошло.
void replyWith(SocketHandle socket, int code, std::string_view meaning, std::string_view body) {
    const std::string head =
        std::format("HTTP/1.1 {} {}\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: {}\r\n"
                    "Connection: close\r\n\r\n",
                    code, meaning, body.size());

    sendAll(socket, head.data(), head.size());
    sendAll(socket, body.data(), body.size());
}

/// Достаёт путь из строки запроса. Пусто — запрос не тот.
std::string_view pathOf(std::string_view request) {
    // Только GET: ничего другого здесь не делают, а разбирать остальное значит
    // заводить возможности, которых никто не просил.
    if (!request.starts_with("GET ")) {
        return {};
    }

    const std::size_t start = 4;
    const std::size_t end = request.find(' ', start);

    if (end == std::string_view::npos) {
        return {};
    }

    return request.substr(start, end - start);
}

} // namespace

std::unique_ptr<HttpServer> HttpServer::start(std::uint16_t port, const ResourceStore& store,
                                              std::string& error) {
#ifdef _WIN32
    // Winsock поднимается один раз на процесс. ENet его уже поднял, но
    // полагаться на чужой запуск нельзя: счётчик обращений на то и есть.
    WSADATA winsock{};
    if (::WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        error = "не удалось поднять Winsock";
        return nullptr;
    }
#endif

    const SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        error = "не удалось создать сокет раздачи";
        return nullptr;
    }

    // Без этого перезапуск сервера упирается в занятый порт: закрытое соединение
    // держит его ещё пару минут.
    const int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = ::htons(port);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(listener);
        error = std::format("порт {} для раздачи ресурсов занят", port);
        return nullptr;
    }

    if (::listen(listener, 16) != 0) {
        closeSocket(listener);
        error = "не удалось начать слушать порт раздачи";
        return nullptr;
    }

    std::unique_ptr<HttpServer> server{new HttpServer};
    server->store_ = &store;
    server->listener_ = static_cast<std::intptr_t>(listener);
    server->worker_ = std::thread{[raw = server.get()] { raw->serve(); }};

    spdlog::info("Resource delivery listening on port {} (http)", port);

    return server;
}

HttpServer::~HttpServer() {
    stopping_.store(true);

    // Закрытие слушающего сокета вышибает поток из ожидания подключения. Иначе
    // он остался бы висеть в accept до первого обращения, которого может не
    // случиться никогда.
    if (listener_ != -1) {
        closeSocket(static_cast<SocketHandle>(listener_));
        listener_ = -1;
    }

    if (worker_.joinable()) {
        worker_.join();
    }
}

void HttpServer::serve() {
    while (!stopping_.load()) {
        const SocketHandle client = ::accept(static_cast<SocketHandle>(listener_), nullptr,
                                             nullptr);

        if (client == kInvalidSocket) {
            // Слушающий сокет закрыли — это наш же способ остановиться.
            if (stopping_.load()) {
                return;
            }
            continue;
        }

        handle(static_cast<std::intptr_t>(client));
    }
}

void HttpServer::handle(std::intptr_t raw) {
    const auto client = static_cast<SocketHandle>(raw);

    applyClientTimeout(client);

    std::string request;
    std::array<char, 2048> buffer{};

    // Читаем до конца заголовков. Тело нас не интересует: у GET его нет.
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < kMaxRequestLength) {
#ifdef _WIN32
        const int received = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const ssize_t received = ::recv(client, buffer.data(), buffer.size(), 0);
#endif

        if (received <= 0) {
            closeSocket(client);
            return;
        }

        request.append(buffer.data(), static_cast<std::size_t>(received));
    }

    const std::string_view path = pathOf(request);

    if (path.empty()) {
        replyWith(client, 400, "Bad Request", "oxyMP раздаёт только ресурсы\n");
        closeSocket(client);
        return;
    }

    if (!path.starts_with(kResourcePath) || !path.ends_with(kResourceSuffix)) {
        replyWith(client, 404, "Not Found", "здесь ничего нет\n");
        closeSocket(client);
        return;
    }

    // Имя между началом пути и расширением — это и есть отпечаток. Ничем иным
    // оно быть не может: поиск идёт по памяти, а не по файловой системе, и
    // подсунуть сюда путь наружу невозможно в принципе.
    const std::string hash{
        path.substr(kResourcePath.size(),
                    path.size() - kResourcePath.size() - kResourceSuffix.size())};

    const std::vector<std::uint8_t>* const content = store_->find(hash);

    if (content == nullptr) {
        replyWith(client, 404, "Not Found", "такого ресурса нет\n");
        closeSocket(client);
        return;
    }

    const std::string head =
        std::format("HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Content-Length: {}\r\n"
                    "Connection: close\r\n\r\n",
                    content->size());

    if (sendAll(client, head.data(), head.size())) {
        sendAll(client, reinterpret_cast<const char*>(content->data()), content->size());
    }

    spdlog::debug("отдан ресурс {}... ({} КБ)", hash.substr(0, 12), content->size() / 1024);

    closeSocket(client);
}

} // namespace oxymp::server
