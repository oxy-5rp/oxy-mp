#include "loaded_image.hpp"

#include <format>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Приводит виртуальный размер секции к чему-то осмысленному.
///
/// У некоторых секций виртуальный размер записан нулём — тогда полагаемся на
/// размер в файле, который загрузчик и отобразил.
std::uint32_t sizeOf(const IMAGE_SECTION_HEADER& header) noexcept {
    return header.Misc.VirtualSize != 0 ? header.Misc.VirtualSize : header.SizeOfRawData;
}

} // namespace

std::unique_ptr<LoadedImage> LoadedImage::open(std::string& error) {
    // nullptr означает «главный модуль процесса», то есть саму GTA5.exe.
    const auto* base = reinterpret_cast<const std::uint8_t*>(::GetModuleHandleW(nullptr));
    if (base == nullptr) {
        error = "не удалось получить базу главного модуля";
        return nullptr;
    }

    const auto& dos = *reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) {
        error = "главный модуль не похож на исполняемый файл Windows";
        return nullptr;
    }

    const auto& nt = *reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE) {
        error = "в главном модуле нет заголовка PE";
        return nullptr;
    }
    if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "главный модуль не 64-разрядный";
        return nullptr;
    }

    std::unique_ptr<LoadedImage> image{new LoadedImage};
    image->base_ = reinterpret_cast<std::uint64_t>(base);
    image->imageSize_ = nt.OptionalHeader.SizeOfImage;

    const auto* headers = IMAGE_FIRST_SECTION(&nt);
    image->sections_.reserve(nt.FileHeader.NumberOfSections);

    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& header = headers[i];

        gamesig::Section section;
        section.name.assign(reinterpret_cast<const char*>(header.Name),
                            ::strnlen(reinterpret_cast<const char*>(header.Name),
                                      IMAGE_SIZEOF_SHORT_NAME));
        section.rva = header.VirtualAddress;
        section.virtualSize = sizeOf(header);
        section.executable = (header.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;

        // Полей файлового размещения у отображённого модуля нет: секции лежат
        // по виртуальным адресам, а не по смещениям в файле.
        image->sections_.push_back(std::move(section));
    }

    image->origin_ = std::format("собственный процесс, модуль по адресу {:#x}", image->base_);

    return image;
}

memscan::ByteView LoadedImage::sectionData(const gamesig::Section& section) const noexcept {
    const std::uint64_t end = static_cast<std::uint64_t>(section.rva) + section.virtualSize;
    if (section.virtualSize == 0 || end > imageSize_) {
        return {};
    }

    return memscan::ByteView{reinterpret_cast<const std::uint8_t*>(base_ + section.rva),
                             section.virtualSize};
}

const std::uint8_t* LoadedImage::rvaToPointer(std::uint64_t rva,
                                              std::size_t needed) const noexcept {
    if (rva + needed > imageSize_) {
        return nullptr;
    }

    return reinterpret_cast<const std::uint8_t*>(base_ + rva);
}

} // namespace oxymp::client::game
