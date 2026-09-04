#include "rpf7.hpp"

#include <miniz.h>

#include <cstring>
#include <format>

namespace oxymp::server {
namespace {

constexpr std::uint32_t kMagicRpf7 = 0x52504637;      // «RPF7» в порядке байт файла.
constexpr std::uint32_t kEncryptionOpen = 0x4E45504F; // «OPEN».
constexpr std::uint32_t kEncryptionCfxp = 0x50584643; // «CFXP» — своя альтернатива CitizenFX.

/// Значение поля смещения у записи-каталога.
constexpr std::uint32_t kDirectorySentinel = 0x7FFFFF;

/// Бит поля смещения, отделяющий блочный адрес от флага рода. Адрес — младшие
/// биты, помноженные на размер блока.
constexpr std::uint32_t kOffsetMask = 0x7FFFFF;
constexpr std::uint32_t kBlockSize = 512;

constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kEntrySize = 16;

/// Пределы от битого файла, а не от жадности: игра, получив мусорное число
/// записей, читает по нему за пределы и падает; нам падать незачем.
constexpr std::uint32_t kMaxEntries = 200'000;
constexpr int kMaxDepth = 64;
constexpr std::size_t kMaxUnpacked = std::size_t{512} * 1024 * 1024;

[[nodiscard]] std::uint32_t readU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v)); // архивы little-endian, как и целевая платформа
    return v;
}

[[nodiscard]] std::uint64_t readU64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

/// Запись оглавления в разобранном виде.
struct Entry {
    std::uint32_t nameOffset; ///< Смещение имени в таблице имён.
    std::uint32_t size;       ///< Сжатый размер; 0 — не сжат.
    std::uint32_t offset;     ///< Поле смещения; 0x7FFFFF — каталог.
    std::uint32_t virtFlags;  ///< Файл: распакованный размер. Каталог: первый ребёнок.
    std::uint32_t physFlags;  ///< Каталог: число детей.
};

} // namespace

