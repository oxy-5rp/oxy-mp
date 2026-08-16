#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace oxymp::client {

/// Куда игрок заходил и что отметил звёздочкой.
///
/// Отдельным файлом рядом с клиентом — `history.servers`, — а не в `oxymp.toml`.
/// Причина в том, чем эти два файла отличаются: настройки правит человек, и в
/// них лежит то, что он сам выбрал; история же ведётся сама и растёт при каждом
/// входе. Смешав их, мы заставили бы блокнот открывать список из полусотни
/// адресов там, где человек искал имя игрока.
///
/// Формат — JSON, и это тоже намеренно: файл видно глазами, его можно поправить
/// и удалить, а испорченный он просто читается пустым.
class ServerHistory {
public:
    /// Один сервер, каким его показывает страница.
    struct Entry {
        /// Имя, каким сервер назвался. Пока не назвался — адрес.
        std::string name;

        /// Опознаватель из оформления. Пусто у тех, к кому шли по адресу.
        std::string id;

        /// Адрес, по которому к нему возвращаются.
        std::string url;
    };

    /// Читает файл. Отсутствующий и испорченный — это пустая история.
    [[nodiscard]] static ServerHistory load(const std::filesystem::path& file);

    /// Записывает файл. Неудача не смертельна: история — не настройка.
    void save(const std::filesystem::path& file) const;

    [[nodiscard]] const std::vector<Entry>& recent() const noexcept { return recent_; }
    [[nodiscard]] const std::vector<Entry>& favorite() const noexcept { return favorite_; }

    /// Отмечает вход на сервер.
    ///
    /// Тот же адрес не заводит второй записи, а поднимается наверх: список
    /// недавних отвечает на вопрос «куда я ходил в последний раз», и повторный
    /// вход именно это и меняет.
    void visited(Entry entry);

    /// Добавляет сервер в избранное. Повторное добавление ничего не меняет.
    void addFavorite(Entry entry);

    /// Убирает сервер из избранного по опознавателю.
    void removeFavorite(std::string_view id);

private:
    /// Сколько недавних помнится.
    ///
    /// Больше страница всё равно не показывает, а расти без предела файлу
    /// незачем: история сервера, куда зашли однажды год назад, не нужна никому.
    static constexpr std::size_t kMaxRecent = 32;

    std::vector<Entry> recent_;
    std::vector<Entry> favorite_;
};

} // namespace oxymp::client
