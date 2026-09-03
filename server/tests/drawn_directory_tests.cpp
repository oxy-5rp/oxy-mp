#include "drawn_directory.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace oxymp;
using namespace oxymp::server;

namespace {

constexpr std::size_t kPlenty = 64;

} // namespace

TEST_CASE("a leaving player's number does not stay a blip target", "[server][blips]") {
    // Номер игрока — наименьшее свободное место (PlayerRegistry::freeId), и
    // ушедший освобождает его следующему вошедшему. Список получателей метки
    // обязан забыть его сам: иначе вошедший под тем же номером увидел бы
    // метку, назначенную не ему.
    BlipDirectory blips;

    const shared::BlipId id = blips.add(shared::BlipState{}, kPlenty);
    REQUIRE(id != shared::kInvalidBlipId);

    REQUIRE(blips.setTargets(id, false, {3, 7, 9}));

    blips.forgetPlayer(7);

    const BlipDirectory::Entry* const found = blips.find(id);
    REQUIRE(found != nullptr);

    CHECK(found->targets == std::vector<shared::PlayerId>{3, 9});
}

TEST_CASE("forgetting a player not on the list changes nothing", "[server][blips]") {
    BlipDirectory blips;

    const shared::BlipId id = blips.add(shared::BlipState{}, kPlenty);
    REQUIRE(id != shared::kInvalidBlipId);

    REQUIRE(blips.setTargets(id, false, {3, 9}));

    blips.forgetPlayer(404);

    const BlipDirectory::Entry* const found = blips.find(id);
    REQUIRE(found != nullptr);

    CHECK(found->targets == std::vector<shared::PlayerId>{3, 9});
}
