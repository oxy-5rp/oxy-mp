// sigcheck — проверка сигнатур игрового движка.
//
// Проверяет, что каждая сигнатура из каталога находится в коде игры ровно один
// раз. Сигнатуры привязаны к конкретной сборке, поэтому до того как писать код,
// работающий с игрой, нужно знать, какие из них ещё действительны.
//
// Байты берутся из памяти запущенной игры: в файле на диске секции с кодом
// зашифрованы — это видно по энтропии в режиме --sections — и настоящих
// инструкций там нет. Работа идёт только на чтение: ничего не внедряется,
// ни файлы игры, ни её процесс не изменяются.

#include "image_source.hpp"
#include "pe_image.hpp"
#include "process_image.hpp"

#include <oxymp/gamesig/catalog.hpp>
#include <oxymp/memscan/scanner.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace {

using namespace oxymp;

/// Имя процесса игры; оно же имя её главного модуля.
const std::wstring kGameProcessName = L"GTA5.exe";

/// Дополняет строку пробелами до заданной ширины в символах.
///
/// std::format отмеряет ширину поля в байтах, а кириллица в UTF-8 занимает два
/// байта на символ — из-за этого колонки с русскими заголовками разъезжаются.
std::string pad(std::string_view text, std::size_t width) {
    std::size_t characters = 0;
    for (const char c : text) {
        // Продолжающие байты UTF-8 имеют вид 10xxxxxx и отдельными символами не являются.
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++characters;
        }
    }

    std::string result{text};
    if (characters < width) {
        result.append(width - characters, ' ');
    }
    return result;
}

enum class Status {
    Ok,           ///< Ровно столько совпадений, сколько ожидалось.
    NotFound,     ///< Совпадений нет.
    Ambiguous,    ///< Совпадений больше, чем ожидалось: доверять адресу нельзя.
    Unreadable,   ///< Совпадение найдено, но по вычисленному адресу нет данных.
};

struct Result {
    const gamesig::Signature* signature = nullptr;
    Status status = Status::NotFound;
    std::size_t matchCount = 0;
    std::uint64_t siteRva = 0;
    std::uint64_t targetRva = 0;
    std::uint64_t value = 0;
    std::string detail;
};

/// Все вхождения сигнатуры во всех исполняемых секциях, в виде RVA.
std::vector<std::uint64_t> scanExecutableSections(const sigcheck::ImageSource& image,
                                                  const memscan::Pattern& pattern) {
    std::vector<std::uint64_t> hits;

    for (const sigcheck::Section& section : image.sections()) {
        if (!section.executable) {
            continue;
        }

        for (const std::size_t offset : memscan::findAll(image.sectionData(section), pattern)) {
            hits.push_back(section.rva + offset);
        }
    }

    return hits;
}

Result evaluate(const sigcheck::ImageSource& image, const gamesig::Signature& signature) {
    Result result;
    result.signature = &signature;

    const auto pattern = memscan::Pattern::parse(signature.pattern);
    if (!pattern) {
        result.status = Status::NotFound;
        result.detail = "сигнатура записана некорректно";
        return result;
    }

    const auto hits = scanExecutableSections(image, *pattern);
    result.matchCount = hits.size();

    if (hits.empty()) {
        result.status = Status::NotFound;
        return result;
    }
    if (hits.size() != signature.expectedMatches) {
        result.status = Status::Ambiguous;
        return result;
    }

    const std::int64_t site = static_cast<std::int64_t>(hits.front()) + signature.offset;
    if (site < 0) {
        result.status = Status::Unreadable;
        result.detail = "смещение уводит за начало образа";
        return result;
    }
    result.siteRva = static_cast<std::uint64_t>(site);

    switch (signature.resolution) {
    case gamesig::Resolution::Address:
        result.targetRva = result.siteRva;
        result.value = image.baseAddress() + result.siteRva;
        result.status = Status::Ok;
        break;

    case gamesig::Resolution::RipRelative: {
        const std::uint8_t* operand = image.rvaToPointer(result.siteRva, sizeof(std::int32_t));
        if (operand == nullptr) {
            result.status = Status::Unreadable;
            result.detail = "по адресу операнда нет данных в файле";
            break;
        }

        std::int32_t displacement = 0;
        std::memcpy(&displacement, operand, sizeof(displacement));

        // RIP-относительная адресация отсчитывается от конца инструкции,
        // то есть от байта, следующего за четырёхбайтным смещением.
        const std::int64_t target =
            static_cast<std::int64_t>(result.siteRva) + sizeof(displacement) + displacement;
        if (target < 0) {
            result.status = Status::Unreadable;
            result.detail = "смещение указывает за начало образа";
            break;
        }

        result.targetRva = static_cast<std::uint64_t>(target);
        result.value = image.baseAddress() + result.targetRva;
        result.status = Status::Ok;
        break;
    }

    case gamesig::Resolution::Immediate32: {
        const std::uint8_t* operand = image.rvaToPointer(result.siteRva, sizeof(std::uint32_t));
        if (operand == nullptr) {
            result.status = Status::Unreadable;
            result.detail = "по адресу константы нет данных в файле";
            break;
        }

        std::uint32_t immediate = 0;
        std::memcpy(&immediate, operand, sizeof(immediate));

        result.targetRva = result.siteRva;
        result.value = immediate;
        result.status = Status::Ok;
        break;
    }
    }

    return result;
}

