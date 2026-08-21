#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <unordered_map>

namespace oxymp::client::game {

/// Контрольные точки: столбы света с иконкой, поставленные сервером.
///
/// Устроены как метки на карте, а не как маркеры: игра заводит точку у себя,
/// выдаёт описатель и дальше ведёт её сама — рисовать её каждый кадр не нужно и
/// нельзя. Отсюда словарь «номер сессии → описатель игры».
///
/// Место, вид и радиус игра принимает один раз, при заведении, и поменять их у
/// заведённой точки нечем. Поэтому здесь помнится и присланное состояние: по
/// нему видно, довольно ли перекрасить точку или её придётся завести заново.
///
/// Вызывать можно только изнутри скриптового тика.
class Checkpoints {
public:
    explicit Checkpoints(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Заводит точку или поправляет уже заведённую.
    void apply(const shared::CheckpointState& state);

    void remove(shared::CheckpointId id);

    /// Убирает все разом. Нужно при разрыве.
    void clear();

private:
    /// Точка, какой её знает клиент.
    struct Shown {
        /// Что о ней сказал сервер в последний раз.
        shared::CheckpointState state;

        /// Описатель игры. Ноль — точка есть в сессии, но не заведена здесь:
        /// её погасили признаком visible.
        int handle = 0;
    };

    /// Заводит точку в игре и красит её. Ноль — игра отказала.
    [[nodiscard]] int create(const shared::CheckpointState& state) const;

    /// Снимает точку в игре. Описатель обнуляется вызывающим.
    void destroy(int handle) const;

    /// Совпадает ли у двух состояний всё, что игра принимает только при
    /// заведении.
    [[nodiscard]] static bool sameShape(const shared::CheckpointState& left,
                                        const shared::CheckpointState& right) noexcept;

    NativeHandler create_ = nullptr;
    NativeHandler delete_ = nullptr;
    NativeHandler setHeight_ = nullptr;
    NativeHandler setColour_ = nullptr;
    NativeHandler setIconColour_ = nullptr;

    std::unordered_map<shared::CheckpointId, Shown> shown_;
};

} // namespace oxymp::client::game
