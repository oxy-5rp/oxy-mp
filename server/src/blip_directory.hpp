#pragma once

#include <oxymp/script/dimension.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstddef>
#include <unordered_map>

namespace oxymp::server {

/// Метки на карте, поставленные сессией.
///
/// Устроены проще предметов, и это опять следствие того, чем они являются.
/// Предмет стоит в мире, и до него нужно дойти; метка нарисована на карте и
/// видна оттуда, откуда бы на карту ни смотрели. Поэтому расстоянием они не
/// отбираются вовсе — только измерением.
///
/// Меняться метка вправе когда угодно: ресурс красит её, переименовывает и
/// двигает по ходу дела. Поэтому состояние здесь хранится целиком, и уходит оно
/// к клиенту тоже целиком — одним сообщением на «заведи» и «поправь».
class BlipDirectory {
public:
    struct Blip {
        shared::BlipState state;

        /// В каком слое мира метка видна. См. script/dimension.hpp.
        std::int32_t dimension = script::kDefaultDimension;
    };

    /// Ставит метку и выдаёт ей номер.
    ///
    /// limit — сколько меток разрешено в сессии. kInvalidBlipId означает отказ:
    /// предел исчерпан.
    [[nodiscard]] shared::BlipId add(shared::BlipState state, std::size_t limit);

    /// Правит метку целиком. false — метки с таким номером не было.
    ///
    /// Номер в присланном состоянии не читается: его назначили при заведении, и
    /// менять его нельзя.
    bool update(shared::BlipId id, shared::BlipState state);

    /// Переставляет метку в другой слой мира. false — метки уже нет.
    bool setDimension(shared::BlipId id, std::int32_t dimension);

    /// Убирает метку. false — метки с таким номером не было.
    bool remove(shared::BlipId id);

    [[nodiscard]] const Blip* find(shared::BlipId id) const;

    [[nodiscard]] const std::unordered_map<shared::BlipId, Blip>& all() const noexcept {
        return blips_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return blips_.size(); }

private:
    std::unordered_map<shared::BlipId, Blip> blips_;

    /// Счётчик выданных номеров. Растёт и не сбрасывается: номер убранной метки
    /// не выдаётся повторно, иначе отставшее сообщение о ней попало бы в
    /// другую, успевшую занять её номер.
    shared::BlipId nextId_ = 0;
};

} // namespace oxymp::server
