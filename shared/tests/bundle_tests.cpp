#include <oxymp/shared/resource/bundle.hpp>
#include <oxymp/shared/resource/vault.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using oxymp::shared::Bundle;

namespace {

[[nodiscard]] std::vector<std::uint8_t> bytesOf(std::string_view text) {
    return std::vector<std::uint8_t>{text.begin(), text.end()};
}

[[nodiscard]] std::string textOf(std::span<const std::uint8_t> bytes) {
    return std::string{bytes.begin(), bytes.end()};
}

[[nodiscard]] std::vector<Bundle::File> sample() {
    return {
        Bundle::File{.path = "server/index.js", .contents = bytesOf("alt.log('привет')")},
        Bundle::File{.path = "client/index.js", .contents = bytesOf("alt.on('connect', ok)")},
        Bundle::File{.path = "stream/car.yft", .contents = bytesOf(std::string(5000, 'M'))},
    };
}

/// Достаёт один файл из свёртка целиком — как это делал бы клиент.
[[nodiscard]] std::vector<std::uint8_t> take(std::span<const std::uint8_t> packed,
                                             const Bundle::Header& header,
                                             const Bundle::Entry& entry,
                                             std::span<const std::uint8_t> key) {
    const auto at = static_cast<std::size_t>(header.bodyStart() + entry.offset);

    std::vector<std::uint8_t> part{packed.begin() + at,
                                   packed.begin() + at + static_cast<std::size_t>(entry.size)};

    Bundle::openBody(part, entry.offset, header, key);

    return part;
}

} // namespace

TEST_CASE("a bundle gives back every file that went into it") {
    const auto key = oxymp::shared::Vault::builtInKey();
    const std::vector<Bundle::File> files = sample();

    const std::vector<std::uint8_t> packed = Bundle::pack(files, key);

    const auto header = Bundle::readHeader(packed);
    REQUIRE(header.has_value());
    CHECK(header->entryCount == files.size());

    const std::vector<Bundle::Entry> index = Bundle::readIndex(
        *header,
        std::span{packed}.subspan(Bundle::kHeaderLength, header->indexLength),
        key);

    REQUIRE(index.size() == files.size());

    for (const Bundle::File& wanted : files) {
        const auto found = std::ranges::find(index, wanted.path, &Bundle::Entry::path);
        REQUIRE(found != index.end());

        CHECK(take(packed, *header, *found, key) == wanted.contents);
    }
}

TEST_CASE("a bundle keeps no plain text of what it holds") {
    const auto key = oxymp::shared::Vault::builtInKey();
    const std::vector<Bundle::File> files = sample();

    const std::vector<std::uint8_t> packed = Bundle::pack(files, key);
    const std::string asText = textOf(packed);

    // Ни имени файла, ни его содержимого не должно быть видно простым поиском:
    // ради этого свёрток и заведён.
    CHECK(asText.find("server/index.js") == std::string::npos);
    CHECK(asText.find("alt.log") == std::string::npos);
    CHECK(asText.find("alt.on") == std::string::npos);
}

TEST_CASE("a piece of a bundle opens without the rest of it") {
    const auto key = oxymp::shared::Vault::builtInKey();
    const std::vector<Bundle::File> files = sample();

    const std::vector<std::uint8_t> packed = Bundle::pack(files, key);

    const auto header = Bundle::readHeader(packed);
    REQUIRE(header.has_value());

    const std::vector<Bundle::Entry> index = Bundle::readIndex(
        *header, std::span{packed}.subspan(Bundle::kHeaderLength, header->indexLength), key);

    const auto found = std::ranges::find(index, "stream/car.yft", &Bundle::Entry::path);
    REQUIRE(found != index.end());

    // Ради этого свойства формат и такой: игра читает модель кусками по
    // смещению, и кусок обязан расшифровываться сам по себе.
    constexpr std::uint64_t kFrom = 1234;
    constexpr std::size_t kHowMuch = 100;

    const auto at = static_cast<std::size_t>(header->bodyStart() + found->offset + kFrom);

    std::vector<std::uint8_t> middle{packed.begin() + at, packed.begin() + at + kHowMuch};
    Bundle::openBody(middle, found->offset + kFrom, *header, key);

    CHECK(textOf(middle) == std::string(kHowMuch, 'M'));
}

TEST_CASE("the same files pack into the very same bundle") {
    const auto key = oxymp::shared::Vault::builtInKey();

    // Порядок подачи другой, содержимое то же: свёрток обязан выйти побайтно
    // одинаковым — иначе игрок качал бы заново то, что у него уже есть.
    std::vector<Bundle::File> shuffled = sample();
    std::ranges::reverse(shuffled);

    CHECK(Bundle::pack(sample(), key) == Bundle::pack(shuffled, key));
}

TEST_CASE("a bundle that is not ours is refused") {
    const std::vector<std::uint8_t> nonsense(Bundle::kHeaderLength, 0x42);

    CHECK_FALSE(Bundle::readHeader(nonsense).has_value());

    // Обрезанный тоже: заголовок целиком не прочёлся, и верить нечему.
    CHECK_FALSE(Bundle::readHeader(std::span{nonsense}.subspan(0, 8)).has_value());
}

TEST_CASE("a lying entry count does not make readIndex reserve the world") {
    // entryCount приезжает из чужого файла и ничем не проверен: сервер с
    // крохотным index может назвать его 0xFFFFFFFF. До правки это шло прямо в
    // reserve() и просило сотни гигабайт под записи, которых в файле нет и
    // быть не может, — а падает такой reserve() необработанным исключением, то
    // есть эта проверка сама по себе и есть регресс: не упади процесс, тест
    // просто пройдёт и покажет, что записи прочлись как обычно.
    const auto key = oxymp::shared::Vault::builtInKey();
    const std::vector<Bundle::File> files = sample();

    const std::vector<std::uint8_t> packed = Bundle::pack(files, key);

    auto header = Bundle::readHeader(packed);
    REQUIRE(header.has_value());

    header->entryCount = 0xFFFFFFFFU;

    const std::vector<Bundle::Entry> index = Bundle::readIndex(
        *header, std::span{packed}.subspan(Bundle::kHeaderLength, header->indexLength), key);

    // Разбор не смотрит на entryCount вовсе — он читает буфер, пока в нём
    // хватает места на запись, — и лживое число не должно менять результат.
    CHECK(index.size() == files.size());
}

TEST_CASE("a bundle with a broken index yields nothing rather than nonsense") {
    const auto key = oxymp::shared::Vault::builtInKey();

    std::vector<std::uint8_t> packed = Bundle::pack(sample(), key);

    const auto header = Bundle::readHeader(packed);
    REQUIRE(header.has_value());

    // Портим оглавление: разбор обязан отдать пустоту, а не записи с длинами из
    // мусора — по таким мы прочли бы чужую память.
    std::vector<std::uint8_t> index{packed.begin() + Bundle::kHeaderLength,
                                    packed.begin() + Bundle::kHeaderLength + header->indexLength};

    for (std::uint8_t& byte : index) {
        byte = static_cast<std::uint8_t>(byte ^ 0xFFU);
    }

    CHECK(Bundle::readIndex(*header, index, key).empty());
}
