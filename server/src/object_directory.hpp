#pragma once

#include <oxymp/script/dimension.hpp>
#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstddef>
#include <unordered_map>

namespace oxymp::server {

/// Предметы сессии: ящики, ограждения, рампы — всё, что ставят в мир руками.
///
/// Устроены заметно проще машин, и это не упрощение ради экономии, а следствие
/// того, чем они являются. У машины есть ведущий — клиент, считающий её физику,
/// — потому что машина едет, мнётся и переворачивается, и считать это может
/// только игра. Предмет стоит. Подвинуть его может лишь тот, кто поставил, и
/// подвинет он его у всех разом, через сервер.
///
/// Отсюда весь состав: ни ведущего, ни снимков на ходу, ни скорости. Всё, что о
/// предмете нужно знать, задаётся при появлении и больше не меняется.
///
/// Если однажды понадобятся предметы, которые можно толкать и бросать, эта
/// простота кончится: им придётся выдать ведущего и снимки, то есть повторить
/// устройство машин. Заводить это заранее, «на будущее», не стоит — лишний
/// ведущий у неподвижного ящика будет ошибкой, которую придётся объяснять.
class ObjectDirectory {
public:
    /// Предмет, каким его знает сервер.
    struct Object {
        std::uint32_t model = 0;
        shared::Vec3 position;
        shared::Vec3 rotation;

        /// В каком слое мира он стоит. См. script/dimension.hpp.
        std::int32_t dimension = script::kDefaultDimension;
    };

    /// Ставит предмет и выдаёт ему номер.
    ///
    /// limit — сколько предметов разрешено в сессии. kInvalidObjectId означает
    /// отказ: либо предел исчерпан, либо модель негодная.
    [[nodiscard]] shared::ObjectId add(std::uint32_t model, const shared::Vec3& position,
                                       const shared::Vec3& rotation, std::size_t limit);

    /// Переставляет предмет в другой слой мира. false — предмета уже нет.
    bool setDimension(shared::ObjectId id, std::int32_t dimension);

    /// Убирает предмет. false — предмета с таким номером не было.
    bool remove(shared::ObjectId id);

    [[nodiscard]] const Object* find(shared::ObjectId id) const;

    [[nodiscard]] const std::unordered_map<shared::ObjectId, Object>& all() const noexcept {
        return objects_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }

private:
    std::unordered_map<shared::ObjectId, Object> objects_;

    /// Счётчик выданных номеров. Растёт и не сбрасывается: номер убранного
    /// предмета не выдаётся повторно, иначе отставшее сообщение о нём попало бы
    /// в другой, успевший занять его номер.
    shared::ObjectId nextId_ = 0;
};

} // namespace oxymp::server
