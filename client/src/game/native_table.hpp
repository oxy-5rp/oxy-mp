#pragma once

#include "engine_addresses.hpp"

#include <cstdint>
#include <vector>

namespace oxymp::client::game {

/// Обработчик нативной функции игры.
///
/// Аргументов в привычном смысле не принимает: и доводы, и место под результат
/// лежат в контексте вызова, указатель на который и передаётся.
using NativeHandler = void (*)(void* context);

/// Поиск обработчиков нативов по хешу.
///
/// Таблица регистрации обфусцирована: корзины связаны списками, а счётчики,
/// хеши и указатели зашифрованы через XOR с собственным адресом. Разбирать это
/// не нужно и не следует — игра снимает обфускацию сама, и мы пользуемся её же
/// функцией поиска. Схема шифрования остаётся её внутренним делом, а значит
/// переживёт её изменение без единой правки у нас.
class NativeTable {
public:
    /// Берёт адреса таблицы и функции поиска из разрешённого каталога.
    explicit NativeTable(const EngineAddresses& addresses) noexcept;

    /// Обработчик натива либо nullptr, если натива с таким хешем в игре нет.
    [[nodiscard]] NativeHandler handlerFor(std::uint64_t hash) const noexcept;

    /// Хеши, зарегистрированные в таблице, не более указанного числа.
    ///
    /// Единственное место, где обфускация разбирается своими силами, и нужно оно
    /// не для работы, а для сверки: по хешу отсюда поиск обязан находить
    /// обработчик. Не находит — сломан вызов; находит, а канонический хеш из
    /// открытой базы не находится — значит в этой сборке у натива другой хеш.
    ///
    /// Схема снята с дизассемблера самой функции поиска: счётчик и хеши
    /// зашифрованы XOR-ом с собственным адресом записи.
    [[nodiscard]] std::vector<std::uint64_t> registeredHashes(std::size_t limit) const;

    /// Сколько нативов всего зарегистрировано.
    [[nodiscard]] std::size_t registeredCount() const;

    /// Готова ли таблица к работе.
    [[nodiscard]] bool valid() const noexcept { return table_ != nullptr && lookup_ != nullptr; }

private:
    /// Функция игры: принимает таблицу и хеш, возвращает обработчик.
    using LookupFunction = NativeHandler (*)(void* table, std::uint64_t hash);

    void* table_ = nullptr;
    LookupFunction lookup_ = nullptr;
};

} // namespace oxymp::client::game
