#include <oxymp/config/settings.hpp>

#include <toml++/toml.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include <windows.h>

namespace oxymp::config {
namespace {

/// Язык, на котором меню заговорит при первом запуске.
///
/// У страницы их два — `en` и `ru`, — и выбирать между ними по языку Windows
/// честнее, чем всегда брать английский: человек, у которого система по-русски,
/// с большой вероятностью ждёт русского и здесь. У нашего образца alt:V в
/// настройках стоит `ru`, и выбирал его не человек.
///
/// Это только умолчание: выбранное игроком на странице ложится в файл и с этого
/// мгновения решает всё.
[[nodiscard]] const char* defaultLanguage() {
    // Младшие десять бит — сам язык, старшие — его местная разновидность.
    // Разновидность здесь не важна: русский он и в Казахстане русский.
    constexpr WORD kRussian = 0x19;

    return PRIMARYLANGID(::GetUserDefaultUILanguage()) == kRussian ? "ru" : "en";
}

/// Умолчания — они же перечень того, какие настройки вообще бывают.
///
/// Имена и значения взяты у alt:V из `altv.toml` до последнего поля. Часть из них
/// нам сейчас не нужна вовсе — `linuxCompatibility`, `enableOverwolfOverlay`, —
/// и всё же они здесь: файл настроек переносят с одного мультиплеера на другой
/// руками, и настройка, которую мы молча выбросили, потерялась бы при первом же
/// сохранении.
toml::table defaults() {
    return toml::table{
        {"audioFrameLimit", false},
        {"autoBackup", true},
        {"autoFindMic", false},
        {"branch", "release"},
        {"cachePath", ""},
        {"cefAlwaysFullCopy", false},
        {"cefUseHardwareAcceleration", true},
        {"consoleHeight", 0.4},
        {"consoleWidth", 0.45},
        {"crashOnFatalError", false},
        {"crashReporterEnabled", true},
        // Вторая настройка, которой у alt:V нет, и она здесь по той же
        // надобности, что и первая: у alt:V такого вопроса не возникает вовсе.
        //
        // Закрывать ли Rockstar Games Launcher, когда клиент оказался внутри
        // игры. Права на игру он выдаёт один раз, при её запуске, и дальше не
        // нужен ни ей, ни нам — а память и место в панели задач занимает.
        //
        // По умолчанию да. Выключатель оставлен не для красоты: закрытие — это
        // всё же снятие чужого процесса, и если у чьей-то копии игра без
        // лаунчера жить откажется, выключать это придётся тому, у кого игра уже
        // не запускается.
        {"closeRockstarLauncher", true},

        {"debug", false},
        {"disableForcedRawInput", false},
        {"disableRtl", false},
        {"discordRichPresence", true},
        {"displaySystemUpdateMessagesInLog", false},

        // Настройки, которых у нас пока никто не читает, и всё же они здесь.
        // Файл настроек переносят с одного мультиплеера на другой руками, и
        // настройка, которую мы молча выбросили, потерялась бы при первом же
        // сохранении. Значения — те же, что у alt:V по умолчанию.
        {"earlyAuthTestURL", ""},
        {"enableDiscordOverlay", false},
        {"enableGuildedOverlay", false},
        {"enableNvidiaShadowPlayOverlay", false},
        {"enableOverwolfOverlay", false},
        {"linuxCompatibility", false},

        {"expandedConsole", true},
        {"externalConsoleX", 0},
        {"externalConsoleY", 0},

        // Единственная настройка, которой у alt:V нет, и она здесь вынужденно.
        //
        // Язык игры — `ru-RU`, `en-US` и так далее, — который лаунчер называет
        // ей при запуске. Пусто означает «как решит лаунчер Rockstar», и это
        // умолчание: не выбрав ничего, игрок получает то же, что и без нас.
        //
        // Настройка нужна потому, что внутри сессии игра язык сменить не даёт:
        // меню паузы там сетевое, и выбранную строку оно возвращает обратно —
        // ровно как в обычной GTA Online. Единственное мгновение, когда игра
        // ещё слушает о языке, — её собственный запуск.
        {"gameLanguage", ""},

        {"gtaPlatform", "rgl"},
        {"gtapath", ""},
        {"heapSize", 1024},
        {"lang", defaultLanguage()},
        {"lastip", ""},
        {"launcherSkin", ""},
        {"launcherSkinsDisabled", toml::array{}},
        {"logTimeFormat", "%H:%M:%S"},
        {"maxDownloadSpeed", 0},
        {"name", "Player"},
        {"netgraph", false},
        {"permissionsSet", false},
        {"promotedOnTop", true},
        {"region", "global"},
        {"streamerMode", false},
        {"textureBudgetPatch", true},
        {"uiVolume", 100},
        {"useExternalConsole", false},
        {"useSharedTextures", true},
        {"voiceActivationEnabled", false},
        {"voiceActivationKey", 78},
        {"voiceAutoInputVolume", true},
        {"voiceEnabled", true},
        {"voiceInputDevice", ""},
        {"voiceInputNormalization", true},
        {"voiceInputSensitivity", 20},
        {"voiceInputVolume", 100},
        {"voiceNoiseSuppression", true},
        {"voiceVolume", 200},
    };
}

/// Настройки, которые в файле и на странице называются по-разному.
///
/// Расхождение не наше: страница взята у alt:V, файл — тоже, и у них самих эти
/// имена разные. Свести их к одному нельзя, не поправив либо страницу, либо
/// переносимость файла; выбран перевод в одном месте.
constexpr std::array<std::pair<const char*, const char*>, 5> kRenamed{{
    {"lang", "language"},
    {"netgraph", "netgraphEnabled"},
    {"maxDownloadSpeed", "downloadSpeedLimit"},
    {"voiceActivationEnabled", "voiceActivation"},
    {"voiceInputNormalization", "voiceNormalization"},
}};

[[nodiscard]] std::string_view pageName(std::string_view fileName) {
    for (const auto& [file, page] : kRenamed) {
        if (fileName == file) {
            return page;
        }
    }

    return fileName;
}

[[nodiscard]] std::string_view fileName(std::string_view pageName) {
    for (const auto& [file, page] : kRenamed) {
        if (pageName == page) {
            return file;
        }
    }

    return pageName;
}

/// Кладёт значение на место умолчания того же вида — или отказывает вслух.
///
/// Общая для чтения файла и для правки со страницы: обеим нужно одно и то же —
/// не дать чужому значению незаметно подменить вид настройки. Испорченный файл
/// объявлен не бедой («получаются те же умолчания»), а это обещание держится
/// только на настоящем отказе разбора; синтаксически верная строка чужого
/// вида — скажем, `crashReporterEnabled = "yes"` вместо булева — проходила бы
/// молча и подменяла собой умолчание строкой, которую flag() потом читает как
/// false что бы в ней ни было. Дробное и целое между собой не в счёт: JS не
/// различает `1` и `1.0`, а страница шлёт то, что есть.
bool assignTyped(toml::table& table, const std::string& name, const toml::node& existing,
                 const toml::node& incoming) {
    if (existing.is_floating_point() && incoming.is_integer()) {
        table.insert_or_assign(name, static_cast<double>(incoming.as_integer()->get()));
        return true;
    }

    if (existing.is_integer() && incoming.is_floating_point()) {
        table.insert_or_assign(name, static_cast<std::int64_t>(incoming.as_floating_point()->get()));
        return true;
    }

    if (existing.type() != incoming.type()) {
        return false;
    }

    table.insert_or_assign(name, incoming);
    return true;
}

} // namespace

struct Settings::State {
    toml::table table = defaults();
};

Settings::Settings() : state_{std::make_unique<State>()} {}
Settings::~Settings() = default;

Settings::Settings(Settings&&) noexcept = default;
Settings& Settings::operator=(Settings&&) noexcept = default;

Settings Settings::load(const std::filesystem::path& file) {
    Settings settings;

    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return settings;
    }

