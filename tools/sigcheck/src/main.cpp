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

#include "pe_image.hpp"
#include "process_image.hpp"

#include <oxymp/gamesig/catalog.hpp>
#include <oxymp/gamesig/image_source.hpp>
#include <oxymp/gamesig/resolver.hpp>
#include <oxymp/memscan/scanner.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
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

/// Строка отчёта: сигнатура и то, чем закончилось её разрешение.
struct Result {
    const gamesig::Signature* signature = nullptr;
    gamesig::Resolved resolved;
};

std::string describeValue(const Result& result) {
    const gamesig::Resolved& resolved = result.resolved;

    if (!resolved.ok()) {
        return resolved.detail;
    }

    switch (result.signature->resolution) {
    case gamesig::Resolution::Address:
        return std::format("функция  VA {:#x}  (rva {:#x})", resolved.value, resolved.targetRva);
    case gamesig::Resolution::RipRelative:
        return std::format("ссылка   VA {:#x}  (rva {:#x})", resolved.value, resolved.targetRva);
    case gamesig::Resolution::Immediate32:
        return std::format("значение {:#x}  ({})", resolved.value, resolved.value);
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

int reportSections(const gamesig::ImageSource& image) {
    std::cout << std::format("  {}{}{}{}{}{}\n", pad("СЕКЦИЯ", 12), pad("RVA", 12),
                             pad("ВИРТ. РАЗМЕР", 16), pad("ДОСТУПНО", 16), pad("ИСП.", 8),
                             pad("ЭНТРОПИЯ", 10));
    std::cout << "  " << std::string(74, '-') << '\n';

    for (const gamesig::Section& section : image.sections()) {
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

int probePattern(const gamesig::ImageSource& image, std::string_view patternText) {
    const auto pattern = memscan::Pattern::parse(patternText);
    if (!pattern) {
        std::cerr << "Сигнатура записана некорректно.\n";
        return 2;
    }

    std::cout << std::format("  Сигнатура: {}\n\n", patternText);

    /// Сколько адресов показывать. Неоднозначную сигнатуру всё равно придётся
    /// уточнять, и полный список сотни совпадений для этого не нужен.
    constexpr std::size_t kShownHits = 8;

    std::size_t total = 0;
    for (const gamesig::Section& section : image.sections()) {
        const auto hits = memscan::findAll(image.sectionData(section), *pattern);
        total += hits.size();

        std::cout << std::format("  {:<10}{:>10} совпадений\n", section.name, hits.size());

        // Адреса, а не только число: по числу видно, годится ли сигнатура, а
        // работать дальше — разбирать код вокруг — можно только по адресу.
        for (std::size_t i = 0; i < hits.size() && i < kShownHits; ++i) {
            const std::uint64_t rva = section.rva + hits[i];

            std::cout << std::format("              rva {:#x}  (VA {:#x})\n", rva,
                                     image.baseAddress() + rva);
        }
    }

    std::cout << std::format("\n  Всего: {}\n", total);
    return total > 0 ? 0 : 1;
}

/// Разбирает цель, заданную идентификатором сигнатуры либо RVA вида 0x1234.
///
/// У идентификатора берётся то, во что сигнатура разрешилась, а не место
/// совпадения: спрашивают обычно про признак или функцию, а не про байты,
/// по которым их нашли.
std::optional<std::uint64_t> resolveTarget(const gamesig::ImageSource& image,
                                           std::string_view target) {
    if (const gamesig::Signature* signature = gamesig::find(target); signature != nullptr) {
        const gamesig::Resolved resolved = gamesig::resolve(image, *signature);
        if (!resolved.ok()) {
            std::cerr << std::format("Сигнатура \"{}\" не разрешилась: {}\n", target,
                                     gamesig::describe(resolved.status));
            return std::nullopt;
        }

        return resolved.targetRva;
    }

    const auto* begin = target.data();
    const auto* end = begin + target.size();
    if (target.starts_with("0x") || target.starts_with("0X")) {
        begin += 2;
    }

    std::uint64_t rva = 0;
    if (std::from_chars(begin, end, rva, 16).ptr != end) {
        std::cerr << std::format("\"{}\" — не идентификатор из каталога и не RVA\n", target);
        return std::nullopt;
    }

    return rva;
}

/// Границы функции, которой принадлежит адрес, по таблице раскрутки стека.
///
/// Нужно, чтобы от места, пойманного дозором, дойти до функции целиком: дозор
/// называет инструкцию, а перехватывать и искать вызывающих можно только
/// функцию.
///
/// Искать пролог перебором назад — гадание: у обфусцированного кода начало
/// выглядит как угодно. Но у 64-разрядной Windows есть точный ответ. Раскрутка
/// стека при исключении требует знать границы каждой функции, поэтому
/// компилятор складывает их в секцию .pdata — по три числа на функцию: начало,
/// конец и описание раскрутки. Таблица отсортирована по началу, так что нужная
/// запись ищется делением пополам.
///
/// Эти границы не догадка и не эвристика: по ним раскручивает стек сама Windows.
int reportFunction(const gamesig::ImageSource& image, std::string_view target) {
    const auto wanted = resolveTarget(image, target);
    if (!wanted) {
        return 2;
    }

    const gamesig::Section* pdata = nullptr;
    for (const gamesig::Section& section : image.sections()) {
        if (section.name == ".pdata") {
            pdata = &section;
            break;
        }
    }

    if (pdata == nullptr) {
        std::cerr << "В образе нет секции .pdata — границы функций взять неоткуда.\n";
        return 1;
    }

    /// Запись таблицы раскрутки: три четырёхбайтных RVA.
    struct RuntimeFunction {
        std::uint32_t begin;
        std::uint32_t end;
        std::uint32_t unwind;
    };

    const memscan::ByteView data = image.sectionData(*pdata);
    const std::size_t count = data.size() / sizeof(RuntimeFunction);

    std::cout << std::format("  Ищем функцию, которой принадлежит rva {:#x}\n", *wanted);
    std::cout << std::format("  В таблице .pdata записей: {}\n\n", count);

    // Деление пополам: таблица отсортирована по началу функции.
    std::size_t low = 0;
    std::size_t high = count;
    RuntimeFunction found{};
    bool ok = false;

    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;

        RuntimeFunction entry{};
        std::memcpy(&entry, data.data() + middle * sizeof(RuntimeFunction), sizeof(entry));

        if (*wanted < entry.begin) {
            high = middle;
        } else if (*wanted >= entry.end) {
            low = middle + 1;
        } else {
            found = entry;
            ok = true;
            break;
        }
    }

    if (!ok) {
        std::cout << "  Функция не найдена: адрес не попал ни в одну запись таблицы.\n"
                     "  Так бывает у кода, который создан на ходу, — а у обфусцированных\n"
                     "  участков GTA это обычное дело.\n";
        return 1;
    }

    std::cout << std::format("  Начало  rva {:#x}  (VA {:#x})\n", found.begin,
                             image.baseAddress() + found.begin);
    std::cout << std::format("  Конец   rva {:#x}  (VA {:#x})\n", found.end,
                             image.baseAddress() + found.end);
    std::cout << std::format("  Размер  {} байт\n", found.end - found.begin);
    std::cout << std::format("  Адрес внутри неё на {} байт от начала\n\n",
                             *wanted - found.begin);

    std::cout << std::format("  Кто её зовёт: sigcheck --xrefs {:#x}\n", found.begin);

    return 0;
}

/// Все места в коде, которые ссылаются на заданный адрес.
///
/// Нужно, чтобы чинить поведение игры поимённо, не трогая её глобальных
/// признаков. Признак вроде «идёт сетевая игра» читают десятки мест, и менять
/// его целиком нельзя — проверено, игра падает. А вот перенаправить одну
/// конкретную проверку на свой байт можно, и для этого нужно сперва увидеть
/// весь список читающих.
///
/// Ищутся RIP-относительные ссылки: в x86-64 обращение к глобальной переменной
/// почти всегда такое. Смещение отсчитывается от конца инструкции, а конец
/// зависит от того, есть ли за смещением непосредственный операнд, — поэтому
/// проверяются оба случая, и в отчёте видно, какой из них подошёл.
///
/// Ложных срабатываний практически не бывает, и это считается, а не
/// предполагается: совпасть должны все четыре байта смещения, то есть один
/// случай на 2^32 позиций. На 27 мегабайтах кода это сотые доли одного
/// ожидаемого совпадения.
int reportReferences(const gamesig::ImageSource& image, std::string_view target) {
    const auto wanted = resolveTarget(image, target);
    if (!wanted) {
        return 2;
    }

    std::cout << std::format("  Ищем ссылки на rva {:#x}  (VA {:#x})\n\n", *wanted,
                             image.baseAddress() + *wanted);

    /// Сколько байт инструкции может идти после четырёхбайтного смещения.
    /// Ноль — смещение последнее; единица — за ним непосредственный операнд,
    /// как у `cmp byte ptr [rip+X], 0`.
    constexpr std::ptrdiff_t kTails[] = {0, 1};

    /// Сколько ссылок показывать по секции.
    ///
    /// Много: список нужен не для чтения глазами, а чтобы отобрать из него
    /// пишущих по коду инструкции. Отбор без полного списка невозможен.
    constexpr std::size_t kShownPerSection = 4096;

    /// Сколько байт инструкции показывать перед смещением.
    ///
    /// Двух хватает на код с модрм — `c6 05`, `88 05`, `8a 05`, `38 3d`, — а
    /// третий берётся на случай префикса вроде REX.
    constexpr std::size_t kOpcodeBytes = 3;

    std::size_t found = 0;

    for (const gamesig::Section& section : image.sections()) {
        if (!section.executable) {
            continue;
        }

        const memscan::ByteView data = image.sectionData(section);
        if (data.size() < sizeof(std::int32_t)) {
            continue;
        }

        std::size_t here = 0;

        for (std::size_t offset = 0; offset + sizeof(std::int32_t) <= data.size(); ++offset) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, data.data() + offset, sizeof(displacement));

            for (const std::ptrdiff_t tail : kTails) {
                const std::int64_t end = static_cast<std::int64_t>(section.rva) +
                                         static_cast<std::int64_t>(offset) +
                                         static_cast<std::int64_t>(sizeof(displacement)) + tail;

                if (end + displacement != static_cast<std::int64_t>(*wanted)) {
                    continue;
                }

                ++here;
                ++found;

                if (here > kShownPerSection) {
                    continue;
                }

                // Байты перед смещением — это код инструкции, и по нему видно
                // главное: читает она признак или пишет в него. Читателей у
                // такого признака могут быть тысячи, а пишущих единицы, и
                // именно они интересны — это они поднимают и кладут состояние.
                //
                // Разбирать инструкцию целиком здесь незачем: длина кода зависит
                // от префиксов, а глазами `c6 05` (запись числа) и `88 05`
                // (запись из регистра) отличаются от `38 3d` и `8a 05` сразу.
                std::string before;
                for (std::size_t back = kOpcodeBytes; back >= 1; --back) {
                    if (offset < back) {
                        before += "?? ";
                        continue;
                    }
                    before += std::format("{:02x} ", data[offset - back]);
                }

                std::cout << std::format("  {:<10} rva {:#010x}  хвост {}  код  {}\n",
                                         section.name, section.rva + offset, tail, before);
            }
        }

        // Счётчик по секции печатается всегда, даже когда ссылок нет: по нему
        // сразу видно, в настоящем ли коде мы их нашли. Вторая секция .text у
        // GTA зашифрована, и совпадения в ней — это совпадения в шуме.
        std::cout << std::format("  {:<10} итого {} ссылок (энтропия {:.2f})\n\n", section.name,
                                 here, shannonEntropy(data));
    }

    std::cout << std::format("  Всего ссылок: {}\n", found);

    if (found != 0) {
        std::cout << "\n  Дальше — смотреть код вокруг каждой: --dump <rva>. Инструкция\n"
                     "  начинается на несколько байт раньше смещения, сколько именно —\n"
                     "  зависит от её кода и префиксов.\n";
    }

    return found > 0 ? 0 : 1;
}

const Result* findResult(const std::vector<Result>& results, std::string_view id) {
    for (const Result& result : results) {
        if (result.signature->id == id) {
            return &result;
        }
    }
    return nullptr;
}

/// Шестнадцатеричный дамп участка образа.
///
/// Нужен, чтобы разбирать код игры вокруг найденной сигнатуры: структуры движка
/// обфусцированы, и единственный надёжный источник знаний о том, как они
/// устроены, — это машинный код, который сама игра по ним ходит.
///
/// Цель задаётся либо идентификатором сигнатуры (тогда показывается код вокруг
/// места совпадения), либо RVA напрямую.
int dumpRegion(const gamesig::ImageSource& image, std::string_view target, std::size_t length) {
    /// Сколько байт показать до места совпадения: начало функции обычно раньше.
    constexpr std::uint64_t kContextBefore = 0x40;

    std::uint64_t rva = 0;

    if (const gamesig::Signature* signature = gamesig::find(target); signature != nullptr) {
        const gamesig::Resolved resolved = gamesig::resolve(image, *signature);
        if (!resolved.ok()) {
            std::cerr << std::format("Сигнатура \"{}\" не разрешилась: {}\n", target,
                                     gamesig::describe(resolved.status));
            return 1;
        }

        rva = resolved.siteRva > kContextBefore ? resolved.siteRva - kContextBefore : 0;

        std::cout << std::format("  Сигнатура       {}\n", target);
        std::cout << std::format("  Место совпадения rva {:#x}  (VA {:#x})\n", resolved.siteRva,
                                 image.baseAddress() + resolved.siteRva);
        std::cout << std::format("  Разрешилась в   {:#x}\n\n", resolved.value);
    } else {
        const auto* begin = target.data();
        const auto* end = begin + target.size();
        if (target.starts_with("0x") || target.starts_with("0X")) {
            begin += 2;
        }

        if (std::from_chars(begin, end, rva, 16).ptr != end) {
            std::cerr << std::format("\"{}\" — не идентификатор из каталога и не RVA\n", target);
            return 2;
        }
    }

    std::cout << std::format("  Дамп с rva {:#x}, {} байт\n\n", rva, length);

    constexpr std::size_t kPerLine = 16;

    for (std::size_t offset = 0; offset < length; offset += kPerLine) {
        const std::uint8_t* line = image.rvaToPointer(rva + offset, 1);
        if (line == nullptr) {
            std::cout << std::format("  {:08x}  <данных нет>\n", rva + offset);
            break;
        }

        std::string hex;
        std::string text;

        for (std::size_t i = 0; i < kPerLine; ++i) {
            const std::uint8_t* byte = image.rvaToPointer(rva + offset + i, 1);
            if (byte == nullptr) {
                hex += "   ";
                text.push_back(' ');
                continue;
            }

            hex += std::format("{:02x} ", *byte);
            text.push_back(*byte >= 0x20 && *byte <= 0x7E ? static_cast<char>(*byte) : '.');
        }

        std::cout << std::format("  {:08x}  {} {}\n", rva + offset, hex, text);
    }

    return 0;
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
                 "  --probe <сигнатура> посчитать совпадения произвольной сигнатуры\n"
                 "  --dump <что> [байт] шестнадцатеричный дамп; <что> — идентификатор\n"
                 "                      сигнатуры из каталога либо RVA вида 0x1234\n"
                 "  --xrefs <что>       все места кода, ссылающиеся на этот адрес.\n"
                 "                      Нужно, чтобы перенаправить одну проверку, не трогая\n"
                 "                      признак игры целиком\n";
}

int run(int argc, char** argv) {
    std::filesystem::path gamePath;
    std::string_view mode;
    std::string_view probe;
    std::string_view dumpTarget;
    std::string_view xrefTarget;
    std::string_view funcTarget;
    std::size_t dumpLength = 0x100;

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
        } else if (argument == "--func") {
            if (++i >= argc) {
                printUsage();
                return 2;
            }
            mode = argument;
            funcTarget = argv[i];
        } else if (argument == "--xrefs") {
            if (++i >= argc) {
                printUsage();
                return 2;
            }
            mode = argument;
            xrefTarget = argv[i];
        } else if (argument == "--dump") {
            if (++i >= argc) {
                printUsage();
                return 2;
            }
            mode = argument;
            dumpTarget = argv[i];

            // Длина необязательна: следующий довод берётся, только если это число.
            if (i + 1 < argc) {
                const std::string_view next = argv[i + 1];
                std::size_t parsed = 0;
                if (std::from_chars(next.data(), next.data() + next.size(), parsed).ptr ==
                        next.data() + next.size() &&
                    parsed != 0) {
                    dumpLength = parsed;
                    ++i;
                }
            }
        } else {
            printUsage();
            return 2;
        }
    }

    std::cout << "oxyMP · sigcheck — проверка сигнатур игрового движка\n\n";

    std::string error;
    std::unique_ptr<gamesig::ImageSource> image;

    bool clientLoaded = false;

    if (gamePath.empty()) {
        auto attached = sigcheck::ProcessImage::attach(kGameProcessName, error);
        if (attached && attached->unreadableBytes() != 0) {
            std::cout << std::format("  Предупреждение: {} байт памяти прочитать не удалось,\n"
                                     "  эти участки заполнены нулями.\n",
                                     attached->unreadableBytes());
        }
        if (attached) {
            clientLoaded = attached->clientLoaded();
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
    if (mode == "--func") {
        return reportFunction(*image, funcTarget);
    }
    if (mode == "--xrefs") {
        return reportReferences(*image, xrefTarget);
    }
    if (mode == "--dump") {
        return dumpRegion(*image, dumpTarget, dumpLength);
    }

    std::vector<Result> results;
    results.reserve(gamesig::catalog().size());
    for (const gamesig::Signature& signature : gamesig::catalog()) {
        results.push_back(Result{&signature, gamesig::resolve(*image, signature)});
    }

    // Дата сборки — самая надёжная проверка того, что каталог вообще применим
    // к этому исполняемому файлу, поэтому она идёт до разбора остальных сигнатур.
    bool buildMatches = true;
    if (const Result* buildDate = findResult(results, "build_date_string");
        buildDate != nullptr && buildDate->resolved.ok()) {
        const std::string text = gamesig::readCString(*image, buildDate->resolved.targetRva, 32);

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
    std::size_t requiredFailures = 0;

    for (const Result& result : results) {
        if (result.resolved.ok()) {
            ++okCount;
        } else if (result.signature->required) {
            ++requiredFailures;
        }

        // Разведочные сигнатуры помечаются прямо в строке. Путать их с
        // остальными в отчёте нельзя: неудача обязательной означает сломанный
        // каталог и неработающий клиент, а неудача разведочной — всего лишь
        // ещё не сделанную работу.
        const std::string id = result.signature->required
                                   ? std::string{result.signature->id}
                                   : std::format("{} (разведка)", result.signature->id);

        std::cout << std::format("  {}{}{}{}\n", pad(id, 30),
                                 pad(std::format("{}", result.resolved.matchCount), 8),
                                 pad(gamesig::describe(result.resolved.status), 16),
                                 describeValue(result));
    }

    std::cout << std::format("\n  Итого: {} из {} сигнатур разрешились однозначно.\n", okCount,
                             results.size());

    if (requiredFailures != 0) {
        std::cout << "\n  Не разрешившиеся сигнатуры нужно уточнить в отладчике.\n"
                     "  «НЕОДНОЗНАЧНО» означает, что сигнатуру надо удлинить,\n"
                     "  «НЕ НАЙДЕНА» — что окрестности кода изменились.\n";

        if (clientLoaded) {
            std::cout << "\n  Но сперва учтите: в этом процессе уже сидит oxymp-client.dll,\n"
                         "  а он правит код игры на месте. Сигнатура места, которое он\n"
                         "  переписал, не найдётся — и это не поломка. Так ведёт себя,\n"
                         "  например, landing_page_branch: клиент меняет там условный\n"
                         "  переход на безусловный ещё до первого кадра.\n"
                         "  Чтобы увидеть игру нетронутой, запустите её без лаунчера.\n";
        }

        return 1;
    }

    if (okCount != results.size()) {
        std::cout << "\n  Не разрешились только разведочные сигнатуры. Клиент с этим\n"
                     "  запускается как обычно: без них нет лишь той возможности,\n"
                     "  ради которой они заведены.\n";
    }

    return buildMatches ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    return run(argc, argv);
}
