#pragma once

#include <oxymp/memscan/scanner.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace oxymp::sigcheck {

/// Секция PE-образа.
struct Section {
    std::string name;
    std::uint32_t rva = 0;
    std::uint32_t virtualSize = 0;

    /// Размер и положение в файле. У образа, прочитанного из памяти процесса,
    /// смысла не имеют и остаются нулевыми.
    std::uint32_t rawSize = 0;
    std::uint32_t fileOffset = 0;

    bool executable = false;
};

/// Источник байт образа для поиска сигнатур.
///
/// Существует ровно затем, чтобы отчёт и разбор каталога не зависели от того,
/// откуда взялись байты. Исполняемый файл GTA зашифрован на диске, поэтому
/// осмысленный поиск возможен только по памяти запущенной игры — но логика
/// проверки при этом не меняется ни на строчку.
class ImageSource {
public:
    virtual ~ImageSource() = default;

    ImageSource(const ImageSource&) = delete;
    ImageSource& operator=(const ImageSource&) = delete;

    /// Адрес, по которому образ фактически расположен.
    ///
    /// Для файла это предпочтительная база загрузки, для процесса — настоящая,
    /// уже сдвинутая ASLR. Поэтому сравнивать между запусками имеет смысл RVA.
    [[nodiscard]] virtual std::uint64_t baseAddress() const noexcept = 0;

    [[nodiscard]] virtual const std::vector<Section>& sections() const noexcept = 0;

    /// Байты секции, доступные для поиска. Могут быть короче виртуального размера.
    [[nodiscard]] virtual memscan::ByteView sectionData(const Section& section) const noexcept = 0;

    /// RVA в указатель на доступные данные. nullptr, если данных нет или их
    /// меньше, чем needed.
    [[nodiscard]] virtual const std::uint8_t* rvaToPointer(std::uint64_t rva,
                                                           std::size_t needed) const noexcept = 0;

    /// Откуда взят образ — одной строкой для отчёта.
    [[nodiscard]] virtual std::string origin() const = 0;

protected:
    ImageSource() = default;
};

} // namespace oxymp::sigcheck