/// Строка, на которую указывает разрешённая сигнатура. Пусто, если её там нет.
std::string readString(const sigcheck::ImageSource& image, std::uint64_t rva, std::size_t maxLength) {
    const std::uint8_t* data = image.rvaToPointer(rva, 1);
    if (data == nullptr) {
        return {};
    }

    std::string text;
    for (std::size_t i = 0; i < maxLength; ++i) {
        const std::uint8_t* byte = image.rvaToPointer(rva + i, 1);
        if (byte == nullptr || *byte == 0) {
            break;
        }
        if (*byte < 0x20 || *byte > 0x7E) {
            return {};
        }
        text.push_back(static_cast<char>(*byte));
    }

    return text;
}

std::string_view statusLabel(Status status) {
    switch (status) {
    case Status::Ok:
        return "ok";
    case Status::NotFound:
        return "НЕ НАЙДЕНА";
    case Status::Ambiguous:
        return "НЕОДНОЗНАЧНО";
    case Status::Unreadable:
        return "НЕ ЧИТАЕТСЯ";
    }
    return "?";
}

std::string describeValue(const Result& result) {
    if (result.status != Status::Ok) {
        return result.detail.empty() ? std::string{} : result.detail;
    }

    switch (result.signature->resolution) {
    case gamesig::Resolution::Address:
        return std::format("функция  VA {:#x}  (rva {:#x})", result.value, result.targetRva);
    case gamesig::Resolution::RipRelative:
        return std::format("ссылка   VA {:#x}  (rva {:#x})", result.value, result.targetRva);
    case gamesig::Resolution::Immediate32:
        return std::format("значение {:#x}  ({})", result.value, result.value);
    }
    return {};
}

/// Энтропия Шеннона в битах на байт.
///
/// Нужна, чтобы отличить обычный машинный код от упакованного или зашифрованного.
/// У живого кода x86-64 много повторяющихся байт (префиксы, коды регистров, нули
/// в смещениях), поэтому он держится около 6.0-6.5. Сжатые и зашифрованные данные
/// статистически неотличимы от случайных и дают 7.9 и выше.
double shannonEntropy(memscan::ByteView data) {
    if (data.empty()) {
        return 0.0;
    }

    std::array<std::uint64_t, 256> histogram{};
    for (const std::uint8_t byte : data) {
        ++histogram[byte];
    }

    const double total = static_cast<double>(data.size());
    double entropy = 0.0;
    for (const std::uint64_t count : histogram) {
        if (count == 0) {
            continue;
        }
        const double probability = static_cast<double>(count) / total;
        entropy -= probability * std::log2(probability);
    }

    return entropy;
}

int reportSections(const sigcheck::ImageSource& image) {
    std::cout << std::format("  {}{}{}{}{}{}\n", pad("СЕКЦИЯ", 12), pad("RVA", 12),
                             pad("ВИРТ. РАЗМЕР", 16), pad("ДОСТУПНО", 16), pad("ИСП.", 8),
                             pad("ЭНТРОПИЯ", 10));
    std::cout << "  " << std::string(74, '-') << '\n';

    for (const sigcheck::Section& section : image.sections()) {
        const memscan::ByteView data = image.sectionData(section);

        std::cout << std::format("  {}{}{}{}{}{:.2f}\n", pad(section.name, 12),
                                 pad(std::format("{:x}", section.rva), 12),
                                 pad(std::format("{}", section.virtualSize), 16),
                                 pad(std::format("{}", data.size()), 16),
                                 pad(section.executable ? "да" : "нет", 8), shannonEntropy(data));
    }

    std::cout << "\n  Ориентир: обычный код x86-64 даёт около 6.0-6.5 бит на байт.\n"
                 "  Значения выше 7.5 означают сжатие или шифрование — искать сигнатуры\n"
                 "  в таком блоке бессмысленно, пока он не будет развёрнут в памяти.\n";

    return 0;
}

int probePattern(const sigcheck::ImageSource& image, std::string_view patternText) {
    const auto pattern = memscan::Pattern::parse(patternText);
    if (!pattern) {
        std::cerr << "Сигнатура записана некорректно.\n";
        return 2;
    }

    std::cout << std::format("  Сигнатура: {}\n\n", patternText);

    std::size_t total = 0;
    for (const sigcheck::Section& section : image.sections()) {
        const auto hits = memscan::findAll(image.sectionData(section), *pattern);
        total += hits.size();

        std::cout << std::format("  {:<10}{:>10} совпадений\n", section.name, hits.size());
    }

    std::cout << std::format("\n  Всего: {}\n", total);
    return total > 0 ? 0 : 1;
}

