// Имена тестов латиницей: ctest передаёт их обратно в исполняемый файл как
// фильтр, и не-ASCII имена ломаются о кодировку консоли Windows.

#include "attachment_directory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace oxymp;
using namespace oxymp::server;

namespace {

AttachmentDirectory::Ref ref(shared::EntityKind kind, std::uint32_t id) {
    return AttachmentDirectory::Ref{.kind = kind, .id = id};
}

/// Привязка одного к другому, без смещений: проверяется здесь не геометрия.
shared::EntityAttachment link(AttachmentDirectory::Ref self, AttachmentDirectory::Ref target) {
    shared::EntityAttachment attachment;
    attachment.kind = self.kind;
    attachment.id = self.id;
    attachment.targetKind = target.kind;
    attachment.target = target.id;
    return attachment;
}

} // namespace

TEST_CASE("an entity remembers what it hangs on", "[attachments]") {
    AttachmentDirectory directory;

    const auto hat = ref(shared::EntityKind::Object, 1);
    const auto head = ref(shared::EntityKind::Player, 0);

    REQUIRE(directory.attach(link(hat, head)));

    const shared::EntityAttachment* const found = directory.find(hat);
    REQUIRE(found != nullptr);

    // Ноль здесь — законный номер первого вошедшего, а не «никого»: у игроков
    // пустой номер это единицы во всех разрядах.
    CHECK(found->targetKind == shared::EntityKind::Player);
    CHECK(found->target == 0);

    // Род различает то, чего не различает номер: первый предмет и первая машина
    // ходят под одним и тем же номером.
    CHECK(directory.find(ref(shared::EntityKind::Vehicle, 1)) == nullptr);
}

TEST_CASE("an entity cannot hang on itself", "[attachments]") {
    AttachmentDirectory directory;

    const auto box = ref(shared::EntityKind::Object, 3);

    CHECK_FALSE(directory.attach(link(box, box)));
    CHECK(directory.find(box) == nullptr);
}

// Игра ведёт привязанное, пересчитывая его положение от родителя. У двоих,
// привязанных друг к другу, родитель есть у каждого — и пересчёт не кончается
// никогда. Расплачивается за это игра игрока, а не сервер.
TEST_CASE("a ring of attachments is refused", "[attachments]") {
    AttachmentDirectory directory;

    const auto first = ref(shared::EntityKind::Object, 1);
    const auto second = ref(shared::EntityKind::Object, 2);
    const auto third = ref(shared::EntityKind::Object, 3);

    REQUIRE(directory.attach(link(first, second)));
    REQUIRE(directory.attach(link(second, third)));

    // Третий на первого замкнул бы кольцо: первый висит на втором, второй на
    // третьем.
    CHECK_FALSE(directory.attach(link(third, first)));
    CHECK(directory.find(third) == nullptr);

    // А на второго — не замкнул бы: второй на первом не висит.
    CHECK_FALSE(directory.attach(link(third, second)));
}

TEST_CASE("what hung on a vanished entity is loosened", "[attachments]") {
    AttachmentDirectory directory;

    const auto car = ref(shared::EntityKind::Vehicle, 5);
    const auto boxOne = ref(shared::EntityKind::Object, 1);
    const auto boxTwo = ref(shared::EntityKind::Object, 2);
    const auto elsewhere = ref(shared::EntityKind::Object, 3);
    const auto other = ref(shared::EntityKind::Vehicle, 6);

    REQUIRE(directory.attach(link(boxOne, car)));
    REQUIRE(directory.attach(link(boxTwo, car)));
    REQUIRE(directory.attach(link(elsewhere, other)));

    const std::vector<AttachmentDirectory::Ref> loosened = directory.forget(car);

    // Оба ящика отвязаны, и о каждом нужно сказать клиентам: иначе они так и
    // держали бы их привязанными к номеру, за которым больше ничего нет.
    CHECK(loosened.size() == 2);
    CHECK(std::ranges::find(loosened, boxOne) != loosened.end());
    CHECK(std::ranges::find(loosened, boxTwo) != loosened.end());

    CHECK(directory.find(boxOne) == nullptr);
    CHECK(directory.find(boxTwo) == nullptr);

    // Чужая привязка не тронута.
    CHECK(directory.find(elsewhere) != nullptr);
}

TEST_CASE("a vanished entity forgets what it hung on itself", "[attachments]") {
    AttachmentDirectory directory;

    const auto hat = ref(shared::EntityKind::Object, 1);
    const auto head = ref(shared::EntityKind::Player, 7);

    REQUIRE(directory.attach(link(hat, head)));

    // Уходит сама шляпа: сказать о ней клиентам нечего — им скажут, что предмета
    // больше нет, и этого довольно.
    CHECK(directory.forget(hat).empty());
    CHECK(directory.size() == 0);
}

TEST_CASE("detaching what was never attached changes nothing", "[attachments]") {
    AttachmentDirectory directory;

    CHECK_FALSE(directory.detach(ref(shared::EntityKind::Object, 1)));
    CHECK_FALSE(directory.attach(link(ref(shared::EntityKind::Object, 1),
                                      ref(shared::EntityKind::None, 0))));
}
