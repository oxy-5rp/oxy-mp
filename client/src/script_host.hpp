#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oxymp::client {

namespace game {
class NativeTable;
}

/// Клиентская скриптовая машина, увиденная со стороны клиента.
///
/// Сама машина живёт отдельной DLL (`client-js`), и это не выбор, а
/// необходимость: клиент собран со статической библиотекой времени выполнения,
/// а движку нужна динамическая. Здесь — то, что её грузит, отвечает на её
/// просьбы и передаёт ей происходящее.
///
/// Отсутствие машины — обычное дело, а не поломка. Клиент, собранный без Node
/// или раздаваемый без неё, работает как прежде: сессия, чат, машины, стрельба.
/// Не работают только ресурсы сервера, и об этом говорится один раз в журнал.
class ScriptHost {
public:
    /// Что машина попросит у клиента.
    ///
    /// Заполняется тем, кто её заводит: сеть, нативы и окна принадлежат клиенту,
    /// а не ей. Пустой обработчик означает «этого здесь нет» — машина отвечает
    /// на такое отказом скрипту, а не тишиной.
    struct Hooks {
        /// Отправить серверу именованное событие.
        std::function<void(std::string_view name, std::string_view payload)> emitServer;

        /// Таблица нативов игры. Пусто — клиент запущен без игры (бот).
        const game::NativeTable* natives = nullptr;

        /// Наш номер в сессии. −1 — сервер ещё не принял.
        ///
        /// Обработчиком, а не числом: номер меняется при переподключении, а
        /// машина спрашивает его тогда, когда он ей понадобился.
        std::function<std::int32_t()> localPlayerId;

        /// Сущность сессии: чем её зовёт сервер и чем её зовёт игра.
        struct Entity {
            std::int32_t id = -1;

            /// Ноль — сущность в сессии есть, а тела у неё здесь ещё нет.
            std::int32_t handle = 0;
        };

        /// Переводчик между номерами сессии и дескрипторами игры.
        ///
        /// Одной связкой, а не россыпью полей: половина перевода была бы хуже
        /// его отсутствия — ресурс получил бы сущность, у которой есть тело, но
        /// нет номера, и отправил бы серверу дескриптор своей игры.
        ///
        /// Спрашивается, а не присылается. Связь эта у каждого игрока своя:
        /// сервер называет всех своими номерами, а тела раздаёт игра, и раздаёт
        /// по-разному — у одного персонаж уже создан, у другого модель ещё
        /// грузится. Присылать её с сервера нечего, потому что сервер её не
        /// знает.
        struct Entities {
            /// Все игроки сессии, включая нас. Без тела — с нулём в handle.
            std::function<std::vector<Entity>()> players;

            /// Все машины сессии.
            std::function<std::vector<Entity>()> vehicles;

            /// Тело по номеру и номер по телу, в обе стороны и для обоих родов.
            std::function<std::int32_t(std::int32_t id)> pedOf;
            std::function<std::int32_t(std::int32_t ped)> playerAt;
            std::function<std::int32_t(std::int32_t id)> carOf;
            std::function<std::int32_t(std::int32_t car)> vehicleAt;

            /// Имя игрока сессии. Пусто — такого игрока нет.
            std::function<std::string(std::int32_t id)> nameOf;
        };

        Entities entities;

        /// Завести окно интерфейса. Ноль — отказ.
        std::function<std::uint32_t(std::string_view resource, std::string_view url)> createView;

        /// Убрать окно.
        std::function<void(std::uint32_t view)> destroyView;

        /// Отдать окну событие.
        std::function<void(std::uint32_t view, std::string_view name,
                           std::string_view payload)> emitView;

        /// Показать окно или спрятать.
        std::function<void(std::uint32_t view, bool visible)> showView;

        /// Отдать окну ввод с клавиатуры или забрать.
        std::function<void(std::uint32_t view, bool focused)> focusView;
    };

    /// Грузит машину из каталога рядом с клиентом.
    ///
    /// Пусто — машины нет или она другой версии; объяснение уходит в journal
    /// вызывающего через error. Это не повод останавливать клиент.
    [[nodiscard]] static std::unique_ptr<ScriptHost> load(
        const std::filesystem::path& clientDirectory, Hooks hooks, std::string& error);

    ~ScriptHost();

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    /// Поднимает ресурс. entry — путь к точке входа от корня ресурса.
    [[nodiscard]] bool startResource(std::string_view name, const std::filesystem::path& root,
                                     std::string_view entry);

    void stopResource(std::string_view name);

    /// Даёт движку поработать. Зовётся раз в кадр.
    void tick();

    /// Событие от сервера — всем поднятым ресурсам.
    void serverEvent(std::string_view name, std::string_view payload);

    /// Событие от страницы интерфейса.
    void viewEvent(std::uint32_t view, std::string_view name, std::string_view payload);

    /// Событие сессии без нагрузки.
    void sessionEvent(std::string_view name);

    /// Нажата или отпущена клавиша.
    void key(std::uint32_t code, bool down);

    /// Всё, что зависит от заголовка границы, спрятано в реализацию.
    ///
    /// Объявлен здесь и открыто, но не определён: собрать его нельзя, а вот
    /// назвать — можно, и это нужно обработчикам, которые машина зовёт обратно.
    /// Они свободные функции (границе нужен указатель на функцию, а не метод), и
    /// закрытый тип им был бы недоступен.
    struct State;

private:
    ScriptHost() = default;

    std::unique_ptr<State> state_;
};

} // namespace oxymp::client
