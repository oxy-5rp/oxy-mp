#include <oxymp/shared/resource/vault.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <format>

namespace oxymp::shared {
namespace {

// --- SHA-256, RFC 6234 --------------------------------------------------------
//
// Своя реализация, а не чужая библиотека, и причина та же, что у шифра: ради
// одной функции тянуть зависимость, которую потом собирать на каждой машине,
// дороже, чем сто строк, проверяемых по контрольным примерам стандарта.

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) {
    std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // Дополнение: единичный бит, нули и длина в битах восемью байтами.
    std::vector<std::uint8_t> padded(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;

    padded.push_back(0x80);
    while (padded.size() % 64 != 56) {
        padded.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(bits >> shift));
    }

    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 64> schedule{};

        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t at = offset + i * 4;
            schedule[i] = (static_cast<std::uint32_t>(padded[at]) << 24) |
                          (static_cast<std::uint32_t>(padded[at + 1]) << 16) |
                          (static_cast<std::uint32_t>(padded[at + 2]) << 8) |
                          static_cast<std::uint32_t>(padded[at + 3]);
        }

        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t a = std::rotr(schedule[i - 15], 7) ^
                                    std::rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
            const std::uint32_t b = std::rotr(schedule[i - 2], 17) ^
                                    std::rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + a + schedule[i - 7] + b;
        }

        std::array<std::uint32_t, 8> working = state;

        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = std::rotr(working[4], 6) ^ std::rotr(working[4], 11) ^
                                     std::rotr(working[4], 25);
            const std::uint32_t choice =
                (working[4] & working[5]) ^ (~working[4] & working[6]);
            const std::uint32_t temp1 =
                working[7] + s1 + choice + kRoundConstants[i] + schedule[i];
            const std::uint32_t s0 = std::rotr(working[0], 2) ^ std::rotr(working[0], 13) ^
                                     std::rotr(working[0], 22);
            const std::uint32_t majority =
                (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const std::uint32_t temp2 = s0 + majority;

            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temp1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temp1 + temp2;
        }

        for (std::size_t i = 0; i < 8; ++i) {
            state[i] += working[i];
        }
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }

    return digest;
}

// --- ChaCha20, RFC 8439 -------------------------------------------------------

void quarterRound(std::array<std::uint32_t, 16>& block, int a, int b, int c, int d) {
    block[a] += block[b];
    block[d] = std::rotl(block[d] ^ block[a], 16);
    block[c] += block[d];
    block[b] = std::rotl(block[b] ^ block[c], 12);
    block[a] += block[b];
    block[d] = std::rotl(block[d] ^ block[a], 8);
    block[c] += block[d];
    block[b] = std::rotl(block[b] ^ block[c], 7);
}

std::uint32_t littleEndian(std::span<const std::uint8_t> from, std::size_t at) {
    return static_cast<std::uint32_t>(from[at]) |
           (static_cast<std::uint32_t>(from[at + 1]) << 8) |
           (static_cast<std::uint32_t>(from[at + 2]) << 16) |
           (static_cast<std::uint32_t>(from[at + 3]) << 24);
}

/// Накладывает поток шифра на данные. Шифрование и расшифровка — одно действие.
void applyStream(std::span<std::uint8_t> data, std::span<const std::uint8_t> key,
                 std::span<const std::uint8_t> nonce) {
    std::array<std::uint32_t, 16> start{};

    // "expand 32-byte k" — постоянная из стандарта.
    start[0] = 0x61707865;
    start[1] = 0x3320646e;
    start[2] = 0x79622d32;
    start[3] = 0x6b206574;

    for (std::size_t i = 0; i < 8; ++i) {
        start[4 + i] = littleEndian(key, i * 4);
    }

    start[12] = 0;

    for (std::size_t i = 0; i < 3; ++i) {
        start[13 + i] = littleEndian(nonce, i * 4);
    }

    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<std::uint32_t, 16> block = start;

        for (int round = 0; round < 10; ++round) {
            quarterRound(block, 0, 4, 8, 12);
            quarterRound(block, 1, 5, 9, 13);
            quarterRound(block, 2, 6, 10, 14);
            quarterRound(block, 3, 7, 11, 15);
            quarterRound(block, 0, 5, 10, 15);
            quarterRound(block, 1, 6, 11, 12);
            quarterRound(block, 2, 7, 8, 13);
            quarterRound(block, 3, 4, 9, 14);
        }

        for (std::size_t i = 0; i < 16; ++i) {
            block[i] += start[i];
        }

        const std::size_t remaining = std::min<std::size_t>(64, data.size() - offset);

        for (std::size_t i = 0; i < remaining; ++i) {
            const std::uint32_t word = block[i / 4];
            const auto keyByte = static_cast<std::uint8_t>(word >> ((i % 4) * 8));

            data[offset + i] ^= keyByte;
        }

        ++start[12];
    }
}

