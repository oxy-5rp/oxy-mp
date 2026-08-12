#include "battleye_link.hpp"

#include "import_hook.hpp"

#include <spdlog/spdlog.h>

#include <cwchar>
#include <filesystem>
#include <memory>
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

/// Заменяет путь к программе в командной строке, оставляя доводы как были.
///
/// Доводы не разбираются и не отсеиваются намеренно: их составил лаунчер, и
/// среди них есть и `-fromRGL`, без которого игра закрывается с ERR_NO_LAUNCHER,
/// и `@commandline.txt`, и язык. Знать, какие из них чьи, нам незачем — важно
/// лишь то, что запускается по этому пути.
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

    std::wstring replacedCommandLine = replaceExecutable(requestedCommandLine, game);

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
