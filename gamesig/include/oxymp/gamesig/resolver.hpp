#pragma once

#include "image_source.hpp"
#include "signature.hpp"

#include <cstdint>
#include <string>

namespace oxymp::gamesig {

/// Чем закончилась попытка разрешить сигнатуру.
enum class ResolveStatus {
    Ok,             ///< Ровно столько совпадений, сколько ожидалось.
    PatternInvalid, ///< Сигнатура записана некорректно — это ошибка в каталоге.
    NotFound,       ///< Совпадений нет: сборка игры не та, под которую выверен каталог.
    Ambiguous,      ///< Совпадений больше ожидаемого; доверять адресу нельзя.
    Unreadable,     ///< Совпадение найдено, но по вычисленному адресу нет данных.
};

/// Разрешённая сигнатура.
struct Resolved {
    ResolveStatus status = ResolveStatus::NotFound;

    /// Сколько раз сигнатура встретилась в исполняемых секциях.
    std::size_t matchCount = 0;

    /// RVA места, куда указывает сигнатура после применения смещения.
    std::uint64_t siteRva = 0;

    /// RVA того, на что это место ссылается. Для Immediate32 совпадает с siteRva:
    /// там лежит константа, а не ссылка.
    std::uint64_t targetRva = 0;

    /// Итог разрешения: адрес внутри образа либо считанная константа.
    std::uint64_t value = 0;

    /// Подробность отказа, если он был.
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return status == ResolveStatus::Ok; }
};

/// Ищет сигнатуру в исполняемых секциях образа и приводит находку к адресу.
///
/// Единственная реализация разрешения на весь проект: ею пользуется и оффлайн-
/// проверка каталога, и клиент, которому те же адреса нужны изнутри игры.
/// Расхождение между «проверено» и «используется» здесь недопустимо — иначе
/// sigcheck подтверждал бы одно, а клиент вызывал другое.
[[nodiscard]] Resolved resolve(const ImageSource& image, const Signature& signature);

/// Человекочитаемое описание исхода.
[[nodiscard]] std::string_view describe(ResolveStatus status) noexcept;

/// Читает строку, оканчивающуюся нулём, по указанному RVA.
///
/// Пусто, если данных нет или встретился непечатаемый байт: так отсеиваются
/// случаи, когда сигнатура разрешилась в мусор, а не в настоящую строку.
/// Нужна обеим сторонам — и проверке каталога, и клиенту, — чтобы сверить дату
/// сборки игры с той, под которую каталог выверен.
[[nodiscard]] std::string readCString(const ImageSource& image, std::uint64_t rva,
                                      std::size_t maxLength);

} // namespace oxymp::gamesig
