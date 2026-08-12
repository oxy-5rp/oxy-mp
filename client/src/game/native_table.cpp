#include "native_table.hpp"

#include <algorithm>
#include <cstring>

namespace oxymp::client::game {
namespace {

constexpr std::string_view kTableId = "native_registration_table";
constexpr std::string_view kLookupId = "native_handler_lookup";

/// Сколько корзин в таблице: индексом служит младший байт хеша.
constexpr std::size_t kBucketCount = 256;

// Раскладка записи регистрации. Снята с дизассемблера функции поиска: ничего
// из этого в игре не документировано, и проверяется оно только тем, что обход
// даёт осмысленные хеши.
//
// Смысл обфускации в том, что ключ шифрования — сам адрес записи. Поэтому
// запись, скопированная в другое место, становится нечитаемой, и просто снять
// дамп памяти недостаточно.
constexpr std::size_t kNextKeyOffset = 0x08;
constexpr std::size_t kCountOffset = 0x48;
constexpr std::size_t kEntriesOffset = 0x54;
constexpr std::size_t kEntrySize = 0x10;
constexpr std::size_t kEntryKeyOffset = 0x08;

/// Предел разумного числа записей в одном узле.
///
/// Настоящее значение — единицы. Заметно большее означает, что снятие
/// обфускации даёт мусор, и дальше идти нельзя: следующий шаг уведёт чтение
/// в произвольную память.
constexpr std::uint32_t kSaneEntryLimit = 64;

/// Сколько нативов ожидается в таблице. Служит только оценкой для выделения
/// памяти под список: точное число известно лишь после обхода.
constexpr std::size_t kExpectedNativeCount = 8192;

std::uint32_t readDword(const std::uint8_t* at) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

/// Склеивает два зашифрованных слова в число, снимая XOR ключом.
std::uint64_t decodeQword(const std::uint8_t* at, std::uint32_t key) noexcept {
    const std::uint64_t low = readDword(at) ^ key;
    const std::uint64_t high = readDword(at + sizeof(std::uint32_t)) ^ key;

    return low | (high << 32);
}

/// Ключ шифрования записи: слово по фиксированному смещению, смешанное с
/// младшей половиной собственного адреса.
std::uint32_t keyOf(const std::uint8_t* at, std::size_t keyOffset) noexcept {
    return readDword(at + keyOffset) ^ static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(at));
}

std::uint32_t entryCount(const std::uint8_t* registration) noexcept {
    const std::uint8_t* counter = registration + kCountOffset;
    const auto address = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(counter));

    return readDword(counter) ^ readDword(counter + sizeof(std::uint32_t)) ^ address;
}

const std::uint8_t* nextRegistration(const std::uint8_t* registration) noexcept {
    const std::uint64_t next = decodeQword(registration, keyOf(registration, kNextKeyOffset));
    return reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(next));
}

std::uint64_t hashAt(const std::uint8_t* registration, std::uint32_t index) noexcept {
    const std::uint8_t* entry = registration + kEntriesOffset + index * kEntrySize;
    return decodeQword(entry, keyOf(entry, kEntryKeyOffset));
}

/// Обходит таблицу, передавая каждый найденный хеш обработчику.
///
/// Обход прекращается, как только обработчик вернёт false.
template <typename Visitor>
void walk(void* table, Visitor&& visit) {
    if (table == nullptr) {
        return;
    }

    const auto* buckets = static_cast<const std::uint8_t* const*>(table);

    for (std::size_t bucket = 0; bucket < kBucketCount; ++bucket) {
        for (const std::uint8_t* registration = buckets[bucket]; registration != nullptr;
             registration = nextRegistration(registration)) {
            const std::uint32_t count = entryCount(registration);
            if (count > kSaneEntryLimit) {
                // Снятие обфускации дало бессмыслицу — дальше по цепочке идти
                // опаснее, чем остановиться.
                return;
            }

            for (std::uint32_t index = 0; index < count; ++index) {
                if (!visit(hashAt(registration, index))) {
                    return;
                }
            }
        }
    }
}

} // namespace

NativeTable::NativeTable(const EngineAddresses& addresses) noexcept
    : table_(addresses.pointerTo<void*>(kTableId)),
      lookup_(addresses.pointerTo<LookupFunction>(kLookupId)) {}

NativeHandler NativeTable::handlerFor(std::uint64_t hash) const noexcept {
    if (!valid()) {
        return nullptr;
    }

    return lookup_(table_, hash);
}

std::vector<std::uint64_t> NativeTable::registeredHashes(std::size_t limit) const {
    std::vector<std::uint64_t> hashes;

    // Предел приходит и запредельным — так просят «все». Резервировать по нему
    // нельзя: вектор столько не выделит и бросит исключение ещё до обхода.
    hashes.reserve(std::min<std::size_t>(limit, kExpectedNativeCount));

    walk(table_, [&](std::uint64_t hash) {
        hashes.push_back(hash);
        return hashes.size() < limit;
    });

    return hashes;
}

std::size_t NativeTable::registeredCount() const {
    std::size_t count = 0;

    walk(table_, [&](std::uint64_t) {
        ++count;
        return true;
    });

    return count;
}

} // namespace oxymp::client::game
