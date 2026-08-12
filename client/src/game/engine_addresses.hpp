#pragma once

#include <oxymp/gamesig/image_source.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace oxymp::client::game {

/// Адреса движка игры, разрешённые для текущего запуска.
///
/// Разрешаются один раз: поиск по десяткам мегабайт кода стоит заметного
/// времени, а внутри одного процесса адреса больше не меняются.
///
/// Владелец всех «магических» адресов в клиенте. Всё остальное спрашивает их
/// по идентификатору из каталога и не знает ни про сигнатуры, ни про то, как
/// они устроены.
class EngineAddresses {
public:
    /// Разрешает весь каталог по образу игры.
    ///
    /// Отказывает при первой же неприятности, и это намеренно. Работать с
    /// половиной адресов нельзя: вызов по неверно разрешённому адресу уронит
    /// игру в случайном месте и без объяснений, тогда как честный отказ на
    /// старте прямо называет причину.
    ///
    /// Сначала сверяется дата сборки игры с той, под которую выверен каталог:
    /// после обновления GTA байты сдвигаются, и часть сигнатур совпадёт не там,
    /// где нужно, оставаясь при этом формально однозначной.
    /// Почему опознание не удалось.
    enum class Failure {
        /// Пока нечего искать: игра ещё не развернула свой код. Пройдёт само,
        /// нужно лишь повторить попытку позже.
        NotReady,

        /// Игра другой сборки. Само не пройдёт и повторами не лечится: под эту
        /// версию каталог не выверялся, и совпадений в ней не будет никогда.
        WrongBuild,
    };

    [[nodiscard]] static std::unique_ptr<EngineAddresses> resolveCatalog(
        const gamesig::ImageSource& image, std::string& error, Failure& failure);

    /// Адрес по идентификатору сигнатуры.
    ///
    /// Идентификатора не из каталога быть не может: он проверяется на этапе
    /// разрешения, поэтому здесь остаётся только достать готовое.
    [[nodiscard]] std::uint64_t operator[](std::string_view id) const noexcept;

    /// То же самое сразу нужным указателем.
    template <typename T>
    [[nodiscard]] T pointerTo(std::string_view id) const noexcept {
        static_assert(std::is_pointer_v<T>, "ожидается тип указателя");
        return reinterpret_cast<T>(static_cast<std::uintptr_t>((*this)[id]));
    }

    [[nodiscard]] const std::string& buildDate() const noexcept { return buildDate_; }

private:
    EngineAddresses() = default;

    /// Ключ — идентификатор из каталога, значение — разрешённый адрес либо
    /// константа, смотря что описывает сигнатура.
    std::unordered_map<std::string, std::uint64_t> addresses_;

    std::string buildDate_;
};

} // namespace oxymp::client::game
