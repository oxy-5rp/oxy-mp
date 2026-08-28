#pragma once

#include <oxymp/script/dimension.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <utility>

namespace oxymp::server {

/// Всё, что сервер рисует игроку, но чего нет в мире как тела.
///
/// Таких родов три — метка на карте, маркер и контрольная точка, — и устроены
/// они одинаково до последнего поля: номер, состояние целиком и слой мира.
/// Одинаковы они не случайно, а потому, что все трое — картинки, а не
/// сущности: у них нет ни физики, ни ведущего, ни снимков на ходу. Заведи мы им
/// три отдельных реестра, они разъехались бы на первой же правке, и разъехались
/// бы молча.
///
/// Состояние хранится целиком и целиком же уходит к клиенту — одним сообщением
/// на «заведи» и «поправь». Раздельные сообщения означали бы, что получатель
/// обязан помнить, знает он уже эту картинку или нет; он и не помнит, и знать
/// ему это незачем.
///
/// Расстоянием ничто из этого не отбирается, в отличие от машин и предметов.
/// Метка видна на карте отовсюду; маркер и точку отбирает тот, кто их рисует, по
/// их собственному полю видимости. Отбирай их сервер — каждый шаг игрока через
/// границу стоил бы сообщения «заведи» или «убери».
///
/// Id — номер картинки, State — её описание из протокола. Ноль в номере
/// означает «такой картинки нет» у всех трёх родов, и счётчик поэтому
/// начинается с единицы.
/// Ничего сверх общего: у маркера и у контрольной точки своего нет.
struct NoExtras {};

template<typename IdType, typename StateType, typename Extras = NoExtras>
class DrawnDirectory {
public:
    /// Наружу — затем, что рассылает картинки один общий на все три рода код, и
    /// вывести из реестра, чем он распоряжается, ему больше неоткуда.
    using Id = IdType;
    using State = StateType;

    struct Entry : Extras {
        State state;

        /// В каком слое мира картинка видна. См. script/dimension.hpp.
        std::int32_t dimension = script::kDefaultDimension;
    };

    /// Заводит картинку и выдаёт ей номер.
    ///
    /// limit — сколько их разрешено в сессии. Ноль в ответе означает отказ:
    /// предел исчерпан.
    [[nodiscard]] Id add(State state, std::size_t limit) {
        if (entries_.size() >= limit) {
            return Id{0};
        }

        // Счётчик начинается с единицы: ноль означает «картинки нет», и
        // выданный им номер был бы неотличим от отсутствия.
        const Id id = ++nextId_;

        state.id = id;
        entries_.emplace(id, Entry{{}, std::move(state)});

        return id;
    }

    /// Правит картинку целиком. false — картинки с таким номером не было.
    ///
    /// Номер в присланном состоянии не читается: его назначили при заведении, и
    /// позволить правке его сменить значило бы разрешить картинке стать другой
    /// картинкой.
    bool update(Id id, State state) {
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            return false;
        }

        state.id = id;
        found->second.state = std::move(state);

        return true;
    }

    /// Переставляет картинку в другой слой мира. false — картинки уже нет.
    bool setDimension(Id id, std::int32_t dimension) {
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            return false;
        }

        found->second.dimension = dimension;
        return true;
    }

    /// Называет тех, кому картинка видна. false — картинки уже нет.
    ///
    /// Есть только там, где Extras это позволяет: у метки список есть, у
    /// маркера и точки его нет вовсе, и звать это для них не собирается.
    ///
    /// Признак общей идёт тем же вызовом, а не своим: порознь их не бывает —
    /// назвавший получателей всегда называет и то, общая ли метка, — и
    /// разделение дало бы промежуток, в котором метка уже не общая, а
    /// получателей у неё ещё нет.
    bool setTargets(Id id, bool global, std::vector<shared::PlayerId> targets)
        requires requires(Entry entry) { entry.targets; }
    {
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            return false;
        }

        found->second.global = global;
        found->second.targets = std::move(targets);
        return true;
    }

    /// Убирает картинку. false — картинки с таким номером не было.
    bool remove(Id id) { return entries_.erase(id) != 0; }

    [[nodiscard]] const Entry* find(Id id) const {
        const auto found = entries_.find(id);
        return found == entries_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] const std::unordered_map<Id, Entry>& all() const noexcept { return entries_; }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::unordered_map<Id, Entry> entries_;

    /// Счётчик выданных номеров. Растёт и не сбрасывается: номер убранной
    /// картинки не выдаётся повторно, иначе отставшее сообщение о ней попало бы
    /// в другую, успевшую занять её номер.
    Id nextId_ = 0;
};

/// Кому метка видна.
///
/// Своё у метки и больше ни у кого: маркер и точку отбирает у себя тот, кто их
/// рисует, а метка нарисована на карте и видна оттуда, откуда бы на карту ни
/// смотрели, — отобрать её может только отправитель.
///
/// По сети ни признак, ни список не едут: метка просто не уходит тем, кому она
/// не предназначена.
struct BlipTargets {
    /// Видна ли метка всем. Пока она общая, список не спрашивается — так же,
    /// как у alt:V, где `isGlobal` называется при заведении и потом не меняется.
    bool global = true;

    std::vector<shared::PlayerId> targets;
};

/// Метки на карте, поставленные сессией.
///
/// Устроены проще предметов, и это опять следствие того, чем они являются.
/// Предмет стоит в мире, и до него нужно дойти; метка нарисована на карте и
/// видна оттуда, откуда бы на карту ни смотрели.
using BlipDirectory = DrawnDirectory<shared::BlipId, shared::BlipState, BlipTargets>;

/// Маркеры: фигуры, нарисованные в мире.
using MarkerDirectory = DrawnDirectory<shared::MarkerId, shared::MarkerState>;

/// Контрольные точки.
///
/// Здесь только их вид. Вход и выход считаются отдельно и на сервере: у alt:V
/// контрольная точка — ещё и зона, а зоны в oxyMP работают целиком и об этом
/// реестре не знают.
using CheckpointDirectory = DrawnDirectory<shared::CheckpointId, shared::CheckpointState>;

} // namespace oxymp::server
