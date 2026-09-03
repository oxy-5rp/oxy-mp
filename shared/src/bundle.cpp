#include <oxymp/shared/resource/bundle.hpp>

#include <oxymp/shared/resource/vault.hpp>

#include <algorithm>
#include <cstring>

namespace oxymp::shared {
namespace {

void writeLittle16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void writeLittle32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void writeLittle64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

[[nodiscard]] std::uint32_t readLittle32(std::span<const std::uint8_t> from, std::size_t at) {
    return static_cast<std::uint32_t>(from[at]) | (static_cast<std::uint32_t>(from[at + 1]) << 8) |
           (static_cast<std::uint32_t>(from[at + 2]) << 16) |
           (static_cast<std::uint32_t>(from[at + 3]) << 24);
}

[[nodiscard]] std::uint64_t readLittle64(std::span<const std::uint8_t> from, std::size_t at) {
    std::uint64_t value = 0;

    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | from[at + static_cast<std::size_t>(i)];
    }

    return value;
}

/// Длина записи оглавления без самого пути.
constexpr std::size_t kEntryFixed = 2 + 8 + 8;

/// Предел на длину пути внутри свёртка.
///
/// Не от жадности, а от испорченного файла: длина пути читается из свёртка, и
/// без предела разбор попытался бы отвести под неё сколько скажут.
constexpr std::size_t kMaxPathLength = 1024;

} // namespace

std::vector<std::uint8_t> Bundle::pack(std::span<const File> files,
                                       std::span<const std::uint8_t> key) {
    // Порядок записей — по пути, а не тот, в каком их подали. Так свёрток
    // одинакового содержимого выходит побайтно одинаковым независимо от того,
    // в каком порядке файловая система отдала каталог, — а на этом держится
    // неизменность отпечатка и весь кеш у клиента.
    std::vector<const File*> ordered;
    ordered.reserve(files.size());

    for (const File& file : files) {
        ordered.push_back(&file);
    }

    std::ranges::sort(ordered, {}, [](const File* file) -> const std::string& { return file->path; });

    std::vector<std::uint8_t> index;
    std::vector<std::uint8_t> body;

    for (const File* file : ordered) {
        if (file->path.size() > kMaxPathLength) {
            continue;
        }

        writeLittle16(index, static_cast<std::uint16_t>(file->path.size()));
        writeLittle64(index, body.size());
        writeLittle64(index, file->contents.size());

        index.insert(index.end(), file->path.begin(), file->path.end());
        body.insert(body.end(), file->contents.begin(), file->contents.end());
    }

    // Добавка — из отпечатка того, что кладём: одинаковый вход даёт одинаковый
    // выход, и пересборка ресурса без правок не заставляет игрока качать заново.
    std::vector<std::uint8_t> together;
    together.reserve(index.size() + body.size());
    together.insert(together.end(), index.begin(), index.end());
    together.insert(together.end(), body.begin(), body.end());

    const std::string mark = fingerprint(together);

    std::uint8_t nonce[kNonceLength] = {};
    for (std::size_t i = 0; i < kNonceLength && (i * 2 + 1) < mark.size(); ++i) {
        const auto digit = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') {
                return static_cast<std::uint8_t>(c - '0');
            }
            return static_cast<std::uint8_t>(c - 'a' + 10);
        };

        nonce[i] = static_cast<std::uint8_t>((digit(mark[i * 2]) << 4) | digit(mark[i * 2 + 1]));
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderLength + index.size() + body.size());

    writeLittle32(out, kMagic);
    writeLittle32(out, kVersion);
    writeLittle32(out, static_cast<std::uint32_t>(index.size()));
    writeLittle32(out, static_cast<std::uint32_t>(ordered.size()));
    writeLittle64(out, body.size());
    out.insert(out.end(), std::begin(nonce), std::end(nonce));

    // Поток один на весь файл: оглавление занимает его начало, тело — всё, что
    // за ним. Начни тело поток заново, оба куска были бы зашифрованы одним и тем
    // же его началом и выдавали бы друг друга.
    applyKeystreamAt(index, key, nonce, 0);
    applyKeystreamAt(body, key, nonce, index.size());

    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), body.begin(), body.end());

    return out;
}

std::optional<Bundle::Header> Bundle::readHeader(std::span<const std::uint8_t> head) {
    if (head.size() < kHeaderLength) {
        return std::nullopt;
    }

    if (readLittle32(head, 0) != kMagic || readLittle32(head, 4) != kVersion) {
        return std::nullopt;
    }

    Header header;
    header.indexLength = readLittle32(head, 8);
    header.entryCount = readLittle32(head, 12);
    header.bodyLength = readLittle64(head, 16);

    std::memcpy(header.nonce, head.data() + 24, kNonceLength);

    return header;
}

std::vector<Bundle::Entry> Bundle::readIndex(const Header& header,
                                             std::span<const std::uint8_t> index,
                                             std::span<const std::uint8_t> key) {
    std::vector<Entry> entries;

    if (index.size() != header.indexLength) {
        return entries;
    }

    std::vector<std::uint8_t> plain(index.begin(), index.end());
    applyKeystreamAt(plain, key, header.nonce, 0);

    std::size_t at = 0;

    // entryCount приезжает из чужого файла и ничем не проверен: резерв по нему
    // впрямую — это неограниченное выделение по одному лживому числу в
    // заголовке. Сервер с крохотным index может назвать entryCount
    // 0xFFFFFFFF, и reserve() попросит несколько сотен гигабайт под записи,
    // которых в файле нет и быть не может, — клиент падает необработанным
    // bad_alloc, даже не дойдя до цикла ниже, который прочёл бы файл честно и
    // остановился бы сам. Больше записей, чем влезает по kEntryFixed байт
    // каждая в сам буфер, всё равно не бывает — тем же пределом и режем.
    entries.reserve(std::min<std::size_t>(header.entryCount, plain.size() / kEntryFixed));

    while (at + kEntryFixed <= plain.size()) {
        const std::size_t pathLength =
            static_cast<std::size_t>(plain[at]) | (static_cast<std::size_t>(plain[at + 1]) << 8);

        const std::uint64_t offset = readLittle64(plain, at + 2);
        const std::uint64_t size = readLittle64(plain, at + 10);

        at += kEntryFixed;

        if (pathLength > kMaxPathLength || at + pathLength > plain.size()) {
            // Испорченное оглавление разбирать дальше нельзя: длины в нём уже
            // ничего не значат, и следующая запись прочлась бы из середины пути.
            return {};
        }

        // Кусок обязан лежать внутри тела: свёрток мог дойти обрезанным, а по
        // такой записи мы прочли бы чужую память.
        if (offset > header.bodyLength || size > header.bodyLength - offset) {
            return {};
        }

        Entry entry;
        entry.path.assign(reinterpret_cast<const char*>(plain.data() + at), pathLength);
        entry.offset = offset;
        entry.size = size;

        at += pathLength;

        entries.push_back(std::move(entry));
    }

    return entries;
}

void Bundle::openBody(std::span<std::uint8_t> part, std::uint64_t offsetInBody,
                      const Header& header, std::span<const std::uint8_t> key) {
    applyKeystreamAt(part, key, header.nonce, header.indexLength + offsetInBody);
}

} // namespace oxymp::shared
