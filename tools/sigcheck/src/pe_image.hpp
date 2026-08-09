#pragma once

#include <oxymp/memscan/scanner.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace oxymp::sigcheck {

/// Секция PE-образа.
struct Section {
    std::string name;
    std::uint32_t rva = 0;
    std::uint32_t virtualSize = 0;
    std::uint32_t fileOffset = 0;
    std::uint32_t rawSize = 0;
    bool executable = false;

    /// Сколько байт секции реально присутствует в файле.
    ///
    /// Виртуальный размер бывает больше файлового (хвост, который загрузчик обнулит),
    /// а файловый — больше виртуального (выравнивание). Сканировать имеет смысл
    /// только пересечение.
    [[nodiscard]] std::uint32_t scannableSize() const noexcept {
        return virtualSize < rawSize ? virtualSize : rawSize;
    }
};

/// Исполняемый файл, прочитанный с диска.
///
/// Игра при этом не запускается и не изменяется — это статический разбор файла.
/// Адреса считаются от предпочтительной базы загрузки; у живого процесса база
/// может отличаться из-за ASLR, поэтому сравнивать имеет смысл RVA, а не VA.
class PeImage {
public:
    [[nodiscard]] static std::optional<PeImage> load(const std::filesystem::path& path,
                                                     std::string& error);

    [[nodiscard]] std::uint64_t imageBase() const noexcept { return imageBase_; }

    [[nodiscard]] const std::vector<Section>& sections() const noexcept { return sections_; }

    /// Байты секции так, как они лежат в файле.
    [[nodiscard]] memscan::ByteView sectionData(const Section& section) const noexcept;

    /// RVA в указатель на буфер файла.
    ///
    /// Возвращает nullptr, если по этому RVA нет данных или их меньше, чем needed:
    /// такое бывает у RVA, попадающих в неинициализированный хвост секции.
    [[nodiscard]] const std::uint8_t* rvaToPointer(std::uint64_t rva,
                                                   std::size_t needed) const noexcept;

private:
    PeImage() = default;

    std::vector<std::uint8_t> bytes_;
    std::vector<Section> sections_;
    std::uint64_t imageBase_ = 0;
};

} // namespace oxymp::sigcheck
