#pragma once

#include "native_table.hpp"

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
    /// читает их как ячейки одинаковой ширины. Меньшие значения кладутся в
    /// младшую часть уже обнулённой ячейки.
    template <typename T>
    void push(T value) noexcept {
        static_assert(sizeof(T) <= sizeof(std::uint64_t), "довод шире ячейки");
        static_assert(std::is_trivially_copyable_v<T>, "довод должен быть простым");

        if (argumentCount_ >= kMaxArguments) {
            return;
        }

        std::memcpy(&argumentValues_[argumentCount_], &value, sizeof(T));
        ++argumentCount_;
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

/// Вызывает натив по готовому обработчику.
///
/// Обработчик берётся у NativeTable и проверяется на пустоту вызывающим:
/// отсутствующий натив — это ошибка в хешах, и молча её проглатывать нельзя.
template <typename Result = void, typename... Args>
Result invokeNative(NativeHandler handler, Args... arguments) {
    NativeContext context;
    (context.push(arguments), ...);

    handler(context.address());

    if constexpr (!std::is_void_v<Result>) {
        return context.result<Result>();
    }
}

} // namespace oxymp::client::game