    toml::parse_result parsed = toml::parse_file(file.string());
    if (!parsed) {
        spdlog::warn("settings did not parse ({}): defaults are used",
                     std::string{parsed.error().description()});
        return settings;
    }

    // Поверх умолчаний, а не вместо них: в файле игрока может не быть настройки,
    // которая появилась у нас позже, и оставить её пустой значило бы получить
    // нулевую громкость там, где должна быть сотня.
    for (auto&& [key, value] : parsed.table()) {
        const std::string name{key.str()};

        const toml::node* const existing = settings.state_->table.get(name);
        if (existing == nullptr) {
            spdlog::debug("настройка {} неизвестна — пропущена", name);
            continue;
        }

        if (!assignTyped(settings.state_->table, name, *existing, value)) {
            spdlog::warn("настройка {} в файле не того вида — оставлено умолчание", name);
        }
    }

    return settings;
}

bool Settings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::ofstream out{file, std::ios::binary | std::ios::trunc};
    if (!out) {
        spdlog::warn("settings were not written: {}", file.string());
        return false;
    }

    out << state_->table << '\n';
    return out.good();
}

bool Settings::flag(std::string_view key) const {
    return state_->table[fileName(key)].value_or(false);
}

std::int64_t Settings::number(std::string_view key) const {
    return state_->table[fileName(key)].value_or<std::int64_t>(0);
}

