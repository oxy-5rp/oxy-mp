#include "rpf7.hpp"

#include <miniz.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace oxymp::server;

namespace {

void putU32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    std::memcpy(out.data() + at, &v, sizeof(v));
}

void putU64(std::vector<std::uint8_t>& out, std::size_t at, std::uint64_t v) {
    std::memcpy(out.data() + at, &v, sizeof(v));
}

/// Пакует запись оглавления в 16 байт по разметке RagePackfile7.
void putEntry(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t nameOffset,
              std::uint32_t size, std::uint32_t offset, std::uint32_t virtFlags,
              std::uint32_t physFlags) {
    const std::uint64_t packed = static_cast<std::uint64_t>(nameOffset) |
                                 (static_cast<std::uint64_t>(size) << 16) |
                                 (static_cast<std::uint64_t>(offset) << 40);
    putU64(out, at, packed);
    putU32(out, at + 8, virtFlags);
    putU32(out, at + 12, physFlags);
}

} // namespace

TEST_CASE("an OPEN RPF7 archive unpacks its files", "[rpf7]") {
    const std::string rawText = "loose model bytes";

    // Сжимаемое содержимое — с повтором, чтобы сырой deflate его действительно
    // ужал, а не сохранил как есть.
    std::string packedText;
    for (int i = 0; i < 64; ++i) {
        packedText += "MODELDATA-";
    }

    // Сырой deflate (без заголовка zlib) — ровно то, что кладёт в записи RPF7.
    std::size_t compressedLen = 0;
    void* const compressed = ::tdefl_compress_mem_to_heap(
        packedText.data(), packedText.size(), &compressedLen, TDEFL_DEFAULT_MAX_PROBES);
    REQUIRE(compressed != nullptr);
    REQUIRE(compressedLen > 0);
    REQUIRE(compressedLen < packedText.size()); // действительно сжалось

    // Таблица имён: root, raw.txt, packed.bin — каждое оканчивается нулём.
    const std::string names = std::string("root") + '\0' + "raw.txt" + '\0' + "packed.bin" + '\0';
    const std::uint32_t nameRoot = 0;
    const std::uint32_t nameRaw = 5;
    const std::uint32_t namePacked = 13;

    const std::uint32_t entryCount = 3;
    const std::size_t entriesAt = 16;
    const std::size_t namesAt = entriesAt + entryCount * 16;

    // Данные файлов кладём по границам блоков 512: raw в блок 1, packed в блок 2.
    const std::size_t rawAt = 512;
    const std::size_t packedAt = 1024;
    const std::size_t total = packedAt + compressedLen;

    std::vector<std::uint8_t> archive(total, 0);

    // Заголовок.
    putU32(archive, 0, 0x52504637);            // «RPF7»
    putU32(archive, 4, entryCount);
    putU32(archive, 8, static_cast<std::uint32_t>(names.size()));
    putU32(archive, 12, 0x4E45504F);           // «OPEN»

    // Записи: корень (каталог, 2 ребёнка с индекса 1) и два файла.
    putEntry(archive, entriesAt + 0 * 16, nameRoot, 0, 0x7FFFFF, /*firstChild*/ 1, /*count*/ 2);
    putEntry(archive, entriesAt + 1 * 16, nameRaw, /*size*/ 0, /*offset blk*/ 1,
             static_cast<std::uint32_t>(rawText.size()), 0);
    putEntry(archive, entriesAt + 2 * 16, namePacked, static_cast<std::uint32_t>(compressedLen),
             /*offset blk*/ 2, static_cast<std::uint32_t>(packedText.size()), 0);

    // Таблица имён и данные файлов.
    std::memcpy(archive.data() + namesAt, names.data(), names.size());
    std::memcpy(archive.data() + rawAt, rawText.data(), rawText.size());
    std::memcpy(archive.data() + packedAt, compressed, compressedLen);
    ::mz_free(compressed);

    std::string error;
    const std::vector<RpfEntry> files = unpackOpenRpf7(archive.data(), archive.size(), error);

    REQUIRE(error.empty());
    REQUIRE(files.size() == 2);

    const RpfEntry* raw = nullptr;
    const RpfEntry* packed = nullptr;
    for (const RpfEntry& f : files) {
        if (f.path == "raw.txt") {
            raw = &f;
        } else if (f.path == "packed.bin") {
            packed = &f;
        }
    }

    REQUIRE(raw != nullptr);
    REQUIRE(packed != nullptr);

    CHECK(std::string(raw->data.begin(), raw->data.end()) == rawText);
    CHECK(std::string(packed->data.begin(), packed->data.end()) == packedText);
}

TEST_CASE("a non-OPEN RPF7 archive refuses out loud", "[rpf7]") {
    std::vector<std::uint8_t> archive(16, 0);
    putU32(archive, 0, 0x52504637); // RPF7
    putU32(archive, 4, 1);
    putU32(archive, 8, 0);
    putU32(archive, 12, 0x0FEFFFFF); // NG — зашифрованное оглавление

    std::string error;
    const std::vector<RpfEntry> files = unpackOpenRpf7(archive.data(), archive.size(), error);

    CHECK(files.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("a file that is not an RPF7 refuses out loud", "[rpf7]") {
    const std::string junk = "this is not an archive at all, not even close";

    std::string error;
    const std::vector<RpfEntry> files = unpackOpenRpf7(
        reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size(), error);

    CHECK(files.empty());
    CHECK_FALSE(error.empty());
}
