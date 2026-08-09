#include "pe_image.hpp"

#include <cstring>
#include <format>
#include <fstream>
#include <type_traits>

namespace oxymp::sigcheck {
namespace {

// Разбор PE ведётся вручную по смещениям из спецификации, без windows.h:
// так утилита остаётся обычным консольным кодом, который легко читать и проверять.

constexpr std::uint16_t kDosSignature = 0x5A4D;      // "MZ"
constexpr std::uint32_t kPeSignature = 0x0000'4550;  // "PE\0\0"
constexpr std::uint16_t kOptionalMagicPe32Plus = 0x20B;

constexpr std::size_t kDosLfanewOffset = 0x3C;
constexpr std::size_t kCoffHeaderSize = 20;
constexpr std::size_t kSectionHeaderSize = 40;

constexpr std::size_t kCoffNumberOfSectionsOffset = 2;
constexpr std::size_t kCoffSizeOfOptionalHeaderOffset = 16;
constexpr std::size_t kOptionalImageBaseOffset = 24;

constexpr std::uint32_t kSectionMemExecute = 0x2000'0000;

template<typename T>
std::optional<T> readAt(const std::vector<std::uint8_t>& data, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);

    if (offset > data.size() || data.size() - offset < sizeof(T)) {
        return std::nullopt;
    }

    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

std::optional<std::vector<std::uint8_t>> readWholeFile(const std::filesystem::path& path,
                                                       std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = std::format("не удалось получить размер файла: {}", ec.message());
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "не удалось открыть файл на чтение";
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        error = "файл прочитан не полностью";
        return std::nullopt;
    }

    return bytes;
}

} // namespace

std::optional<PeImage> PeImage::load(const std::filesystem::path& path, std::string& error) {
    auto bytes = readWholeFile(path, error);
    if (!bytes) {
        return std::nullopt;
    }

    PeImage image;
    image.bytes_ = std::move(*bytes);

    const auto dosSignature = readAt<std::uint16_t>(image.bytes_, 0);
    if (dosSignature != kDosSignature) {
        error = "это не исполняемый файл Windows: нет сигнатуры MZ";
        return std::nullopt;
    }

    const auto lfanew = readAt<std::uint32_t>(image.bytes_, kDosLfanewOffset);
    if (!lfanew) {
        error = "файл обрывается на заголовке DOS";
        return std::nullopt;
    }

    const std::size_t peOffset = *lfanew;
    if (readAt<std::uint32_t>(image.bytes_, peOffset) != kPeSignature) {
        error = "нет сигнатуры PE — файл повреждён или это не PE-образ";
        return std::nullopt;
    }

    const std::size_t coffOffset = peOffset + sizeof(std::uint32_t);
    const auto numberOfSections =
        readAt<std::uint16_t>(image.bytes_, coffOffset + kCoffNumberOfSectionsOffset);
    const auto sizeOfOptionalHeader =
        readAt<std::uint16_t>(image.bytes_, coffOffset + kCoffSizeOfOptionalHeaderOffset);
    if (!numberOfSections || !sizeOfOptionalHeader) {
        error = "файл обрывается на заголовке COFF";
        return std::nullopt;
    }

    const std::size_t optionalOffset = coffOffset + kCoffHeaderSize;
    const auto optionalMagic = readAt<std::uint16_t>(image.bytes_, optionalOffset);
    if (optionalMagic != kOptionalMagicPe32Plus) {
        error = "ожидался 64-битный образ (PE32+), а этот другой";
        return std::nullopt;
    }

    const auto imageBase = readAt<std::uint64_t>(image.bytes_, optionalOffset + kOptionalImageBaseOffset);
    if (!imageBase) {
        error = "файл обрывается на необязательном заголовке";
        return std::nullopt;
    }
    image.imageBase_ = *imageBase;

    const std::size_t sectionTableOffset = optionalOffset + *sizeOfOptionalHeader;
    image.sections_.reserve(*numberOfSections);

    for (std::size_t i = 0; i < *numberOfSections; ++i) {
        const std::size_t entry = sectionTableOffset + i * kSectionHeaderSize;

        if (entry > image.bytes_.size() || image.bytes_.size() - entry < kSectionHeaderSize) {
            error = std::format("файл обрывается на заголовке секции №{}", i);
            return std::nullopt;
        }

        const char* rawName = reinterpret_cast<const char*>(image.bytes_.data() + entry);

        Section section;
        // Имя секции — до 8 байт, завершающий ноль не гарантирован.
        section.name.assign(rawName, ::strnlen(rawName, 8));
        section.virtualSize = *readAt<std::uint32_t>(image.bytes_, entry + 8);
        section.rva = *readAt<std::uint32_t>(image.bytes_, entry + 12);
        section.rawSize = *readAt<std::uint32_t>(image.bytes_, entry + 16);
        section.fileOffset = *readAt<std::uint32_t>(image.bytes_, entry + 20);
        section.executable =
            (*readAt<std::uint32_t>(image.bytes_, entry + 36) & kSectionMemExecute) != 0;

        image.sections_.push_back(std::move(section));
    }

    if (image.sections_.empty()) {
        error = "в образе нет ни одной секции";
        return std::nullopt;
    }

    return image;
}

memscan::ByteView PeImage::sectionData(const Section& section) const noexcept {
    const std::uint32_t size = section.scannableSize();

    if (section.fileOffset > bytes_.size() || bytes_.size() - section.fileOffset < size) {
        return {};
    }

    return memscan::ByteView{bytes_.data() + section.fileOffset, size};
}

const std::uint8_t* PeImage::rvaToPointer(std::uint64_t rva, std::size_t needed) const noexcept {
    for (const Section& section : sections_) {
        if (rva < section.rva) {
            continue;
        }

        const std::uint64_t offsetInSection = rva - section.rva;
        const std::uint32_t available = section.scannableSize();

        if (offsetInSection >= available || available - offsetInSection < needed) {
            continue;
        }

        const std::uint64_t fileOffset = section.fileOffset + offsetInSection;
        if (fileOffset + needed > bytes_.size()) {
            continue;
        }

        return bytes_.data() + fileOffset;
    }

    return nullptr;
}

} // namespace oxymp::sigcheck
