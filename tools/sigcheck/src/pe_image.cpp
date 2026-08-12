#include "pe_image.hpp"

#include "pe_headers.hpp"

#include <format>
#include <fstream>

namespace oxymp::sigcheck {
namespace {

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

std::unique_ptr<PeImage> PeImage::load(const std::filesystem::path& path, std::string& error) {
    auto bytes = readWholeFile(path, error);
    if (!bytes) {
        return nullptr;
    }

    auto headers = peheaders::parse(memscan::ByteView{bytes->data(), bytes->size()}, error);
    if (!headers) {
        return nullptr;
    }

    std::unique_ptr<PeImage> image{new PeImage};
    image->bytes_ = std::move(*bytes);
    image->imageBase_ = headers->imageBase;
    image->sections_ = std::move(headers->sections);
    image->origin_ = std::format("файл {}", path.string());

    return image;
}

std::uint32_t PeImage::availableSize(const gamesig::Section& section) noexcept {
    return section.virtualSize < section.rawSize ? section.virtualSize : section.rawSize;
}

memscan::ByteView PeImage::sectionData(const gamesig::Section& section) const noexcept {
    const std::uint32_t size = availableSize(section);

    if (section.fileOffset > bytes_.size() || bytes_.size() - section.fileOffset < size) {
        return {};
    }

    return memscan::ByteView{bytes_.data() + section.fileOffset, size};
}

const std::uint8_t* PeImage::rvaToPointer(std::uint64_t rva, std::size_t needed) const noexcept {
    for (const gamesig::Section& section : sections_) {
        if (rva < section.rva) {
            continue;
        }

        const std::uint64_t offsetInSection = rva - section.rva;
        const std::uint32_t available = availableSize(section);

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