double Settings::fraction(std::string_view key) const {
    return state_->table[fileName(key)].value_or(0.0);
}

std::string Settings::text(std::string_view key) const {
    return state_->table[fileName(key)].value_or<std::string>("");
}

std::vector<std::string> Settings::list(std::string_view key) const {
    std::vector<std::string> items;

    if (const toml::array* array = state_->table[fileName(key)].as_array(); array != nullptr) {
        for (const toml::node& item : *array) {
            if (const auto text = item.value<std::string>(); text.has_value()) {
                items.push_back(*text);
            }
        }
    }

    return items;
}

void Settings::set(std::string_view key, bool value) {
    state_->table.insert_or_assign(fileName(key), value);
}

void Settings::set(std::string_view key, std::int64_t value) {
    state_->table.insert_or_assign(fileName(key), value);
}

void Settings::set(std::string_view key, double value) {
    state_->table.insert_or_assign(fileName(key), value);
}

void Settings::set(std::string_view key, std::string_view value) {
    state_->table.insert_or_assign(fileName(key), std::string{value});
}

std::string Settings::toJson() const {
    // Имена переводятся в те, которых ждёт страница, поэтому таблица собирается
    // заново, а не отдаётся как есть.
    toml::table forPage;

    for (auto&& [key, value] : state_->table) {
        forPage.insert_or_assign(pageName(key.str()), value);
    }

    std::ostringstream out;
    out << toml::json_formatter{forPage};

    return out.str();
}

bool Settings::applyJson(std::string_view key, std::string_view value) {
    const std::string name{fileName(key)};

    const toml::node* existing = state_->table.get(name);
    if (existing == nullptr) {
        spdlog::debug("страница правит неизвестную настройку {} — отказано", key);
        return false;
    }

    // Запись значения в JSON почти всегда является записью значения в TOML:
    // `true`, `142`, `"Игрок"`, `["a","b"]` читаются обоими одинаково. Этим и
    // пользуемся вместо своего разбора JSON — свой разошёлся бы с настоящим на
    // первой же строке с необычным знаком.
    const std::string document = "value = " + std::string{value};

    toml::parse_result parsed = toml::parse(document);
    if (!parsed) {
        spdlog::debug("значение настройки {} не разобралось: {}", key, value);
        return false;
    }

    toml::node* incoming = parsed.table().get("value");
    if (incoming == nullptr) {
        return false;
    }

    if (!assignTyped(state_->table, name, *existing, *incoming)) {
        spdlog::debug("страница правит настройку {} значением не того вида: {}", key, value);
        return false;
    }

    return true;
}

} // namespace oxymp::config
