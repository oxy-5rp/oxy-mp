#pragma once

#include "image_source.hpp"

#include <memory>
#include <string>

namespace oxymp::sigcheck {

/// Образ модуля, вычитанный из памяти запущенной игры.
///
/// Работает только на чтение: процесс не изменяется, никакой код в него не
/// внедряется. Для GTA это единственный способ увидеть настоящие инструкции —
/// в файле на диске секции с кодом зашифрованы и разворачиваются лишь при запуске.
class ProcessImage final : public ImageSource {
public:
    /// Находит процесс по имени исполняемого файла и вычитывает секции его
    /// главного модуля.
    [[nodiscard]] static std::unique_ptr<ProcessImage> attach(const std::wstring& processName,
                                                              std::string& error);

    [[nodiscard]] std::uint64_t baseAddress() const noexcept override { return baseAddress_; }

    [[nodiscard]] const std::vector<Section>& sections() const noexcept override { return sections_; }

    [[nodiscard]] memscan::ByteView sectionData(const Section& section) const noexcept override;

    [[nodiscard]] const std::uint8_t* rvaToPointer(std::uint64_t rva,
                                                   std::size_t needed) const noexcept override;

    [[nodiscard]] std::string origin() const override { return origin_; }

    /// Сколько байт прочитать не удалось.
    ///
    /// Часть страниц процесса может быть недоступна — они заполняются нулями.
    /// Ненулевое значение стоит учитывать при разборе результатов поиска.
    [[nodiscard]] std::uint64_t unreadableBytes() const noexcept { return unreadableBytes_; }

private:
    ProcessImage() = default;

    /// Индекс секции в sections_ либо размер вектора, если не нашлась.
    [[nodiscard]] std::size_t indexOf(const Section& section) const noexcept;

    std::vector<Section> sections_;
    /// Содержимое секций, параллельно sections_.
    std::vector<std::vector<std::uint8_t>> data_;
    std::uint64_t baseAddress_ = 0;
    std::uint64_t unreadableBytes_ = 0;
    std::string origin_;
};

} // namespace oxymp::sigcheck
