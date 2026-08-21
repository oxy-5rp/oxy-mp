#pragma once

#include <oxymp/shared/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace oxymp::server {

/// Кто к кому привязан.
///
/// Отдельным реестром, а не полем у игрока, машины и предмета, и это выбор, а не
/// лень. Привязка связывает две сущности разного рода, и жить она может только
/// там, где видны оба конца: положи мы её к привязанному, и вопрос «что висит на
/// этой машине» пришлось бы задавать трём реестрам сразу, а отвечать на него
/// нужно при всяком исчезновении.
///
/// Реестр знает только про номера и рода. Существуют ли названные сущности, он
/// не проверяет и проверить не может — списки не у него; это забота того, кто
/// его зовёт.
class AttachmentDirectory {
public:
    /// Сущность сессии: род и номер.
    ///
    /// Своей парой, а не двумя доводами, потому что порознь они бессмысленны:
    /// номер без рода не указывает ни на что — у машин и предметов нумерация
    /// своя, и третья машина с третьим предметом ничем не различаются.
    struct Ref {
        shared::EntityKind kind = shared::EntityKind::None;
        std::uint32_t id = 0;

        [[nodiscard]] bool operator==(const Ref& other) const noexcept = default;
    };

    /// Привязывает сущность к цели.
    ///
    /// Номер и род привязываемого берутся из описания. false — привязка не
    /// имеет смысла: сущность привязывают к самой себе либо цепь замкнулась бы в
    /// кольцо.
    ///
    /// Кольцо отвергается не из чистоплюйства. Игра ведёт привязанное, пересчитывая
    /// его положение от родителя; у двоих, привязанных друг к другу, родитель
    /// есть у каждого, и пересчёт этот не кончается никогда. Расплачивается за
    /// это не сервер, а игра игрока — зависанием.
    bool attach(const shared::EntityAttachment& attachment) {
        if (attachment.kind == shared::EntityKind::None ||
            attachment.targetKind == shared::EntityKind::None) {
            return false;
        }

        const Ref self{.kind = attachment.kind, .id = attachment.id};
        const Ref target{.kind = attachment.targetKind, .id = attachment.target};

        if (self == target || leadsTo(target, self)) {
            return false;
        }

        attachments_[keyOf(self)] = attachment;
        return true;
    }

    /// Отвязывает сущность. false — она и не была привязана.
    bool detach(Ref entity) { return attachments_.erase(keyOf(entity)) != 0; }

    /// К чему привязана сущность. Пусто — ни к чему.
    [[nodiscard]] const shared::EntityAttachment* find(Ref entity) const {
        const auto found = attachments_.find(keyOf(entity));
        return found == attachments_.end() ? nullptr : &found->second;
    }

    /// Забывает всё, что связано с исчезнувшей сущностью.
    ///
    /// И её собственную привязку, и привязки к ней: висящее на убранной машине
    /// осталось бы висеть на пустом месте, а получатель так и держал бы его
    /// привязанным к номеру, за которым больше ничего нет.
    ///
    /// Возвращает тех, кого пришлось отвязать, — о каждом нужно сказать
    /// клиентам. Сама исчезнувшая сущность в этот список не попадает: о её
    /// уходе им скажут отдельно, и отвязывать то, чего уже нет, незачем.
    [[nodiscard]] std::vector<Ref> forget(Ref entity) {
        attachments_.erase(keyOf(entity));

        std::vector<Ref> loosened;

        // Перебором, а не обратным указателем. Привязок в сессии единицы —
        // столько, сколько предметов у кого-то в руках, — и второй список ради
        // них стоил бы дороже перебора: его пришлось бы держать в согласии с
        // первым при всякой правке, а расходятся такие пары молча.
        std::erase_if(attachments_, [&](const auto& pair) {
            const shared::EntityAttachment& attachment = pair.second;

            if (attachment.targetKind != entity.kind || attachment.target != entity.id) {
                return false;
            }

            loosened.push_back(Ref{.kind = attachment.kind, .id = attachment.id});
            return true;
        });

        return loosened;
    }

    /// Все привязки — для того, кто только что вошёл.
    [[nodiscard]] const std::unordered_map<std::uint64_t, shared::EntityAttachment>& all()
        const noexcept {
        return attachments_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return attachments_.size(); }

private:
    /// Род и номер одним числом: род в старшей половине.
    ///
    /// Ключом составной пары был бы std::pair с собственным хешем, а его пришлось
    /// бы объявлять отдельной специализацией. Здесь же ключ — обычное целое, и
    /// пара из него собирается и разбирается на месте.
    [[nodiscard]] static std::uint64_t keyOf(Ref entity) noexcept {
        return (static_cast<std::uint64_t>(entity.kind) << 32U) | entity.id;
    }

    /// Ведёт ли цепь привязок от `from` к `to`.
    ///
    /// Обходом по родителям, а не поиском в глубину: родитель у сущности один, и
    /// цепь оттого не ветвится. Длина её ограничена числом привязок — кольца в
    /// готовом реестре быть не может, потому что каждое отвергается при
    /// заведении.
    [[nodiscard]] bool leadsTo(Ref from, Ref to) const {
        Ref step = from;

        for (std::size_t guard = 0; guard <= attachments_.size(); ++guard) {
            if (step == to) {
                return true;
            }

            const shared::EntityAttachment* const parent = find(step);
            if (parent == nullptr) {
                return false;
            }

            step = Ref{.kind = parent->targetKind, .id = parent->target};
        }

        return false;
    }

    std::unordered_map<std::uint64_t, shared::EntityAttachment> attachments_;
};

} // namespace oxymp::server
