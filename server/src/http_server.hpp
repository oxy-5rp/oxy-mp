#pragma once

#include "resource_store.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace oxymp::server {

/// Раздача ресурсов по HTTP.
///
/// Маленький сайт на том же порту, что и игра, только по TCP: игровой обмен идёт
/// по UDP, и номера портов у них не спорят. Клиент забирает ресурс обычным
/// запросом:
///
///     GET /resources/dlcpacks/<отпечаток>.resource
///
/// Почему HTTP, а не своя раздача поверх игрового соединения. Файлы бывают в
/// десятки мегабайт, и тащить их тем же каналом, которым идёт состояние игроков,
/// значит заставить всех ждать одного качающего. HTTP же умеет всё нужное
/// готовым: докачку, кеширование, любой браузер как средство проверки.
///
/// Что отдаётся. Только ресурсы, только по отпечатку, только целиком. Ни списка
/// файлов, ни обхода каталогов, ни путей с `..` — обращений к файловой системе
/// здесь нет вовсе: всё лежит в памяти и ищется по отпечатку.
class HttpServer {
public:
    /// Поднимает раздачу. Пусто, если порт занять не удалось.
    ///
    /// store обязан пережить сервер: тот держит на него ссылку и читает из него
    /// из своего потока.
    [[nodiscard]] static std::unique_ptr<HttpServer> start(std::uint16_t port,
                                                            const ResourceStore& store,
                                                            std::string& error);

    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

private:
    HttpServer() = default;

    void serve();
    void handle(std::intptr_t client);

    const ResourceStore* store_ = nullptr;

    std::intptr_t listener_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

} // namespace oxymp::server
