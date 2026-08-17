#include "battleye_link.hpp"

#include "import_hook.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

namespace oxymp::rglpatch {
namespace {

/// Звено, которое подменяется, и то, чем оно подменяется.
constexpr const wchar_t* kBattlEyeExecutable = L"GTA5_BE.exe";
constexpr const wchar_t* kGameExecutable = L"GTA5.exe";

/// Ключ, которым игра запускается сразу в свободный режим сети, минуя сюжет.
///
/// Добавляется здесь, а не в игре: командную строку составляет лаунчер
/// Rockstar, и единственное место, где её ещё можно дополнить, — это подмена
/// звена. Изнутри процесса командную строку уже не переписать.
constexpr const wchar_t* kStraightIntoFreemode = L"-StraightIntoFreemode";

/// Доводы лаунчера Rockstar, которым до игры доходить не следует.
///
/// Остальные доводы не разбираются вовсе — см. replaceExecutable, — и этот
/// перечислен поимённо не из желания навести порядок, а потому что ломает своё.
///
/// `-scDiscordClientId` — номер приложения Rockstar в Discord. Его читает
/// `socialclub.dll`, загруженная в игру, и объявляет через `discord_partner_sdk.dll`,
/// что человек играет в GTA V в сюжетном режиме. Для нас это неправда: игрок в
/// сессии oxyMP, и о ней Discord рассказывает клиент — своим приложением и
/// своими словами. Два показа спорят между собой, и игрок видит то одно, то
/// другое.
///
/// Здесь же был `-rglLanguage`, и его отсюда убрали. Отбирали его затем, чтобы
/// починить язык в меню игры — тот самый, что «переключается и возвращается
/// обратно», — и это оказалось рассуждением, а не наблюдением: с отобранным
/// доводом язык возвращается ровно так же. Значит источник у беды другой, а
/// отбирать чужой довод без причины нельзя — с ним игра хотя бы начинает на том
/// языке, который выбран в лаунчере. Возвращать его в этот список, не показав
/// на живой игре, что он и вправду виноват, не нужно.
constexpr std::wstring_view kStrippedArguments[] = {
    L"-scDiscordClientId",
};

/// Перехват один на процесс, поэтому и состояние одно: обработчику неоткуда
/// узнать, кому он принадлежит.
std::unique_ptr<ImportHook> g_hook;
BattlEyeLink::Reporter g_report;
BattlEyeLink::OrderQuery g_order;

using CreateProcessFunction = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                            LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                            LPSTARTUPINFOW, LPPROCESS_INFORMATION);

/// Имя исполняемого файла в пути. Пусто, если пути нет.
std::wstring executableName(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    return std::filesystem::path{path}.filename().wstring();
}

/// Первый разделённый пробелами кусок командной строки — путь к программе.
///
/// Кавычки учитываются: путь к игре лежит в Program Files, и без них он
/// распался бы на два куска.
std::size_t executableTokenLength(const std::wstring& commandLine) {
    if (commandLine.empty()) {
        return 0;
    }

    if (commandLine.front() == L'"') {
        const std::size_t closing = commandLine.find(L'"', 1);
        return closing == std::wstring::npos ? commandLine.size() : closing + 1;
    }

    const std::size_t space = commandLine.find(L' ');
    return space == std::wstring::npos ? commandLine.size() : space;
}

/// Путь к программе, взятый из командной строки, без кавычек.
std::wstring executableFromCommandLine(const std::wstring& commandLine) {
    std::wstring token = commandLine.substr(0, executableTokenLength(commandLine));

    if (token.size() >= 2 && token.front() == L'"' && token.back() == L'"') {
        token = token.substr(1, token.size() - 2);
    }

    return token;
}

/// Конец слова, начавшегося в start. Кавычки учитываются: значение довода
/// может быть взято в них, и пробел внутри кавычек слова не заканчивает.
std::size_t wordEnd(const std::wstring& commandLine, std::size_t start) {
    bool quoted = false;

    for (std::size_t at = start; at < commandLine.size(); ++at) {
        if (commandLine[at] == L'"') {
            quoted = !quoted;
        } else if (commandLine[at] == L' ' && !quoted) {
            return at;
        }
    }

    return commandLine.size();
}

/// Начинается ли слово с этого довода. Сравнение без учёта регистра: лаунчер
/// пишет доводы как ему угодно, а игра их так и читает.
bool isArgument(std::wstring_view word, std::wstring_view name) {
    if (word.size() < name.size()) {
        return false;
    }

    if (::_wcsnicmp(word.data(), name.data(), name.size()) != 0) {
        return false;
    }

    // Довод целиком, а не начало другого: `-scDiscordClientId` не должен уносить
    // с собой выдуманный `-scDiscordClientIdOverride`.
    return word.size() == name.size() || word[name.size()] == L'=';
}

/// Убирает названные доводы из командной строки вместе с их значениями.
///
/// Путь к программе не разбирается: он стоит первым и словом не считается.
std::wstring withoutArguments(const std::wstring& commandLine) {
    const std::size_t executable = executableTokenLength(commandLine);

    std::wstring result = commandLine.substr(0, executable);

    std::size_t at = executable;

    while (at < commandLine.size()) {
        const std::size_t start = commandLine.find_first_not_of(L' ', at);
        if (start == std::wstring::npos) {
            break;
        }

        const std::size_t end = wordEnd(commandLine, start);
        const std::wstring_view word{commandLine.data() + start, end - start};

        bool stripped = false;
        for (const std::wstring_view name : kStrippedArguments) {
            if (isArgument(word, name)) {
                stripped = true;
                spdlog::info("довод лаунчера {} до игры не доходит",
                             std::filesystem::path{std::wstring{word}}.string());
                break;
            }
        }

        if (!stripped) {
            // Вместе с пробелами перед словом: так остаток строки сохраняется
            // ровно таким, каким его составил лаунчер.
            result.append(commandLine, at, end - at);
        }

        at = end;
    }

    return result;
}

/// Довод, которым лаунчер Rockstar называет игре язык.
constexpr std::wstring_view kLanguageArgument = L"-rglLanguage";

/// Ставит игре названный язык вместо того, что назначил лаунчер Rockstar.
///
/// Внутри сессии язык не сменить: меню паузы там сетевое, и строку языка оно
/// возвращает обратно — это не наша поломка, так ведёт себя и обычная GTA
/// Online. Значит выбирать язык нужно там, где игра его ещё слушает, — при
/// запуске.
///
/// Пустой язык означает «не трогать»: тогда игра берёт тот, что выбран в
/// лаунчере Rockstar, и всё остаётся как было.
std::wstring withLanguage(const std::wstring& commandLine, const std::wstring& language) {
    if (language.empty()) {
        return commandLine;
    }

    const std::wstring wanted = std::wstring{kLanguageArgument} + L'=' + language;

    const std::size_t executable = executableTokenLength(commandLine);

    std::wstring result = commandLine.substr(0, executable);

    bool replaced = false;
    std::size_t at = executable;

    while (at < commandLine.size()) {
        const std::size_t start = commandLine.find_first_not_of(L' ', at);
        if (start == std::wstring::npos) {
            break;
        }

        const std::size_t end = wordEnd(commandLine, start);
        const std::wstring_view word{commandLine.data() + start, end - start};

        if (isArgument(word, kLanguageArgument)) {
            // Вместе с пробелами перед доводом: строка остаётся такой же на вид.
            result.append(commandLine, at, start - at);
            result.append(wanted);

            replaced = true;
        } else {
            result.append(commandLine, at, end - at);
        }

        at = end;
    }

    // Довода могло не быть вовсе — тогда он появляется. Лаунчер Rockstar его
    // передаёт всегда, но полагаться на это незачем: своё слово о языке мы
    // говорим одинаково в обоих случаях.
    if (!replaced) {
        result += L' ';
        result += wanted;
    }

    spdlog::info("язык игры: {}", std::filesystem::path{language}.string());

    return result;
}

/// Заменяет путь к программе в командной строке, оставляя доводы как были.
///
/// Доводы, кроме названных в kStrippedArguments, не разбираются намеренно: их
/// составил лаунчер, и среди них есть и `-fromRGL`, без которого игра
/// закрывается с ERR_NO_LAUNCHER, и `@commandline.txt`. Знать, какие из них
/// чьи, нам незачем — важно лишь то, что запускается по этому пути.
std::wstring replaceExecutable(const std::wstring& commandLine, const std::wstring& executable) {
    const std::wstring quoted = L'"' + executable + L'"';

    if (commandLine.empty()) {
        return quoted;
    }

    return quoted + commandLine.substr(executableTokenLength(commandLine));
}

BOOL WINAPI createProcessDetour(LPCWSTR applicationName, LPWSTR commandLine,
                                LPSECURITY_ATTRIBUTES processAttributes,
                                LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles,
                                DWORD creationFlags, LPVOID environment, LPCWSTR currentDirectory,
                                LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) {
    const auto original = g_hook->original<CreateProcessFunction>();

    const std::wstring requestedApplication = applicationName == nullptr ? L"" : applicationName;
    const std::wstring requestedCommandLine = commandLine == nullptr ? L"" : commandLine;

    // Что именно запускают, лаунчер может сказать двумя способами: отдельным
    // путём или первым куском командной строки. Спрашиваем оба.
    const std::wstring requested = requestedApplication.empty()
                                       ? executableFromCommandLine(requestedCommandLine)
                                       : requestedApplication;

    if (::_wcsicmp(executableName(requested).c_str(), kBattlEyeExecutable) != 0) {
        return original(applicationName, commandLine, processAttributes, threadAttributes,
                        inheritHandles, creationFlags, environment, currentDirectory, startup,
                        information);
    }

    // Заказ спрашивается ровно здесь и снимается тем же движением. Не наш
    // запуск — значит игрок нажал «Играть» в окне лаунчера, и трогать его
    // нельзя: без BattlEye его не пустит GTA Online.
    const BattlEyeLink::Order order = g_order ? g_order() : BattlEyeLink::Order{};

    if (!order.substitute) {
        spdlog::info("запуск не наш — звено BattlEye остаётся на месте");

        return original(applicationName, commandLine, processAttributes, threadAttributes,
                        inheritHandles, creationFlags, environment, currentDirectory, startup,
                        information);
    }

    // Игра лежит там же, где промежуточное звено: подменяем имя файла, оставляя
    // каталог. Своего представления о том, где установлена игра, у нас здесь
    // нет и быть не должно — лаунчер знает это лучше.
    const std::wstring game =
        (std::filesystem::path{requested}.parent_path() / kGameExecutable).wstring();

    spdlog::info("лаунчер запускает {}", std::filesystem::path{requested}.string());

    std::error_code ec;
    if (!std::filesystem::exists(game, ec)) {
        spdlog::error("игра не найдена рядом со звеном BattlEye: {} — подмены не будет",
                      std::filesystem::path{game}.string());

        return original(applicationName, commandLine, processAttributes, threadAttributes,
                        inheritHandles, creationFlags, environment, currentDirectory, startup,
                        information);
    }

    std::wstring replacedCommandLine =
        withLanguage(withoutArguments(replaceExecutable(requestedCommandLine, game)), order.language);

    if (order.straightIntoFreemode) {
        replacedCommandLine += L' ';
        replacedCommandLine += kStraightIntoFreemode;

        spdlog::info("игра пойдёт сразу в сетевой свободный режим, минуя сюжет");
    }

    // CreateProcessW вправе изменить переданную ей командную строку, поэтому
    // отдаётся собственный изменяемый буфер, а не содержимое строки.
    std::vector<wchar_t> buffer(replacedCommandLine.begin(), replacedCommandLine.end());
    buffer.push_back(L'\0');

    const BOOL started =
        original(game.c_str(), buffer.data(), processAttributes, threadAttributes, inheritHandles,
                 creationFlags, environment, currentDirectory, startup, information);

    if (started == FALSE) {
        spdlog::error("подменённый запуск не удался: код ошибки Windows {}", ::GetLastError());
        return FALSE;
    }

    spdlog::info("вместо BattlEye запущена игра, процесс {}", information->dwProcessId);

    // Лаунчер получает описатели настоящей игры и дальше следит за ней сам:
    // ради этого подмена и затевалась. Нам остаётся сказать своим, за каким
    // процессом идти.
    if (g_report) {
        g_report(information->dwProcessId);
    }

    return TRUE;
}

} // namespace

std::unique_ptr<BattlEyeLink> BattlEyeLink::install(Reporter report, OrderQuery order,
                                                    std::string& error) {
    if (g_hook != nullptr) {
        error = "подмена уже стоит";
        return nullptr;
    }

    // Перехватывается таблица импорта самого лаунчера, а не чья-нибудь ещё:
    // игру запускает он.
    void* const launcher = ::GetModuleHandleW(nullptr);

    g_report = std::move(report);
    g_order = std::move(order);

    g_hook = ImportHook::install(launcher, "CreateProcessW",
                                 reinterpret_cast<void*>(&createProcessDetour), error);
    if (g_hook == nullptr) {
        g_report = nullptr;
        return nullptr;
    }

    return std::unique_ptr<BattlEyeLink>{new BattlEyeLink};
}

BattlEyeLink::~BattlEyeLink() {
    // Порядок обязателен: пока перехват стоит, обработчик может быть вызван в
    // любое мгновение и обязан оставаться рабочим. Поэтому сперва снятие, и
    // только после него — забывание того, кому докладывать.
    g_hook.reset();
    g_report = nullptr;
    g_order = nullptr;
}

} // namespace oxymp::rglpatch
