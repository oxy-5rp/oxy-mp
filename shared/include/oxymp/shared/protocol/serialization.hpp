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

/// Верхняя граница длины «полотна» — строки, которой заведомо мало обычного
/// предела.
///
/// Такая в протоколе одна: полезная нагрузка именованного события. Ею ходит
/// разметка интерфейса, сочинённая ресурсом сервера, и описание меню из десяти
/// пунктов уже не помещается в двести пятьдесят шесть байт.
///
/// Отдельным пределом, а не поднятием общего: двести пятьдесят шесть байт — это
/// про имена, названия погоды и реплики в чате, и делать их вчетверо длиннее
/// только потому, что где-то понадобилась разметка, значило бы разрешить
/// никнейм в четыре килобайта.
inline constexpr std::size_t kMaxTextLength = 4096;

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

    /// Угол в градусах — двумя байтами вместо четырёх.
    ///
    /// Полный круг раскладывается на 65536 делений, то есть шаг чуть меньше
    /// шести тысячных градуса. Разглядеть такую разницу в повороте персонажа
    /// нельзя ни на каком расстоянии, а два байта на снимке — это два байта,
    /// умноженные на число игроков в квадрате.
    void writeAngle(float degrees);

    /// Скорость — шестью байтами вместо двенадцати.
    ///
    /// По две на ось, шагом в одну шестьдесят четвёртую метра в секунду и с
    /// пределом в пятьсот метров в секунду. Предел взят с запасом: быстрее в
    /// GTA не движется ничто, включая падающий самолёт.
    ///
    /// Скорость нужна получателю ровно для одного — достроить движение между
    /// снимками, — и сантиметр в секунду там ничего не решает.
    void writeVelocity(const Vec3& value);

    /// Пишет длину строки, а затем её байты. Строка длиннее kMaxStringLength
    /// обрезается: отправитель не должен иметь возможности собрать пакет,
    /// который получатель обязан отвергнуть.
    void writeString(std::string_view value);

    /// То же для полотна: длина пишется четырьмя байтами, предел — kMaxTextLength.
    ///
    /// Четырьмя, а не двумя, не ради нынешних четырёх килобайт: два байта
    /// вмещают шестьдесят пять тысяч, и однажды кто-нибудь пришлёт страницу
    /// длиннее. Молчаливая обрезка на границе типа — худший из способов это
    /// узнать.
    void writeText(std::string_view value);

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
    [[nodiscard]] float readAngle() noexcept;
    [[nodiscard]] Vec3 readVelocity() noexcept;
    [[nodiscard]] Vec3 readVec3() noexcept;
    [[nodiscard]] std::string readString();

    /// Читает полотно, записанное writeText.
    [[nodiscard]] std::string readText();

    /// Не было ли попыток прочитать больше, чем есть.
    [[nodiscard]] bool ok() const noexcept { return !failed_; }

    /// Объявляет разбор неудавшимся, не читая ничего.
    ///
    /// Нужно тому, кто разбирает поверх этого класса, а не им самим. Байты могут
    /// быть на месте и всё же не значить ничего: неизвестный номер типа в
    /// значении, список длиной в миллион, вложенность глубже дозволенной. Читатель
    /// про такое не знает — он знает только про границы данных, — а разбирающему
    /// нужно, чтобы одна неудача посреди дерева дошла до самого верха. Иначе
    /// проверять пришлось бы каждый возврат по пути, и однажды один из них
    /// забыли бы проверить.
    void fail() noexcept { failed_ = true; }

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
