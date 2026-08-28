#pragma once

#include <oxymp/script/core.hpp>
#include <oxymp/script/events.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace oxymp::script::js {

/// Движок JavaScript: то, на чём пишутся игровые режимы.
///
/// Внутри — Node.js, но наружу это не видно, и не ради отвлечённой чистоты.
/// Заголовки Node тянут за собой V8, libuv и половину OpenSSL; подключи их
/// сервер — и всякая правка в нём стоила бы пересборки этого хозяйства. Здесь же
/// торчат три действия и ни одного чужого типа.
///
/// Почему это отдельный модуль, а не часть сервера. Сервер знает про файлы на
/// диске, настройки и сокеты; движок — про сущности и события. Между ними стоит
/// `server::Runtime` — переходник в полсотни строк, живущий у сервера, потому
/// что только сервер знает, что такое `ScriptResource`. Движок этого знать не
/// должен: ему довольно имени, каталога и точки входа.
///
/// Заводится один на процесс. Node инициализируется единожды и на всю жизнь
/// процесса — это его собственное правило, а не наше решение.
class Engine {
public:
    virtual ~Engine() = default;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Поднимает движок и подписывает его на события.
    ///
    /// nullptr — с объяснением в error. Ядро и список событий обязаны пережить
    /// движок: он держит на них ссылки и отписывается только при разрушении.
    [[nodiscard]] static std::unique_ptr<Engine> create(Core& core, Events& events,
                                                        std::string& error);

    /// Поднимает ресурс.
    ///
    /// Каждому — своя песочница: свой изолят V8, своя куча, свой цикл событий.
    /// Дороже общей, и намеренно: ресурсы пишут разные люди, и уронивший свою
    /// кучу не должен уносить с собой чужие.
    ///
    /// main задаётся относительно root, и выйти за него не может: путь,
    /// уводящий наружу, отвергается. Рядом с ресурсами лежит и server.cfg.
    virtual bool start(std::string_view name, const std::filesystem::path& root,
                       const std::filesystem::path& main, std::string& error) = 0;

    /// Останавливает. Молча, если он и не поднимался.
    ///
    /// Отвечает, было ли что останавливать. Ответ нужен не движку, а журналу:
    /// строка «Resource stopped» уходит наружу, и написанная о ресурсе, который
    /// и не работал, она врёт — а по журналу разбирают чужие поломки.
    virtual bool stop(std::string_view name) = 0;

    /// Сколько ресурсов поднято прямо сейчас.
    [[nodiscard]] virtual std::size_t running() const noexcept = 0;

protected:
    Engine() = default;
};

} // namespace oxymp::script::js
