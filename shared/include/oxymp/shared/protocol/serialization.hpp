#pragma once

#include <oxymp/shared/math/vec3.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oxymp::shared {

using ByteView = std::span<const std::uint8_t>;

/// Верхняя граница длины строки в сообщении.
///
/// Существует затем, чтобы испорченный или враждебный пакет не заставил
/// получателя выделить сотни мегабайт под «строку».
inline constexpr std::size_t kMaxStringLength = 256;

/// Сборка сообщения в поток байт.
///
/// Числа пишутся от младшего байта к старшему явно, побайтно. Структуры целиком
/// в сокет не отправляются никогда: их раскладка зависит от выравнивания и
/// настроек компилятора, и расхождение проявится не ошибкой, а тихо испорченными
/// данными.
class ByteWriter {
public:
    void writeU8(std::uint8_t value);
    void writeU16(std::uint16_t value);
    void writeU32(std::uint32_t value);
    void writeU64(std::uint64_t value);
    void writeFloat(float value);
    void writeVec3(const Vec3& value);

    /// Пишет длину строки, а затем её байты. Строка длиннее kMaxStringLength
    /// обрезается: отправитель не должен иметь возможности собрать пакет,
    /// который получатель обязан отвергнуть.
    void writeString(std::string_view value);

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

    [[nodiscard]] std::vector<std::uint8_t> take() && noexcept { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

/// Разбор сообщения из потока байт.
///
/// Чтение за границей данных не бросает исключений и не портит память: оно
/// поднимает флаг неудачи и возвращает нули. Поэтому разбор сообщения пишется
/// линейно, а проверка делается один раз в конце — через ok().
class ByteReader {
public:
    explicit ByteReader(ByteView data) noexcept : data_(data) {}

    [[nodiscard]] std::uint8_t readU8() noexcept;
    [[nodiscard]] std::uint16_t readU16() noexcept;
    [[nodiscard]] std::uint32_t readU32() noexcept;
    [[nodiscard]] std::uint64_t readU64() noexcept;
    [[nodiscard]] float readFloat() noexcept;
    [[nodiscard]] Vec3 readVec3() noexcept;
    [[nodiscard]] std::string readString();

    /// Не было ли попыток прочитать больше, чем есть.
    [[nodiscard]] bool ok() const noexcept { return !failed_; }

    /// Прочитаны ли данные до конца.
    ///
    /// Лишние байты — такой же признак расхождения версий, как и нехватка,
    /// поэтому разбор сообщения считает их ошибкой.
    [[nodiscard]] bool exhausted() const noexcept { return position_ == data_.size(); }

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }

private:
    /// Резервирует count байт. Возвращает false и поднимает флаг, если их нет.
    [[nodiscard]] bool consume(std::size_t count) noexcept;

    ByteView data_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

} // namespace oxymp::shared
