#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace oxymp::client::game {

/// Что к чему привязано в мире.
///
/// Привязка — единственное, что связывает две сущности сессии, и оттого
/// единственное, что нельзя наложить в тот миг, когда о нём сказали: тел может
/// не быть ни одного. Машина ещё не заведена, модель предмета грузится, кукла
/// чужого игрока не появилась — а сервер о привязке уже сказал, и сказал один
/// раз.
///
/// Поэтому здесь список желаемого и попытка раз в кадр. Пока обоих тел нет,
/// привязка ждёт; появились — накладывается; исчезло одно из двух — ждёт снова.
/// Ждать ей не надоедает: в отличие от движения, привязка не устаревает, и
/// шляпа, надетая час назад, обязана оказаться на голове у того, кто вошёл
/// сейчас.
///
/// Вызывать можно только изнутри скриптового тика.
class Attachments {
public:
    /// Как найти тело сущности в игре по её роду и номеру. Ноль — тела нет.
    ///
    /// Обратным вызовом, а не ссылками на списки машин, предметов и кукол.
    /// Причина не в отвлечённой чистоте: списки эти живут в трёх разных классах,
    /// и связав их здесь, мы получили бы четвёртый, знающий про все три, — то
    /// есть то самое место, куда со временем стекается всё.
    using Resolve = std::function<int(shared::EntityKind, std::uint32_t)>;

    explicit Attachments(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Запоминает привязку, присланную сервером. Род цели None — отвязать.
    void apply(const shared::EntityAttachment& attachment);

    /// Доводит до конца то, что не удалось раньше. Зовётся раз в кадр.
    void sync(const Resolve& resolve);

    /// Забывает все привязки, ничего не отвязывая.
    ///
    /// Отвязывать при разрыве нечего: тел, к которым они вели, к этому мгновению
    /// уже нет — сессия разобрана целиком.
    void clear();

    [[nodiscard]] std::size_t held() const noexcept { return attachments_.size(); }

private:
    /// Привязка и то, что мы о ней уже сделали.
    struct Entry {
        shared::EntityAttachment wanted;

        /// К какому телу привязано на самом деле. Ноль — ещё ни к какому.
        ///
        /// Помнится вместе с телом привязанного, и оба нужны: игра знает
        /// сущности по описателям, а описатель у той же машины после
        /// перезаведения будет другой. Не заметив смены, мы оставили бы предмет
        /// висеть на прежнем описателе — то есть в пустоте.
        int attachedSelf = 0;
        int attachedTarget = 0;
    };

    /// Род и номер одним числом: род в старшей половине.
    [[nodiscard]] static std::uint64_t keyOf(shared::EntityKind kind, std::uint32_t id) noexcept;

    /// Номер кости у цели. Имя старше номера: перевести его может только игра.
    [[nodiscard]] int boneOf(const shared::EntityAttachment& attachment, int target) const;

    NativeHandler attach_ = nullptr;
    NativeHandler detach_ = nullptr;
    NativeHandler boneByName_ = nullptr;

    std::unordered_map<std::uint64_t, Entry> attachments_;
};

} // namespace oxymp::client::game
