#include "state_bundler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace oxymp;
using namespace oxymp::server;

namespace {

/// Снимок заданной длины, заполненный узнаваемым байтом.
///
/// Содержимое здесь ничего не значит: связка складывает готовые байты и в них
/// не заглядывает. Значение байта нужно ровно для одного — убедиться, что
/// снимок уехал целиком и не перепутался с соседним.
std::vector<std::uint8_t> snapshot(std::size_t length, std::uint8_t mark) {
    return std::vector<std::uint8_t>(length, mark);
}

/// Собранные связки, как их увидел бы транспорт.
struct Collector {
    std::vector<std::vector<std::uint8_t>> bundles;

    void operator()(shared::ByteView bundle) { bundles.emplace_back(bundle.begin(), bundle.end()); }
};

/// Сколько снимков объявлено в связке. Второй байт, сразу за номером сообщения.
std::uint8_t declared(const std::vector<std::uint8_t>& bundle) {
    return bundle.at(1);
}

} // namespace

TEST_CASE("an empty bundle is not sent at all") {
    StateBundler bundler;
    Collector collector;

    bundler.reset();
    bundler.finish(collector);

    CHECK(collector.bundles.empty());
}

TEST_CASE("a bundle names how many snapshots it carries") {
    StateBundler bundler;
    Collector collector;

    bundler.reset();

    for (std::uint8_t mark = 0; mark < 3; ++mark) {
        const auto bytes = snapshot(40, mark);
        bundler.add(shared::ByteView{bytes}, collector);
    }

    bundler.finish(collector);

    REQUIRE(collector.bundles.size() == 1);
    CHECK(declared(collector.bundles.front()) == 3);

    // Номер сообщения, байт длины и три снимка по сорок байт.
    CHECK(collector.bundles.front().size() == 2 + 3 * 40);
}

TEST_CASE("a bundle never outgrows what the transport carries in one packet") {
    StateBundler bundler;
    Collector collector;

    bundler.reset();

    // Сотня снимков по семьдесят байт — семь килобайт, то есть заведомо больше
    // одной посылки. Прежде это уезжало одной связкой, и транспорт резал её на
    // куски надёжными: снимкам такое противопоказано.
    for (std::size_t i = 0; i < 100; ++i) {
        const auto bytes = snapshot(70, static_cast<std::uint8_t>(i));
        bundler.add(shared::ByteView{bytes}, collector);
    }

    bundler.finish(collector);

    REQUIRE(collector.bundles.size() > 1);

    for (const std::vector<std::uint8_t>& bundle : collector.bundles) {
        CHECK(bundle.size() <= kMaxBundleBytes);
    }
}

TEST_CASE("an overgrown bundle loses no snapshot") {
    StateBundler bundler;
    Collector collector;

    bundler.reset();

    constexpr std::size_t kSnapshots = 100;

    for (std::size_t i = 0; i < kSnapshots; ++i) {
        const auto bytes = snapshot(70, static_cast<std::uint8_t>(i));
        bundler.add(shared::ByteView{bytes}, collector);
    }

    bundler.finish(collector);

    std::size_t carried = 0;

    for (const std::vector<std::uint8_t>& bundle : collector.bundles) {
        carried += declared(bundle);

        // Объявленное число обязано сойтись с тем, что в связке лежит: получатель
        // читает ровно столько снимков, сколько ему назвали.
        CHECK(bundle.size() == 2 + declared(bundle) * 70);
    }

    CHECK(carried == kSnapshots);
}

TEST_CASE("a bundle carries no more snapshots than its length byte can name") {
    StateBundler bundler;
    Collector collector;

    bundler.reset();

    // Снимки по два байта: в посылку их влезло бы шестьсот, а объявить можно
    // только двести пятьдесят пять. Предел здесь не от длины, а от того, что
    // число снимков пишется одним байтом.
    for (std::size_t i = 0; i < 300; ++i) {
        const auto bytes = snapshot(2, static_cast<std::uint8_t>(i));
        bundler.add(shared::ByteView{bytes}, collector);
    }

    bundler.finish(collector);

    REQUIRE(collector.bundles.size() == 2);
    CHECK(declared(collector.bundles.front()) == shared::kMaxStatesInBundle);
    CHECK(declared(collector.bundles.back()) == 300 - shared::kMaxStatesInBundle);
}

TEST_CASE("a bundle reset after a full one starts empty") {
    StateBundler bundler;
    Collector collector;

    bundler.reset();

    const auto bytes = snapshot(40, 1);
    bundler.add(shared::ByteView{bytes}, collector);
    bundler.finish(collector);

    // Следующий получатель начинает с чистого листа: связка предыдущего не
    // должна достаться ему в наследство.
    bundler.reset();
    bundler.finish(collector);

    REQUIRE(collector.bundles.size() == 1);
    CHECK(declared(collector.bundles.front()) == 1);
}
