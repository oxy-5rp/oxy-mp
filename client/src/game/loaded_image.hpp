#pragma once

#include <oxymp/gamesig/image_source.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oxymp::client::game {

/// Главный модуль игры, каким он виден изнутри её собственного процесса.
///
/// От источников оффлайн-утилиты отличается тем, что ничего никуда не копирует:
/// код игры уже разложен загрузчиком по своим адресам и расшифрован, поэтому
/// секции отдаются прямыми диапазонами памяти. Отсюда же простое соотношение
/// «адрес = база + RVA», которого нет у образа, прочитанного из файла.
class LoadedImage final : public gamesig::ImageSource {
public:
    /// Разбирает заголовки главного модуля текущего процесса.
    ///
    /// Главный модуль — это GTA5.exe: клиент выполняется внутри неё, поэтому
    /// разыскивать процесс и открывать его описатель не требуется.
    [[nodiscard]] static std::unique_ptr<LoadedImage> open(std::string& error);

    [[nodiscard]] std::uint64_t baseAddress() const noexcept override { return base_; }

    [[nodiscard]] const std::vector<gamesig::Section>& sections() const noexcept override {
        return sections_;
    }

    [[nodiscard]] memscan::ByteView sectionData(
        const gamesig::Section& section) const noexcept override;

    [[nodiscard]] const std::uint8_t* rvaToPointer(std::uint64_t rva,
                                                   std::size_t needed) const noexcept override;

    [[nodiscard]] std::string origin() const override { return origin_; }

private:
    LoadedImage() = default;

    std::vector<gamesig::Section> sections_;
    std::uint64_t base_ = 0;

    /// Размер образа в памяти. Нужен, чтобы ни один запрос не увёл за пределы
    /// отображённого модуля: там начинается чужая память.
    std::uint64_t imageSize_ = 0;

    std::string origin_;
};

} // namespace oxymp::client::game
