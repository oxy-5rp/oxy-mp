#pragma once

#include "image_source.hpp"

#include <filesystem>
#include <memory>

namespace oxymp::sigcheck {

/// Образ, прочитанный с диска как обычный файл.
///
/// Игра при этом не запускается и не изменяется. Для GTA этот источник почти
/// бесполезен: секции с кодом зашифрованы, и осмысленных совпадений в них нет —
/// но он остаётся нужен, чтобы это увидеть (режим --sections показывает энтропию).
class PeImage final : public ImageSource {
public:
    [[nodiscard]] static std::unique_ptr<PeImage> load(const std::filesystem::path& path,
                                                       std::string& error);

    [[nodiscard]] std::uint64_t baseAddress() const noexcept override { return imageBase_; }

    [[nodiscard]] const std::vector<Section>& sections() const noexcept override { return sections_; }

    [[nodiscard]] memscan::ByteView sectionData(const Section& section) const noexcept override;

    [[nodiscard]] const std::uint8_t* rvaToPointer(std::uint64_t rva,
                                                   std::size_t needed) const noexcept override;

    [[nodiscard]] std::string origin() const override { return origin_; }

private:
    PeImage() = default;

    /// Сколько байт секции реально присутствует в файле.
    ///
    /// Виртуальный размер бывает больше файлового (хвост, который загрузчик обнулит),
    /// а файловый — больше виртуального (выравнивание). Искать имеет смысл только
    /// в пересечении.
    [[nodiscard]] static std::uint32_t availableSize(const Section& section) noexcept;

    std::vector<std::uint8_t> bytes_;
    std::vector<Section> sections_;
    std::uint64_t imageBase_ = 0;
    std::string origin_;
};

} // namespace oxymp::sigcheck
