#include "config_file.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <fstream>
#include <locale>
#include <sstream>

namespace oxymp::server::config_file {
namespace {

/// Убирает пробелы по краям.
[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto space = [](unsigned char symbol) { return std::isspace(symbol) != 0; };

    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }

    return text;
}

/// Снимает кавычки, если значение в них взято.
///
/// Нужны они одному: значению с решёткой внутри. Во всех остальных случаях
/// кавычки лишние, и требовать их от человека было бы придиркой.
[[nodiscard]] std::string_view unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }

    return value;
}

/// Отрезает примечание.
///
/// Решётка внутри кавычек примечания не начинает: цвет вида "#FF0000" — это
/// значение, а не начало пояснения.
[[nodiscard]] std::string_view stripComment(std::string_view line) {
    bool quoted = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '"') {
            quoted = !quoted;
        } else if (line[index] == '#' && !quoted) {
            return line.substr(0, index);
        }
    }

    return line;
}

/// Приводит имя ключа к нижнему регистру.
///
/// Чтобы «MaxPlayers» и «maxplayers» означали одно и то же: человек, пишущий
/// файл руками, не обязан помнить, как мы решили его назвать.
[[nodiscard]] std::string lowered(std::string_view text) {
    std::string result{text};

    std::ranges::transform(result, result.begin(), [](unsigned char symbol) {
        return static_cast<char>(std::tolower(symbol));
    });

    return result;
}

/// Разбирает число целиком: «30abc» считается ошибкой, а не тридцатью.
template<typename T>
[[nodiscard]] bool number(std::string_view text, T& value) {
    text = trim(text);

    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);

    return result.ec == std::errc{} && result.ptr == end;
}

/// Разбирает дробное число.
///
/// Отдельно от целого, потому что from_chars для дробных в некоторых
/// стандартных библиотеках отсутствует, а поведение stof зависит от локали:
/// в русской локали точка перестаёт быть разделителем, и «-1037.7» становится
/// минус тысячей тридцатью семью.
[[nodiscard]] bool decimal(std::string_view text, float& value) {
    std::istringstream stream{std::string{trim(text)}};
    stream.imbue(std::locale::classic());

    float parsed = 0.0F;
    stream >> parsed;

    if (stream.fail() || !stream.eof()) {
        return false;
    }

    value = parsed;
    return true;
}

/// Разбирает точку в мире: три числа через запятую.
[[nodiscard]] bool point(std::string_view value, shared::Vec3& position) {
    const std::vector<std::string> parts = split(value);
    if (parts.size() != 3) {
        return false;
    }

    return decimal(parts[0], position.x) && decimal(parts[1], position.y) &&
           decimal(parts[2], position.z);
}

/// Разбирает время суток вида «12:30».
[[nodiscard]] bool timeOfDay(std::string_view value, std::uint8_t& hour, std::uint8_t& minute) {
    const std::size_t colon = value.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }

    std::uint16_t parsedHour = 0;
    std::uint16_t parsedMinute = 0;

    if (!number(value.substr(0, colon), parsedHour) ||
        !number(value.substr(colon + 1), parsedMinute)) {
        return false;
    }

    if (parsedHour > 23 || parsedMinute > 59) {
        return false;
    }

    hour = static_cast<std::uint8_t>(parsedHour);
    minute = static_cast<std::uint8_t>(parsedMinute);

    return true;
}

} // namespace

std::vector<std::string> split(std::string_view value) {
    std::vector<std::string> parts;

    while (!value.empty()) {
        const std::size_t comma = value.find(',');

        if (comma == std::string_view::npos) {
            parts.emplace_back(trim(value));
            break;
        }

        parts.emplace_back(trim(value.substr(0, comma)));
        value.remove_prefix(comma + 1);
    }

    return parts;
}

bool parse(std::string_view text, Entries& entries, std::string& error) {
    std::size_t lineNumber = 0;

    // Метка порядка байтов снимается с начала файла.
    //
    // Её ставит Блокнот, сохраняя в UTF-8, — то есть она будет у половины
    // отредактированных вручную файлов. Оставленная, она прилипает к имени
    // первой настройки, и та перестаёт узнаваться: сервер жалуется на
    // непонятный ключ «resources», показывая его в журнале ровно так же, как
    // написано в файле. Искать эту разницу глазами можно долго.
    constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

    if (text.starts_with(kByteOrderMark)) {
        text.remove_prefix(kByteOrderMark.size());
    }

    while (!text.empty()) {
        const std::size_t breakAt = text.find('\n');
        const std::string_view raw = breakAt == std::string_view::npos ? text
                                                                       : text.substr(0, breakAt);

        text = breakAt == std::string_view::npos ? std::string_view{} : text.substr(breakAt + 1);
        ++lineNumber;

        const std::string_view line = trim(stripComment(raw));
        if (line.empty()) {
            continue;
        }

        // Разделителем служит и двоеточие, и знак равенства: человеку привычно
        // и то и другое, а спорить об этом с ним незачем.
        const std::size_t separator = line.find_first_of(":=");
        if (separator == std::string_view::npos) {
            error = std::format("строка {}: нет разделителя между ключом и значением", lineNumber);
            return false;
        }

        const std::string key = lowered(trim(line.substr(0, separator)));
        if (key.empty()) {
            error = std::format("строка {}: пустое имя настройки", lineNumber);
            return false;
        }

        entries[key] = std::string{unquote(trim(line.substr(separator + 1)))};
    }

    return true;
}