/// Кладёт четырёхбайтное число в конец, младшим байтом вперёд.
void appendLittleEndian(std::vector<std::uint8_t>& to, std::uint32_t value) {
    to.push_back(static_cast<std::uint8_t>(value));
    to.push_back(static_cast<std::uint8_t>(value >> 8));
    to.push_back(static_cast<std::uint8_t>(value >> 16));
    to.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::uint32_t readLittleEndian(std::span<const std::uint8_t> from, std::size_t at) {
    return littleEndian(from, at);
}

/// Длина заголовка: метка, версия, длина содержимого и разовая добавка.
constexpr std::size_t kHeaderLength = 4 + 4 + 4 + Vault::kNonceLength;

} // namespace

std::vector<std::uint8_t> Vault::builtInKey() {
    // Ключ собирается из четырёх частей на месте. Целиком он в файле не лежит, и
    // поиском по строкам его не найти — а это отсекает большинство любопытных.
    constexpr std::array<std::uint8_t, 8> first = {0x7A, 0x1C, 0xE4, 0x93,
                                                   0x2B, 0xD5, 0x60, 0xAF};
    constexpr std::array<std::uint8_t, 8> second = {0x11, 0x8E, 0x47, 0xC2,
                                                    0x39, 0xF0, 0x6D, 0x5B};
    constexpr std::array<std::uint8_t, 8> third = {0xA4, 0x02, 0xBB, 0x7E,
                                                   0xD8, 0x51, 0x9C, 0x36};
    constexpr std::array<std::uint8_t, 8> fourth = {0x63, 0xFA, 0x28, 0x85,
                                                    0x4D, 0x17, 0xE9, 0xC0};

    std::vector<std::uint8_t> key;
    key.reserve(kKeyLength);

    key.insert(key.end(), first.begin(), first.end());
    key.insert(key.end(), second.begin(), second.end());
    key.insert(key.end(), third.begin(), third.end());
    key.insert(key.end(), fourth.begin(), fourth.end());

    // Части перемешиваются между собой, чтобы ключ не совпадал ни с одной из
    // них по отдельности.
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(key[i] ^ (0x5A + i * 3));
    }

    return key;
}

std::vector<std::uint8_t> Vault::pack(std::span<const std::uint8_t> plain,
                                      std::span<const std::uint8_t> key) {
    const std::array<std::uint8_t, 32> digest = sha256(plain);

    // Разовая добавка — первые байты отпечатка содержимого. Одинаковый вход даёт
    // одинаковый выход, и клиент не качает заново то, что не менялось.
    std::array<std::uint8_t, kNonceLength> nonce{};
    std::copy_n(digest.begin(), kNonceLength, nonce.begin());

    std::vector<std::uint8_t> packed;
    packed.reserve(kHeaderLength + plain.size());

    appendLittleEndian(packed, kMagic);
    appendLittleEndian(packed, kVersion);
    appendLittleEndian(packed, static_cast<std::uint32_t>(plain.size()));
    packed.insert(packed.end(), nonce.begin(), nonce.end());

    packed.insert(packed.end(), plain.begin(), plain.end());

    applyStream(std::span{packed}.subspan(kHeaderLength), key, nonce);

    return packed;
}

std::vector<std::uint8_t> Vault::unpack(std::span<const std::uint8_t> packed,
                                        std::span<const std::uint8_t> key, std::string& error) {
    if (packed.size() < kHeaderLength) {
        error = "файл ресурса короче собственного заголовка";
        return {};
    }

    if (readLittleEndian(packed, 0) != kMagic) {
        error = "это не файл ресурса oxyMP";
        return {};
    }

    if (const std::uint32_t version = readLittleEndian(packed, 4); version != kVersion) {
        error = std::format("файл ресурса версии {}, а понимаем {}", version, kVersion);
        return {};
    }

    const std::uint32_t length = readLittleEndian(packed, 8);

    if (packed.size() - kHeaderLength != length) {
        error = "длина ресурса не сходится с заголовком — файл дошёл не целиком";
        return {};
    }

    std::array<std::uint8_t, kNonceLength> nonce{};
    std::copy_n(packed.begin() + 12, kNonceLength, nonce.begin());

    std::vector<std::uint8_t> plain(packed.begin() + kHeaderLength, packed.end());
    applyStream(plain, key, nonce);

    // Проверка отпечатком: расшифровка чужим ключом не отказывает, а молча даёт
    // мусор. Без этой проверки мусор уехал бы в игру.
    const std::array<std::uint8_t, 32> digest = sha256(plain);

    if (!std::equal(nonce.begin(), nonce.end(), digest.begin())) {
        error = "содержимое не сошлось с отпечатком — ключ не тот либо файл испорчен";
        return {};
    }

    return plain;
}

std::string fingerprint(std::span<const std::uint8_t> data) {
    const std::array<std::uint8_t, 32> digest = sha256(data);

    std::string text;
    text.reserve(digest.size() * 2);

    for (const std::uint8_t byte : digest) {
        text += std::format("{:02x}", byte);
    }

    return text;
}

} // namespace oxymp::shared
