#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace oxymp::memscan {

/// Байтовая сигнатура машинного кода: последовательность байт, часть которых
/// заранее неизвестна (адреса, смещения, регистры — они меняются от сборки к сборке).
///
/// Текстовый вид — токены через пробел: "48 8B C8 EB ? 33 C9".
/// Заглушка записывается как "?" или "??".
///
/// Разбирается один раз, дальше переиспользуется. Внутри хранит байты и маску,
/// поэтому сравнение обходится без ветвлений: (data[i] & mask[i]) == bytes[i].
class Pattern {
public:
    /// Разбирает текстовый вид сигнатуры.
    ///
    /// Возвращает nullopt, если строка пустая, содержит некорректный токен
    /// или состоит из одних заглушек — такую сигнатуру искать бессмысленно.
    [[nodiscard]] static std::optional<Pattern> parse(std::string_view text);

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    /// Индекс первого точно известного байта. Сканер использует его как якорь,
    /// чтобы пропускать заведомо неподходящие позиции одним memchr.
    [[nodiscard]] std::size_t anchorIndex() const noexcept { return anchorIndex_; }

    [[nodiscard]] std::uint8_t anchorByte() const noexcept { return bytes_[anchorIndex_]; }

    /// Проверяет совпадение, начиная с data.
    /// Вызывающий обязан гарантировать, что по адресу data доступно не менее size() байт.
    [[nodiscard]] bool matchesAt(const std::uint8_t* data) const noexcept;

private:
    Pattern() = default;

    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint8_t> mask_;
    std::size_t anchorIndex_ = 0;
};

} // namespace oxymp::memscan
