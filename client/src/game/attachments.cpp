#include "attachments.hpp"

#include "native_call.hpp"
#include "native_hashes.hpp"

#include <spdlog/spdlog.h>

namespace oxymp::client::game {
namespace {

/// Довод p9 подписи ATTACH_ENTITY_TO_ENTITY: назначения у него нет и в открытой
/// базе. В скриптах самой игры на его месте ложь — ставим то же.
constexpr bool kUnusedAttachArgument = false;

/// Мягкое закрепление: с ним привязка рвётся сама, стоит двум телам с
/// включёнными столкновениями сойтись в клубок. Сервер сказал «привязано», и
/// рваться это не должно ни от чего — иначе привязанное разъедется у разных
/// игроков по-разному, а сервер об этом не узнает.
constexpr bool kSoftPinning = false;

/// Довод vertexIndex: «положение вершины». Что это значит, открытая база не
/// объясняет, а скрипты игры ставят здесь ноль.
constexpr int kVertexIndex = 0;

/// Кость, означающая «к самой сущности, а не к кости».
constexpr int kNoBone = -1;

} // namespace

Attachments::Attachments(const NativeTable& table) noexcept
    : attach_(table.handlerFor(natives::kAttachEntityToEntity)),
      detach_(table.handlerFor(natives::kDetachEntity)),
      boneByName_(table.handlerFor(natives::kGetEntityBoneIndexByName)) {}

bool Attachments::ready() const noexcept {
    return attach_ != nullptr && detach_ != nullptr;
}

std::uint64_t Attachments::keyOf(shared::EntityKind kind, std::uint32_t id) noexcept {
    return (static_cast<std::uint64_t>(kind) << 32U) | id;
}

void Attachments::apply(const shared::EntityAttachment& attachment) {
    if (attachment.kind == shared::EntityKind::None) {
        return;
    }

    const std::uint64_t key = keyOf(attachment.kind, attachment.id);
    const auto known = attachments_.find(key);

    if (known == attachments_.end()) {
        attachments_.emplace(key, Entry{.wanted = attachment});
        return;
    }

    // То же самое, что уже наложено, накладывать заново не нужно: привязка
    // приходит и при входе, и при всяком изменении, и вошедший получил бы рывок
    // предмета на ровном месте.
    if (known->second.wanted == attachment) {
        return;
    }

    known->second.wanted = attachment;
    known->second.attachedSelf = 0;
    known->second.attachedTarget = 0;
}

int Attachments::boneOf(const shared::EntityAttachment& attachment, int target) const {
    if (attachment.boneName.empty() || boneByName_ == nullptr) {
        return attachment.bone;
    }

    const int found = invokeNative<int>(boneByName_, target, attachment.boneName.c_str());

    // Минус единица от игры означает «такой кости у этой модели нет». Тогда
    // остаётся номер, названный сервером: он мог оказаться верным для этой
    // модели, даже когда имя не подошло.
    return found == kNoBone ? attachment.bone : found;
}

void Attachments::sync(const Resolve& resolve) {
    if (!ready()) {
        return;
    }

    std::erase_if(attachments_, [&](auto& pair) {
        Entry& entry = pair.second;
        const shared::EntityAttachment& wanted = entry.wanted;

        const int self = resolve(wanted.kind, wanted.id);

        // Отвязка. Тела уже может не быть — тогда отвязывать нечего, и запись
        // всё равно уходит: висеть ей больше не на чем.
        if (wanted.targetKind == shared::EntityKind::None) {
            if (self != 0 && entry.attachedSelf == self) {
                // Оба признака ложью: не ронять на землю физическим телом и не
                // включать столкновения тому, у кого их не было.
                invokeNative<void>(detach_, self, false, false);
                spdlog::debug("сущность {} отвязана", wanted.id);
            }

            return true;
        }

        const int target = resolve(wanted.targetKind, wanted.target);

        // Одного из двух тел ещё нет. Ждём: привязка не устаревает, а тело
        // появится, как только доедет машина или догрузится модель.
        if (self == 0 || target == 0) {
            entry.attachedSelf = 0;
            entry.attachedTarget = 0;
            return false;
        }

        if (entry.attachedSelf == self && entry.attachedTarget == target) {
            return false;
        }

        // isPed важен не для порядка: без него у привязанного не работает
        // наклон вперёд, а крен — только в отрицательную сторону. Так это
        // описано у самой игры.
        //
        // Родов персонажа два, а не один, и второй здесь долго не значился.
        // Игрок — это персонаж, но и кукла сервера (`alt.Ped`) тоже персонаж:
        // разница между ними в том, кто ими распоряжается, а не в том, из чего
        // они сделаны. Привязанная кукла из-за этого висела с перекошенным
        // поворотом, а объяснить это было нечем.
        const bool isPed = wanted.kind == shared::EntityKind::Player ||
                           wanted.kind == shared::EntityKind::Ped;
        const int bone = boneOf(wanted, target);

        invokeNative<void>(attach_, self, target, bone, wanted.position.x, wanted.position.y,
                           wanted.position.z, wanted.rotation.x, wanted.rotation.y,
                           wanted.rotation.z, kUnusedAttachArgument, kSoftPinning,
                           wanted.collision, isPed, kVertexIndex, wanted.fixedRotation);

        entry.attachedSelf = self;
        entry.attachedTarget = target;

        spdlog::debug("сущность {} привязана к {} (кость {})", wanted.id, wanted.target, bone);
        return false;
    });
}

void Attachments::clear() {
    attachments_.clear();
}

} // namespace oxymp::client::game
