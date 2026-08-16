#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oxymp::config {

/// Настройки клиента: всё, что переживает запуск.
///
/// Лежат в `oxymp.toml` рядом с лаунчером, и поля названы так же, как у alt:V в
/// `altv.toml`. Совпадение намеренное: игрок, знающий один файл, узнаёт второй
/// без объяснений, а страница меню взята оттуда же и спрашивает поля по их
/// тамошним именам. Своё название означало бы правку страницы при каждом её
/// обновлении.
///
/// Хранилище по имени, а не набор полей, и это не лень. Настроек четыре десятка,
/// страница правит их по одной — `settings:change(имя, значение)`, — и всякое
/// новое поле при наборе полей пришлось бы вписывать в четыре места: в
/// объявление, в чтение, в запись и в разбор правки. По имени оно вписывается в
/// одно — в таблицу умолчаний.
///
/// Умолчания — источник истины о том, какие настройки вообще есть. Чего нет в
/// них, того нет и в файле: неизвестное имя из файла не читается, неизвестное имя
/// от страницы не принимается. Иначе первая же опечатка завела бы настройку-двойник,
/// которую никто не читает.
class Settings {
public:
    /// Читает настройки из файла.
    ///
    /// Ненайденный файл — не беда, а обычное первое знакомство: получатся
    /// умолчания. Испорченный файл тоже не беда: о нём пишется в журнал, и
    /// получаются те же умолчания. Отказаться запускать игру из-за настроек —
    /// худшее, что можно сделать с человеком, который просто хочет играть.
    [[nodiscard]] static Settings load(const std::filesystem::path& file);

    /// Записывает настройки в файл. Каталог заводится при надобности.
    bool save(const std::filesystem::path& file) const;

    Settings();
    ~Settings();

    Settings(Settings&&) noexcept;
    Settings& operator=(Settings&&) noexcept;

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    [[nodiscard]] bool flag(std::string_view key) const;
    [[nodiscard]] std::int64_t number(std::string_view key) const;
    [[nodiscard]] double fraction(std::string_view key) const;
    [[nodiscard]] std::string text(std::string_view key) const;
    [[nodiscard]] std::vector<std::string> list(std::string_view key) const;

    void set(std::string_view key, bool value);
    void set(std::string_view key, std::int64_t value);
    void set(std::string_view key, double value);
    void set(std::string_view key, std::string_view value);

    /// Всё, что есть, — строкой JSON.
    ///
    /// Ровно то, что страница ждёт в `settings:update`: объект, у которого имена
    /// полей совпадают с именами настроек.
    [[nodiscard]] std::string toJson() const;

    /// Принимает правку от страницы.
    ///
    /// value — запись значения в JSON: `true`, `142`, `"Игрок"`, `["a","b"]`.
    /// Приходит она именно так, потому что страница шлёт доводы события строкой
    /// JSON, и разбирать их до значения здесь — единственное место, где известно,
    /// какого вида должна быть эта настройка.
    ///
    /// Вид сохраняется прежним. Страница шлёт `1` и для целого, и для дробного —
    /// в JavaScript это одно и то же число, — и принять его как целое значило бы
    /// однажды записать `consoleHeight = 1` вместо `1.0` и получить файл, который
    /// сама же и не прочтёт.
    ///
    /// Возвращает false, если имя неизвестно или значение не разобралось.
    bool applyJson(std::string_view key, std::string_view value);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace oxymp::config
