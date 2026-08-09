#include "pe_headers.hpp"

#include <cstring>
#include <format>
#include <type_traits>

namespace oxymp::sigcheck::peheaders {
namespace {

// Разбор ведётся вручную по смещениям из спецификации PE, без windows.h:
// так код одинаково работает и с байтами из файла, и с байтами из чужого процесса.

constexpr std::uint16_t kDosSignature = 0x5A4D;     // "MZ"
constexpr std::uint32_t kPeSignature = 0x0000'4550; // "PE\0\0"
constexpr std::uint16_t kOptionalMagicPe32Plus = 0x20B;

constexpr std::size_t kDosLfanewOffset = 0x3C;
constexpr std::size_t kCoffHeaderSize = 20;
constexpr std::size_t kSectionHeaderSize = 40;
constexpr std::size_t kSectionNameLength = 8;

constexpr std::size_t kCoffNumberOfSectionsOffset = 2;
constexpr std::size_t kCoffSizeOfOptionalHeaderOffset = 16;
constexpr std::size_t kOptionalImageBaseOffset = 24;

constexpr std::uint32_t kSectionMemExecute = 0x2000'0000;

template<typename T>
std::optional<T> readAt(memscan::ByteView data, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);

    if (offset > data.size() || data.size() - offset < sizeof(T)) {
        return std::nullopt;
    }

    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

} // namespace

std::optional<Headers> parse(memscan::ByteView data, std::string& error) {
    if (readAt<std::uint16_t>(data, 0) != kDosSignature) {
        error = "нет сигнатуры MZ — это не исполняемый образ Windows";
        return std::nullopt;
    }

    const auto lfanew = readAt<std::uint32_t>(data, kDosLfanewOffset);
    if (!lfanew) {
        error = "образ обрывается на заголовке DOS";
        return std::nullopt;
    }

    const std::size_t peOffset = *lfanew;
    if (readAt<std::uint32_t>(data, peOffset) != kPeSignature) {
        error = "нет сигнатуры PE — образ повреждён";
        return std::nullopt;
    }

    const std::size_t coffOffset = peOffset + sizeof(std::uint32_t);
    const auto sectionCount = readAt<std::uint16_t>(data, coffOffset + kCoffNumberOfSectionsOffset);
    const auto optionalSize = readAt<std::uint16_t>(data, coffOffset + kCoffSizeOfOptionalHeaderOffset);
    if (!sectionCount || !optionalSize) {
        error = "образ обрывается на заголовке COFF";
        return std::nullopt;
    }

    const std::size_t optionalOffset = coffOffset + kCoffHeaderSize;
    if (readAt<std::uint16_t>(data, optionalOffset) != kOptionalMagicPe32Plus) {
        error = "ожидался 64-битный образ (PE32+), а этот другой";
        return std::nullopt;
    }

    const auto imageBase = readAt<std::uint64_t>(data, optionalOffset + kOptionalImageBaseOffset);
    if (!imageBase) {
        error = "образ обрывается на необязательном заголовке";
        return std::nullopt;
    }

    Headers headers;
    headers.imageBase = *imageBase;
    headers.sections.reserve(*sectionCount);

    const std::size_t sectionTableOffset = optionalOffset + *optionalSize;

    for (std::size_t i = 0; i < *sectionCount; ++i) {
        const std::size_t entry = sectionTableOffset + i * kSectionHeaderSize;

        const auto virtualSize = readAt<std::uint32_t>(data, entry + 8);
        const auto rva = readAt<std::uint32_t>(data, entry + 12);
        const auto rawSize = readAt<std::uint32_t>(data, entry + 16);
        const auto fileOffset = readAt<std::uint32_t>(data, entry + 20);
        const auto characteristics = readAt<std::uint32_t>(data, entry + 36);

        if (!virtualSize || !rva || !rawSize || !fileOffset || !characteristics) {
            error = std::format("образ обрывается на заголовке секции №{}", i);
            return std::nullopt;
        }

        const char* rawName = reinterpret_cast<const char*>(data.data() + entry);

        Section section;
        // Имя секции занимает до восьми байт, завершающий ноль не гарантирован.
        section.name.assign(rawName, ::strnlen(rawName, kSectionNameLength));
        section.virtualSize = *virtualSize;
        section.rva = *rva;
        section.rawSize = *rawSize;
        section.fileOffset = *fileOffset;
        section.executable = (*characteristics & kSectionMemExecute) != 0;

        headers.sections.push_back(std::move(section));
    }

    if (headers.sections.empty()) {
        error = "в образе нет ни одной секции";
        return std::nullopt;
    }

    return headers;
}

} // namespace oxymp::sigcheck::peheaders
