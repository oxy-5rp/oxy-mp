#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace oxymp::rglpatch {

/// Подмена одной записи в таблице импорта модуля.
///
/// Врезка в код здесь была бы лишней. Лаунчер вызывает CreateProcessW через
/// таблицу импорта, а таблица — это просто массив указателей: заменить в нём
/// один — значит перехватить все вызовы этого модуля, не тронув ни байта чужого
/// кода. Заодно перехват не задевает остальных: kernel32 общая на процесс, а
/// таблица импорта у каждого модуля своя.
class ImportHook {
public:
    ~ImportHook();

    ImportHook(const ImportHook&) = delete;
    ImportHook& operator=(const ImportHook&) = delete;

    /// Ставит перехват в таблице импорта указанного модуля.
    ///
    /// Имя библиотеки не спрашивается намеренно: одна и та же функция приходит
    /// то из kernel32, то из api-ms-win-core-*, и завязываться на то, какой из
    /// них выбрал компоновщик, — значит сломаться на следующей сборке лаунчера.
    /// Ищется имя функции по всем библиотекам сразу.
    [[nodiscard]] static std::unique_ptr<ImportHook> install(void* module,
                                                             std::string_view function,
                                                             void* replacement,
                                                             std::string& error);

    /// Исходная функция, приведённая к нужному типу.
    template <typename T>
    [[nodiscard]] T original() const noexcept {
        return reinterpret_cast<T>(original_);
    }

private:
    ImportHook() = default;

    /// Ячейка таблицы, в которую записан наш адрес.
    void** slot_ = nullptr;

    void* original_ = nullptr;
};

} // namespace oxymp::rglpatch
