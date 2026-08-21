#include <oxymp/gamesig/resolver.hpp>

#include <oxymp/memscan/pattern.hpp>
#include <oxymp/memscan/scanner.hpp>

#include <cstring>
#include <vector>

namespace oxymp::gamesig {
namespace {

/// Все вхождения сигнатуры во всех исполняемых секциях, в виде RVA.
///
/// Ищем только в исполняемых секциях: сигнатура — это байты машинного кода, и
/// совпадение в данных было бы случайным.
///
/// По энтропии секции здесь ничего не отсеивается, и это выяснено дорого.
///
/// У GTA секций с кодом две, и у второй энтропия 7.78 против 6.65 у первой —
/// признак шифрования. Отсюда напрашивался вывод, что настоящих инструкций там
/// нет, а совпадения — случайность на тринадцати мегабайтах случайных байт. Так
/// и было сделано: вторая секция стала пропускаться.
///
/// Вывод оказался неверным. Аппаратный дозор за признаком сетевой игры поймал
/// две записи, пришедшие ровно оттуда — с rva 0x3ad92c1 и 0x3dc8e4e. Секция
/// исполняется. Высокая энтропия у неё не оттого, что код мёртв, а оттого, что
/// он расшифровывается по мере надобности и разом целиком никогда не выглядит
/// расшифрованным.
///
/// Поэтому здесь не отсеивается ничего: отчёт обязан показывать всё, что нашёл,
/// а решать, верное ли это место, — дело того, кто заводит сигнатуру.
std::vector<std::uint64_t> scanSections(const ImageSource& image,
                                        const memscan::Pattern& pattern, bool inData) {
    std::vector<std::uint64_t> hits;

    for (const Section& section : image.sections()) {
        if (section.executable == inData) {
            continue;
        }

        for (const std::size_t offset : memscan::findAll(image.sectionData(section), pattern)) {
            hits.push_back(section.rva + offset);
        }
    }

    return hits;
}

} // namespace

std::string_view describe(ResolveStatus status) noexcept {
    switch (status) {
    case ResolveStatus::Ok:
        return "ok";
    case ResolveStatus::PatternInvalid:
        return "СИГНАТУРА НЕВЕРНА";
    case ResolveStatus::NotFound:
        return "НЕ НАЙДЕНА";
    case ResolveStatus::Ambiguous:
        return "НЕОДНОЗНАЧНО";
    case ResolveStatus::Unreadable:
        return "НЕ ЧИТАЕТСЯ";
    }
    return "?";
}

std::string readCString(const ImageSource& image, std::uint64_t rva, std::size_t maxLength) {
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

Resolved resolve(const ImageSource& image, const Signature& signature) {
    Resolved result;

    const auto pattern = memscan::Pattern::parse(signature.pattern);
    if (!pattern) {
        result.status = ResolveStatus::PatternInvalid;
        result.detail = "сигнатура записана некорректно";
        return result;
    }

    const auto hits = scanSections(image, *pattern, signature.inData);
    result.matchCount = hits.size();

    if (hits.empty()) {
        result.status = ResolveStatus::NotFound;
        return result;
    }
    if (hits.size() != signature.expectedMatches) {
        result.status = ResolveStatus::Ambiguous;
        return result;
    }

    const std::int64_t site = static_cast<std::int64_t>(hits.front()) + signature.offset;
    if (site < 0) {
        result.status = ResolveStatus::Unreadable;
        result.detail = "смещение уводит за начало образа";
        return result;
    }
    result.siteRva = static_cast<std::uint64_t>(site);

    switch (signature.resolution) {
    case Resolution::Address:
        result.targetRva = result.siteRva;
        result.value = image.baseAddress() + result.siteRva;
        result.status = ResolveStatus::Ok;
        return result;

    case Resolution::RipRelative: {
        const std::uint8_t* operand = image.rvaToPointer(result.siteRva, sizeof(std::int32_t));
        if (operand == nullptr) {
            result.status = ResolveStatus::Unreadable;
            result.detail = "по адресу операнда нет данных";
            return result;
        }

        std::int32_t displacement = 0;
        std::memcpy(&displacement, operand, sizeof(displacement));

        // RIP-относительная адресация отсчитывается от конца инструкции. Обычно
        // это байт сразу за четырёхбайтным смещением, но у инструкций с
        // непосредственным операндом за смещением идёт ещё и он — на столько же
        // отстоит и конец инструкции.
        const std::int64_t target = static_cast<std::int64_t>(result.siteRva) +
                                    sizeof(displacement) + signature.tailBytes + displacement;
        if (target < 0) {
            result.status = ResolveStatus::Unreadable;
            result.detail = "смещение указывает за начало образа";
            return result;
        }

        result.targetRva = static_cast<std::uint64_t>(target);
        result.value = image.baseAddress() + result.targetRva;
        result.status = ResolveStatus::Ok;
        return result;
    }

    case Resolution::ImageRelative: {
        const std::uint8_t* operand = image.rvaToPointer(result.siteRva, sizeof(std::uint32_t));
        if (operand == nullptr) {
            result.status = ResolveStatus::Unreadable;
            result.detail = "по адресу смещения нет данных";
            return result;
        }

        std::uint32_t displacement = 0;
        std::memcpy(&displacement, operand, sizeof(displacement));

        result.targetRva = displacement;
        result.value = image.baseAddress() + result.targetRva;
        result.status = ResolveStatus::Ok;
        return result;
    }

    case Resolution::Immediate32: {
        const std::uint8_t* operand = image.rvaToPointer(result.siteRva, sizeof(std::uint32_t));
        if (operand == nullptr) {
            result.status = ResolveStatus::Unreadable;
            result.detail = "по адресу константы нет данных";
            return result;
        }

        std::uint32_t immediate = 0;
        std::memcpy(&immediate, operand, sizeof(immediate));

        result.targetRva = result.siteRva;
        result.value = immediate;
        result.status = ResolveStatus::Ok;
        return result;
    }
    }

    return result;
}

} // namespace oxymp::gamesig
