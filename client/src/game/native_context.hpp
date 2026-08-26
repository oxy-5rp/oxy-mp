#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace oxymp::client::game {

/// Контекст вызова натива.
///
/// Нативы принимают не аргументы в привычном смысле, а указатель на эту
/// структуру: в ней лежат буфер доводов, их число и место под результат.
/// Раскладку первых четырёх полей задаёт игра, менять их порядок нельзя.
///
/// Хвост — рабочая область, которую натив вправе занять под промежуточные
/// векторы. Она обязана существовать и быть обнулена: натив рассчитывает на
/// неё, не проверяя, и запись мимо буфера испортила бы наш стек.
///
/// Лежит отдельно от native_call.hpp затем, что здесь нет ни одного чужого
/// адреса: это чистая раскладка памяти, и её можно проверить набором без
/// запущенной игры. Ошибка в укладке доводов стоила драйверского места в машине
/// (см. push), и второй раз выяснять это живой игрой не стоит.
class alignas(16) NativeContext {
public:
    /// Сколько доводов помещается. Больше не требуется ни одному нативу,
    /// который нужен клиенту, а превышение поймается проверкой при добавлении.
    static constexpr std::size_t kMaxArguments = 32;

    /// Обнулять поля не требуется: у всех есть инициализаторы, а рабочая
    /// область и буферы объявлены пустыми списками. Остаётся связать указатели
    /// со своими буферами.
    NativeContext() noexcept : returnValue_(results_), arguments_(argumentValues_) {}

    /// Добавляет довод.
    ///
    /// Каждый занимает восемь байт независимо от собственного размера: игра
    /// читает их как ячейки одинаковой ширины.
    ///
    /// **Целые расширяются со знаком, а не копируются побайтно.** Это правка,
    /// оплаченная живой игрой. Прежде довод укладывался `memcpy` на свою ширину
    /// в обнулённую ячейку — и `std::int8_t{-1}` превращался в 255, потому что
    /// единственный записанный байт `0xFF` оставался в младшей части нуля.
    /// Место водителя в GTA — как раз `-1`, и `GET_PED_IN_VEHICLE_SEAT`
    /// спрашивался про место 255, которого нет ни у одной машины. Отвечал он
    /// всегда «никого», водитель не опознавался никогда, и наружу это выходило
    /// двумя бедами сразу: свой игрок уезжал на сервер пассажиром переднего
    /// места, а чужого пересаживали `SET_PED_INTO_VEHICLE` каждый кадр — по
    /// тридцать тысяч раз за сессию, судя по журналу.
    ///
    /// Беззнаковые расширяются нулями — то есть так же, как раньше. Дробные и
    /// указатели укладываются как есть: `float` игра читает четырьмя байтами из
    /// той же обнулённой ячейки, и превращать его во что-либо нельзя.
    template <typename T>
    void push(T value) noexcept {
        static_assert(sizeof(T) <= sizeof(std::uint64_t), "довод шире ячейки");
        static_assert(std::is_trivially_copyable_v<T>, "довод должен быть простым");

        if (argumentCount_ >= kMaxArguments) {
            return;
        }

        std::uint64_t& cell = argumentValues_[argumentCount_];
        ++argumentCount_;

        if constexpr (std::is_same_v<T, bool>) {
            cell = value ? 1U : 0U;
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            cell = static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
        } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
            cell = static_cast<std::uint64_t>(value);
        } else {
            cell = 0;
            std::memcpy(&cell, &value, sizeof(T));
        }
    }

    /// Результат вызова.
    ///
    /// Индекс нужен векторам: тройка координат возвращается тремя ячейками
    /// подряд, а не одной структурой.
    template <typename T>
    [[nodiscard]] T result(std::size_t index = 0) const noexcept {
        static_assert(sizeof(T) <= sizeof(std::uint64_t), "результат шире ячейки");
        static_assert(std::is_trivially_copyable_v<T>, "результат должен быть простым");

        T value{};
        if (index < kMaxArguments) {
            std::memcpy(&value, &results_[index], sizeof(T));
        }
        return value;
    }

    /// Сколько доводов уложено. Нужно только проверкам: игра читает это поле
    /// сама, из своей части раскладки.
    [[nodiscard]] std::uint32_t argumentCount() const noexcept { return argumentCount_; }

    /// Уложенный довод как его увидит игра. Тоже только для проверок.
    [[nodiscard]] std::uint64_t argument(std::size_t index) const noexcept {
        return index < kMaxArguments ? argumentValues_[index] : 0;
    }

    [[nodiscard]] void* address() noexcept { return this; }

private:
    // Первые четыре поля — раскладка игры. Порядок и размеры менять нельзя.
    void* returnValue_ = nullptr;      // +0
    std::uint32_t argumentCount_ = 0;  // +8
    void* arguments_ = nullptr;        // +16
    std::uint32_t dataCount_ = 0;      // +24

    /// Рабочая область игры под векторы плюс запас. Наши буферы идут после неё,
    /// чтобы запись игры в свою область не задела их.
    std::uint8_t scratch_[192] = {};

    std::uint64_t argumentValues_[kMaxArguments] = {};
    std::uint64_t results_[kMaxArguments] = {};
};

} // namespace oxymp::client::game
