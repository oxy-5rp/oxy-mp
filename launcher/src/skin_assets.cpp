#include "skin_assets.hpp"

#include <oxymp/config/skin.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace oxymp::launcher {
namespace {

/// Размер окна, когда оформления нет.
///
/// Тот же, что у alt:V с его образцовым skin.bin: 300 на 400. Взято не с
/// потолка — под этот размер нарисован фон в образце, и окно без оформления
/// должно быть таким же, иначе смена оформления меняла бы ещё и размер окна.
constexpr int kDefaultWidth = 300;
constexpr int kDefaultHeight = 400;

/// Разбирает base64 в байты.
///
/// Свой разбор, а не библиотека: base64 — это таблица на 64 знака и сдвиги, и
/// заводить ради них зависимость значило бы менять понятные тридцать строк на
/// непонятную сотню килобайт. Строки сюда приходят из файла оформления, то есть
/// от стороннего средства, поэтому негодный знак — не исключение, а обычный
/// отказ: пустой ответ означает «оформления нет».
[[nodiscard]] std::vector<std::uint8_t> decodeBase64(std::string_view text) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::array<std::int8_t, 256> back{};
    back.fill(-1);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        back[static_cast<unsigned char>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 4 * 3);

    std::uint32_t accumulator = 0;
    int bits = 0;

    for (const char symbol : text) {
        // Перевод строки и пробелы внутри base64 — обычное дело: так его
        // переносят те, кто складывает файл вручную.
        if (symbol == '\n' || symbol == '\r' || symbol == ' ' || symbol == '\t') {
            continue;
        }

        if (symbol == '=') {
            break;
        }

        const std::int8_t value = back[static_cast<unsigned char>(symbol)];
        if (value < 0) {
            return {};
        }

        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return bytes;
}

/// Размер картинки PNG, взятый из её заголовка.
///
/// Первый блок PNG — IHDR, и ширина с высотой лежат в нём на известном месте
/// старшим байтом вперёд. Ничего, кроме размера, отсюда не нужно: рисовать
/// картинку будет страница, а окну довольно знать, какой ей быть.
struct ImageSize {
    int width = 0;
    int height = 0;
};

[[nodiscard]] std::optional<ImageSize> pngSize(const std::vector<std::uint8_t>& bytes) {
    constexpr std::array<std::uint8_t, 8> kSignature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    constexpr std::size_t kHeaderEnd = 24;

    if (bytes.size() < kHeaderEnd ||
        std::memcmp(bytes.data(), kSignature.data(), kSignature.size()) != 0) {
        return std::nullopt;
    }

    const auto number = [&bytes](std::size_t at) {
        return (static_cast<std::uint32_t>(bytes[at]) << 24) |
               (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 8) |
               static_cast<std::uint32_t>(bytes[at + 3]);
    };

    const std::uint32_t width = number(16);
    const std::uint32_t height = number(20);

    // Заведомо негодный размер лучше отвергнуть здесь: окно шириной в полмиллиона
    // точек Windows создаст, и увидеть его будет уже нельзя.
    constexpr std::uint32_t kSane = 4096;
    if (width == 0 || height == 0 || width > kSane || height > kSane) {
        return std::nullopt;
    }

    return ImageSize{static_cast<int>(width), static_cast<int>(height)};
}

/// Запись оглавления файла .ico.
///
/// Файл начинается шестибайтным заголовком, за ним идут такие записи — по одной
/// на размер значка. Разбираются они здесь, а не средствами Windows, и это не
/// упрямство: LookupIconIdFromDirectoryEx читает оглавление того вида, в каком
/// оно лежит внутри исполняемого файла, а у файла на диске оно другое — вместо
/// номера ресурса там смещение.
#pragma pack(push, 1)
struct IconEntry {
    std::uint8_t width;
    std::uint8_t height;
    std::uint8_t colours;
    std::uint8_t reserved;
    std::uint16_t planes;
    std::uint16_t bits;
    std::uint32_t size;
    std::uint32_t offset;
};
#pragma pack(pop)

/// Делает значок нужного размера из содержимого файла .ico.
///
/// Windows масштабирует сама: ей отдаётся самое крупное изображение из файла, а
/// желаемый размер она берёт из доводов. Иначе значок в панели задач оказался бы
/// уменьшенной копией того, что нарисовано для заголовка окна.
[[nodiscard]] HICON makeIcon(const std::vector<std::uint8_t>& bytes, int wanted) {
    constexpr std::size_t kDirectory = 6;

    if (bytes.size() < kDirectory) {
        return nullptr;
    }

    std::uint16_t count = 0;
    std::memcpy(&count, bytes.data() + 4, sizeof(count));

    if (count == 0 || bytes.size() < kDirectory + count * sizeof(IconEntry)) {
        return nullptr;
    }

    IconEntry chosen{};
    int chosenWidth = 0;

    for (std::uint16_t i = 0; i < count; ++i) {
        IconEntry entry{};
        std::memcpy(&entry, bytes.data() + kDirectory + i * sizeof(IconEntry), sizeof(entry));

        if (entry.offset + entry.size > bytes.size()) {
            continue;
        }

        // Нулевая ширина в оглавлении означает 256 точек: в байт это число не
        // помещается, и так его записывают со времён Windows Vista.
        const int width = entry.width == 0 ? 256 : entry.width;

        if (width > chosenWidth) {
            chosen = entry;
            chosenWidth = width;
        }
    }

    if (chosenWidth == 0) {
        return nullptr;
    }

    // Версия 0x00030000 — та, которую Windows понимает как «обычный значок»;
    // другой у этой функции не бывает.
    constexpr DWORD kIconVersion = 0x00030000;

    return ::CreateIconFromResourceEx(const_cast<PBYTE>(bytes.data() + chosen.offset), chosen.size,
                                      TRUE, kIconVersion, wanted, wanted, LR_DEFAULTCOLOR);
}

/// Ссылка с данными для страницы. Пустая картинка остаётся пустой строкой.
[[nodiscard]] std::string dataUrl(const std::string& base64) {
    if (base64.empty()) {
        return {};
    }

    return "data:image/png;base64," + base64;
}

} // namespace