std::vector<std::string> apply(const Entries& entries, Config& config) {
    // Жалобы — готовыми предложениями, а не именами ключей. Причина в том, что
    // поводов пожаловаться стало три и они разные: опечатка в имени, непонятное
    // значение и ключ, сменивший смысл. Знает, который случился, только это
    // место; вызывающему остаётся показать сказанное человеку.
    std::vector<std::string> complaints;

    for (const auto& [key, value] : entries) {
        bool understood = true;

        if (key == "name") {
            config.name = value;
        } else if (key == "password") {
            config.password = value;
        } else if (key == "port") {
            understood = number(value, config.port);
        } else if (key == "maxplayers") {
            understood = number(value, config.maxPlayers);
        } else if (key == "resources") {
            // Ключ сменил смысл, и старое значение узнаётся по косой черте.
            //
            // Раньше здесь стоял путь к игровым файлам — единственному, что
            // сервер тогда раздавал. Теперь это перечень скриптовых ресурсов, как
            // в alt:V, а путь переехал в gamefiles. Промолчать нельзя: сервер,
            // прочитавший путь как имя ресурса, пожаловался бы на пропавший
            // каталог «resources/dlcpacks» и перестал бы раздавать файлы — и
            // связать одно с другим хозяину было бы не по чему.
            if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos) {
                config.gameFilesDirectory = value;
                complaints.emplace_back(
                    "\"resources\" теперь перечень скриптовых ресурсов; путь принят как "
                    "\"gamefiles\" — перенесите его туда");
                continue;
            }

            config.resources.clear();

            for (std::string& name : split(value)) {
                if (!name.empty()) {
                    config.resources.push_back(std::move(name));
                }
            }
        } else if (key == "gamefiles") {
            config.gameFilesDirectory = value;
        } else if (key == "resourcedirectory") {
            config.resourceDirectory = value;
        } else if (key == "pausemenu") {
            config.pausemenuPath = value;
        } else if (key == "spawn") {
            understood = point(value, config.spawnPosition);
        } else if (key == "spawnheading") {
            understood = decimal(value, config.spawnHeading);
        } else if (key == "streamdistance") {
            understood = decimal(value, config.streamDistance);
        } else if (key == "maxvehicles") {
            understood = number(value, config.maxVehicles);
        } else if (key == "maxobjects") {
            understood = number(value, config.maxObjects);
        } else if (key == "weather") {
            config.weather = value;
        } else if (key == "time") {
            understood = timeOfDay(value, config.startingHour, config.startingMinute);
        } else if (key == "money") {
            understood = number(value, config.startingMoney);
        } else if (key == "admins") {
            config.admins.clear();

            for (const std::string& part : split(value)) {
                shared::PlayerId id = shared::kInvalidPlayerId;

                if (!number(part, id)) {
                    understood = false;
                    break;
                }

                config.admins.push_back(id);
            }
        } else {
            complaints.push_back(std::format("настройка \"{}\" не понята и пропущена", key));
            continue;
        }

        // Ключ знакомый, а значение — нет. Это не то же самое, что опечатка в
        // имени, и сказать об этом нужно иначе: хозяин должен искать ошибку там,
        // где она есть.
        if (!understood) {
            complaints.push_back(
                std::format("у настройки \"{}\" непонятное значение \"{}\"", key, value));
        }
    }

    return complaints;
}

bool read(const std::filesystem::path& path, Entries& entries, std::string& error) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        error = std::format("файл \"{}\" не открывается", path.string());
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    return parse(buffer.str(), entries, error);
}

bool load(const std::filesystem::path& path, Config& config, std::string& error) {
    if (!std::filesystem::exists(path)) {
        // Файла нет — это не беда: сервер работает на значениях по умолчанию,
        // ровно как работал до появления конфигурации.
        return true;
    }

    Entries entries;
    if (!read(path, entries, error)) {
        return false;
    }

    // Вызов с указанием пространства имён, и это не украшательство. Аргументы
    // здесь — стандартные типы, а значит поиск имени заглядывает и в namespace
    // std; там живёт std::apply, который берёт кортеж. Без уточнения компилятор
    // выбирает его и разбирает Config как кортеж — с сообщением об ошибке,
    // которое ведёт в заголовки стандартной библиотеки и никак не сюда.
    for (const std::string& complaint : config_file::apply(entries, config)) {
        spdlog::warn("{}", complaint);
    }

    return true;
}

} // namespace oxymp::server::config_file