std::vector<RpfEntry> unpackOpenRpf7(const std::uint8_t* bytes, std::size_t size,
                                     std::string& error) {
    error.clear();

    if (bytes == nullptr || size < kHeaderSize) {
        error = "the file is smaller than an RPF7 header";
        return {};
    }

    const std::uint32_t magic = readU32(bytes);
    const std::uint32_t entryCount = readU32(bytes + 4);
    const std::uint32_t nameLength = readU32(bytes + 8);
    const std::uint32_t encryption = readU32(bytes + 12);

    if (magic != kMagicRpf7) {
        error = "not an RPF7 archive";
        return {};
    }

    if (encryption != kEncryptionOpen && encryption != kEncryptionCfxp) {
        // Зашифрованное оглавление (NG/AES) нам не по зубам: ключ у игры, а она
        // чужой архив не расшифровывает. Такие собирают не для модов.
        error = std::format("the table of contents is encrypted (type {:#010x}); only OPEN "
                            "archives are supported",
                            encryption);
        return {};
    }

    if (entryCount == 0) {
        return {}; // пустой архив — не ошибка
    }

    if (entryCount > kMaxEntries) {
        error = std::format("the archive claims {} entries — refusing as corrupt", entryCount);
        return {};
    }

    const std::size_t entriesAt = kHeaderSize;
    const std::size_t entriesBytes = static_cast<std::size_t>(entryCount) * kEntrySize;
    const std::size_t namesAt = entriesAt + entriesBytes;

    if (namesAt < entriesAt || namesAt + nameLength < namesAt ||
        namesAt + nameLength > size) {
        error = "the entry table and name table run past the end of the file";
        return {};
    }

    std::vector<Entry> entries(entryCount);
    for (std::uint32_t i = 0; i < entryCount; ++i) {
        const std::uint8_t* const raw = bytes + entriesAt + static_cast<std::size_t>(i) * kEntrySize;
        const std::uint64_t packed = readU64(raw);

        // Битовое поле {nameOffset:16, size:24, offset:24}, младшими битами
        // вперёд — как у RagePackfile7::Entry в CitizenFX.
        entries[i].nameOffset = static_cast<std::uint32_t>(packed & 0xFFFFULL);
        entries[i].size = static_cast<std::uint32_t>((packed >> 16) & 0xFFFFFFULL);
        entries[i].offset = static_cast<std::uint32_t>((packed >> 40) & 0xFFFFFFULL);
        entries[i].virtFlags = readU32(raw + 8);
        entries[i].physFlags = readU32(raw + 12);
    }

    const char* const names = reinterpret_cast<const char*>(bytes + namesAt);

    const auto nameAt = [&](std::uint32_t at, std::string& out) -> bool {
        if (at >= nameLength) {
            return false;
        }
        const std::size_t maxLen = nameLength - at;
        const std::size_t len = ::strnlen(names + at, maxLen);
        if (len == maxLen) {
            return false; // имя не оканчивается нулём внутри таблицы
        }
        out.assign(names + at, len);
        return true;
    };

    std::vector<RpfEntry> files;
    std::string tooBig;
    std::size_t unpackedTotal = 0;

    // Обход дерева от корня (запись 0 — корневой каталог). Рекурсия по индексу с
    // ограничением глубины и проверкой, что дети лежат внутри таблицы. Циклы
    // ограничивает и глубина, и то, что дети каталога всегда идут после него.
    auto walk = [&](auto&& self, std::uint32_t dirIndex, const std::string& prefix,
                    int depth) -> bool {
        if (depth > kMaxDepth) {
            error = "the directory tree is deeper than makes sense — refusing as corrupt";
            return false;
        }

        const Entry& dir = entries[dirIndex];
        const std::uint64_t first = dir.virtFlags;
        const std::uint64_t count = dir.physFlags;

        if (first + count > entryCount || first + count < first) {
            error = "a directory points to entries past the table — refusing as corrupt";
            return false;
        }

        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint32_t childIndex = static_cast<std::uint32_t>(first + i);
            const Entry& child = entries[childIndex];

            std::string name;
            if (!nameAt(child.nameOffset, name) || name.empty()) {
                error = "an entry has no valid name — refusing as corrupt";
                return false;
            }

            if (child.offset == kDirectorySentinel) {
                if (childIndex <= dirIndex) {
                    error = "a directory points back at itself — refusing as corrupt";
                    return false;
                }
                if (!self(self, childIndex, prefix + name + "/", depth + 1)) {
                    return false;
                }
                continue;
            }

            // Файл. Блочный адрес — младшие биты смещения на размер блока.
            const std::size_t byteOffset =
                static_cast<std::size_t>(child.offset & kOffsetMask) * kBlockSize;
            const std::uint32_t unpackedSize = child.virtFlags;
            const bool compressed = child.size != 0;
            const std::size_t onDisk = compressed ? child.size : unpackedSize;

            if (byteOffset > size || onDisk > size - byteOffset) {
                error = std::format("file \"{}\" runs past the end of the archive", name);
                return false;
            }

            if (unpackedTotal + unpackedSize > kMaxUnpacked) {
                error = "the archive unpacks to more than half a gigabyte — refusing";
                return false;
            }

            RpfEntry out;
            out.path = prefix + name;
            out.data.resize(unpackedSize);

            if (unpackedSize != 0) {
                if (compressed) {
                    // Сырой deflate: без заголовка zlib и без сверки adler.
                    const std::size_t got = ::tinfl_decompress_mem_to_mem(
                        out.data.data(), unpackedSize, bytes + byteOffset, onDisk, 0);
                    if (got != unpackedSize) {
                        error = std::format("file \"{}\" did not inflate to its stated size", name);
                        return false;
                    }
                } else {
                    std::memcpy(out.data.data(), bytes + byteOffset, unpackedSize);
                }
            }

            unpackedTotal += unpackedSize;
            files.push_back(std::move(out));
        }

        return true;
    };

    if (!walk(walk, 0, std::string{}, 0)) {
        return {};
    }

    return files;
}

} // namespace oxymp::server