const Result* findResult(const std::vector<Result>& results, std::string_view id) {
    for (const Result& result : results) {
        if (result.signature->id == id) {
            return &result;
        }
    }
    return nullptr;
}

void printUsage() {
    std::cerr << "Использование:\n"
                 "  sigcheck [источник] [режим]\n\n"
                 "Источник байт:\n"
                 "  --attach            память запущенной игры (по умолчанию)\n"
                 "  --game <путь>       файл на диске; секции с кодом там зашифрованы,\n"
                 "                      поэтому годится только для --sections\n\n"
                 "Режимы:\n"
                 "  (без режима)        проверить весь каталог сигнатур\n"
                 "  --sections          показать секции образа и их энтропию\n"
                 "  --probe <сигнатура> посчитать совпадения произвольной сигнатуры\n";
}

int run(int argc, char** argv) {
    std::filesystem::path gamePath;
    std::string_view mode;
    std::string_view probe;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];

        if (argument == "--game") {
            if (++i >= argc) {
                printUsage();
                return 2;
            }
            gamePath = argv[i];
        } else if (argument == "--attach") {
            gamePath.clear();
        } else if (argument == "--sections") {
            mode = argument;
        } else if (argument == "--probe") {
            if (++i >= argc) {
                printUsage();
                return 2;
            }
            mode = argument;
            probe = argv[i];
        } else {
            printUsage();
            return 2;
        }
    }

    std::cout << "oxyMP · sigcheck — проверка сигнатур игрового движка\n\n";

    std::string error;
    std::unique_ptr<sigcheck::ImageSource> image;

    if (gamePath.empty()) {
        auto attached = sigcheck::ProcessImage::attach(kGameProcessName, error);
        if (attached && attached->unreadableBytes() != 0) {
            std::cout << std::format("  Предупреждение: {} байт памяти прочитать не удалось,\n"
                                     "  эти участки заполнены нулями.\n",
                                     attached->unreadableBytes());
        }
        image = std::move(attached);
    } else {
        image = sigcheck::PeImage::load(gamePath, error);
    }

    if (!image) {
        std::cerr << std::format("Не удалось получить образ: {}\n", error);
        return 2;
    }

    std::cout << std::format("  Источник        {}\n", image->origin());
    std::cout << std::format("  Каталог выверен под сборку {}\n", gamesig::kTargetGameVersion);
    std::cout << std::format("  База образа     {:#x}\n\n", image->baseAddress());

    if (mode == "--sections") {
        return reportSections(*image);
    }
    if (mode == "--probe") {
        return probePattern(*image, probe);
    }

    std::vector<Result> results;
    results.reserve(gamesig::catalog().size());
    for (const gamesig::Signature& signature : gamesig::catalog()) {
        results.push_back(evaluate(*image, signature));
    }

    // Дата сборки — самая надёжная проверка того, что каталог вообще применим
    // к этому исполняемому файлу, поэтому она идёт до разбора остальных сигнатур.
    bool buildMatches = true;
    if (const Result* buildDate = findResult(results, "build_date_string");
        buildDate != nullptr && buildDate->status == Status::Ok) {
        const std::string text = readString(*image, buildDate->targetRva, 32);

        buildMatches = text == gamesig::kVerifiedBuildDate;
        std::cout << std::format("  Дата сборки     \"{}\"{}\n\n", text,
                                 buildMatches ? "" : "  <- НЕ СОВПАДАЕТ С КАТАЛОГОМ");

        if (!buildMatches) {
            std::cout << std::format("  Каталог выверялся на сборке от \"{}\". Игра обновилась,\n"
                                     "  и часть сигнатур может указывать не туда.\n\n",
                                     gamesig::kVerifiedBuildDate);
        }
    }

    std::cout << std::format("  {}{}{}{}\n", pad("ИДЕНТИФИКАТОР", 30), pad("СОВП.", 8),
                             pad("СТАТУС", 16), "РЕЗУЛЬТАТ");
    std::cout << "  " << std::string(96, '-') << '\n';

    std::size_t okCount = 0;
    for (const Result& result : results) {
        if (result.status == Status::Ok) {
            ++okCount;
        }

        std::cout << std::format("  {}{}{}{}\n", pad(result.signature->id, 30),
                                 pad(std::format("{}", result.matchCount), 8),
                                 pad(statusLabel(result.status), 16), describeValue(result));
    }

    std::cout << std::format("\n  Итого: {} из {} сигнатур разрешились однозначно.\n", okCount,
                             results.size());

    if (okCount != results.size()) {
        std::cout << "\n  Не разрешившиеся сигнатуры нужно уточнить в отладчике.\n"
                     "  «НЕОДНОЗНАЧНО» означает, что сигнатуру надо удлинить,\n"
                     "  «НЕ НАЙДЕНА» — что окрестности кода изменились.\n";
        return 1;
    }

    return buildMatches ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    return run(argc, argv);
}