SkinAssets SkinAssets::load(const std::filesystem::path& skinFile) {
    SkinAssets assets;
    assets.width = kDefaultWidth;
    assets.height = kDefaultHeight;

    const std::optional<config::Skin> skin = config::Skin::load(skinFile);
    if (!skin.has_value()) {
        return assets;
    }

    assets.name = skin->launcherName.empty() ? skin->gameName : skin->launcherName;
    assets.background = dataUrl(skin->launcherBackground);

    if (!skin->primaryColor.empty()) {
        assets.accent = "#" + skin->primaryColor;
    }

    // Размер окна — размер картинки. Не совпади они, фон либо растянулся бы, либо
    // оставил полосу пустоты; alt:V решает это так же.
    const std::vector<std::uint8_t> background = decodeBase64(skin->launcherBackground);

    if (const std::optional<ImageSize> size = pngSize(background)) {
        assets.width = size->width;
        assets.height = size->height;
    } else if (!skin->launcherBackground.empty()) {
        spdlog::warn("фон лаунчера не разобрался как PNG — окно останется {}x{}", assets.width,
                     assets.height);
    }

    if (!skin->icon.empty()) {
        const std::vector<std::uint8_t> icon = decodeBase64(skin->icon);

        assets.largeIcon = makeIcon(icon, ::GetSystemMetrics(SM_CXICON));
        assets.smallIcon = makeIcon(icon, ::GetSystemMetrics(SM_CXSMICON));

        if (assets.largeIcon == nullptr) {
            spdlog::warn("значок из оформления не разобрался — останется свой");
        }
    }

    spdlog::info("оформление окна: {}, {}x{}", assets.name.empty() ? "без имени" : assets.name,
                 assets.width, assets.height);

    return assets;
}

} // namespace oxymp::launcher
