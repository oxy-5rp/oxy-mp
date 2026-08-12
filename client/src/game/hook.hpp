#pragma once

#include <cstdint>
#include <string>

namespace oxymp::client::game {

/// Механизм перехвата, общий на весь процесс.
///
/// Отдельная сущность, потому что подготовка и снятие делаются ровно по разу, а
/// перехватов бывает много. Снятие обязательно: оставленный перехват — это
/// переход в наш модуль из кода игры, и если модуль выгрузится раньше, игра
/// уйдёт исполнять освобождённую память.
class HookEngine {
public:
    [[nodiscard]] static bool initialise(std::string& error);

    /// Снимает все перехваты и освобождает механизм.
    static void shutdown() noexcept;
};

/// Один перехват функции игры.
///
/// Владеет перехватом целиком: ставит при установке, снимает при разрушении.
/// Копировать нельзя — иначе снятие произошло бы дважды.
class Hook {
public:
    Hook() = default;
    ~Hook();

    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

    /// Ставит перехват: вызовы target начинают приходить в detour.
    ///
    /// Адрес исходной функции сохраняется и доступен через original(): почти
    /// всегда обработчику нужно вызвать то, что он подменил.
    [[nodiscard]] bool install(void* target, void* detour, std::string& error);

    /// Снимает перехват немедленно, не дожидаясь разрушения.
    ///
    /// Нужен там, где важен порядок: пока перехват стоит, обработчик обязан
    /// оставаться работоспособным, поэтому убирать его состояние можно только
    /// после снятия. Повторный вызов безвреден.
    void remove() noexcept;

    /// Исходная функция, приведённая к нужному типу.
    ///
    /// Указатель ведёт не на начало подменённой функции, а на трамплин с её
    /// первыми инструкциями, поэтому вызывать нужно именно его.
    template <typename T>
    [[nodiscard]] T original() const noexcept {
        return reinterpret_cast<T>(original_);
    }

    [[nodiscard]] bool installed() const noexcept { return target_ != nullptr; }

private:
    void* target_ = nullptr;
    void* original_ = nullptr;
};

} // namespace oxymp::client::game
