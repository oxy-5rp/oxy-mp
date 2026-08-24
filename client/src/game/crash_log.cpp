#include "crash_log.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Сколько падений записывать.
///
/// Обычно хватает одного: за первым, как правило, идёт лавина таких же из
/// разрушенного состояния, и толку от неё нет. Несколько — на случай, когда
/// первое исключение оказалось не тем.
constexpr int kMaxReports = 4;

std::atomic<int> g_reported{0};

/// Смертельно ли исключение.
///
/// В игре за секунду проходят десятки исключений, которые она ловит сама и
/// переживает: чужие языковые (0xE06D7363), отладочные, оповещения. Записывать
/// их значило бы утопить журнал в шуме, а нужное в нём потерять.
bool fatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case STATUS_FATAL_APP_EXIT:
        return true;
    default:
        return false;
    }
}

std::string_view describe(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "array bounds exceeded";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "noncontinuable exception";
    case STATUS_FATAL_APP_EXIT:
        return "the game called it fatal itself";
    default:
        return "unknown";
    }
}

/// Куда именно пришёлся адрес: имя модуля и смещение в нём.
///
/// Голый адрес почти бесполезен: игра грузится по разным адресам, и одно и то
/// же место в коде выглядит от запуска к запуску по-разному. Смещение же
/// сравнивается напрямую — и с прошлыми падениями, и с адресами из каталога
/// сигнатур.
constexpr const char* kUnknownModule = "outside known modules";

std::string locate(const void* address) {
    HMODULE module = nullptr;

    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(address), &module) == 0) {
        return kUnknownModule;
    }

    std::wstring path(MAX_PATH, L'\0');
    const DWORD written =
        ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(written);

    const auto offset = static_cast<std::uintptr_t>(static_cast<const std::uint8_t*>(address) -
                                                    reinterpret_cast<std::uint8_t*>(module));

    return std::format("{}+{:#x}", std::filesystem::path{path}.filename().string(), offset);
}

/// Откуда пришли к месту падения.
///
/// Настоящей раскрутки стека здесь нет, и она не нужна: символов у игры не
/// бывает, а назвать требуется лишь модуль и смещение в нём. Поэтому стек
/// просматривается сверху вниз, и всякое слово, попавшее внутрь загруженного
/// модуля, считается вероятным адресом возврата.
///
/// Способ грубый: среди названного окажутся и случайные совпадения. Зато он
/// отвечает на главный вопрос — чей код привёл к падению, наш или игры, — а
/// точнее без символов всё равно не выйдет. Без этого следа падение внутри игры
/// неотличимо от падения из-за игры: адрес один и тот же, а причина разная.
void reportStack(const CONTEXT& context) {
    /// Сколько строк показать: дальше начинается прошлое, к падению отношения
    /// не имеющее.
    constexpr std::size_t kDepth = 14;

    /// Сколько слов стека просмотреть.
    constexpr std::size_t kWords = 512;

    MEMORY_BASIC_INFORMATION region{};

    // Стек спрашивается у Windows, а не берётся с запасом: за его концом лежит
    // сторожевая страница, и чтение из неё уронило бы нас прямо в обработчике
    // чужого падения.
    if (::VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &region, sizeof(region)) == 0) {
        return;
    }

    const auto top = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;

    if (top <= context.Rsp) {
        return;
    }

    const auto* const stack = reinterpret_cast<const std::uintptr_t*>(context.Rsp);
    const std::size_t available = (top - context.Rsp) / sizeof(std::uintptr_t);

    std::size_t shown = 0;

    for (std::size_t i = 0; i < std::min(kWords, available) && shown < kDepth; ++i) {
        const std::uintptr_t value = stack[i];

        // Мелкие числа — это счётчики и длины, а не адреса.
        if (value < 0x10000) {
            continue;
        }

        const std::string where = locate(reinterpret_cast<const void*>(value));
        if (where == kUnknownModule) {
            continue;
        }

        spdlog::critical("  called from {} ({:#x})", where, value);
        ++shown;
    }
}

LONG CALLBACK onException(EXCEPTION_POINTERS* pointers) {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;

    if (!fatal(record.ExceptionCode) || g_reported.fetch_add(1) >= kMaxReports) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    spdlog::critical("the game crashed: {} ({:#010x}) at {:#x} - {}",
                     describe(record.ExceptionCode),
                     static_cast<std::uint32_t>(record.ExceptionCode),
                     reinterpret_cast<std::uintptr_t>(record.ExceptionAddress),
                     locate(record.ExceptionAddress));

    // У обращения по недоступному адресу есть подробности: читали или писали и
    // по какому адресу. Ради них исключение и разбирается: «писали в ноль» и
    // «читали из освобождённой памяти» — разные поломки с разными причинами.
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        constexpr std::array<const char*, 9> kOperations = {"read", "write", "", "", "",
                                                            "", "", "", "execute"};

        const auto operation = record.ExceptionInformation[0];

        spdlog::critical("  {} at {:#x}",
                         operation < kOperations.size() ? kOperations[operation] : "access",
                         record.ExceptionInformation[1]);
    }

    if (pointers->ContextRecord != nullptr) {
        const CONTEXT& context = *pointers->ContextRecord;

        spdlog::critical("  rip={:#x} rsp={:#x} rcx={:#x} rdx={:#x} r8={:#x} r9={:#x}", context.Rip,
                         context.Rsp, context.Rcx, context.Rdx, context.R8, context.R9);

        reportStack(context);
    }

    spdlog::default_logger()->flush();

    // Исключение передаётся дальше — тому, кому оно предназначалось. Наше дело
    // здесь записать, а не решать: игра ловит часть своих падений сама, и
    // перехватить их значило бы изменить её поведение ради журнала.
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

std::unique_ptr<CrashLog> CrashLog::install() {
    std::unique_ptr<CrashLog> log{new CrashLog};

    // Первой в очереди: обработчики игры вправе проглотить исключение, и тогда
    // до нас оно не дойдёт вовсе.
    log->handle_ = ::AddVectoredExceptionHandler(1, &onException);

    if (log->handle_ == nullptr) {
        spdlog::warn("the crash handler was not installed: a crash will cut the log silently");
        return nullptr;
    }

    return log;
}

CrashLog::~CrashLog() {
    if (handle_ != nullptr) {
        ::RemoveVectoredExceptionHandler(handle_);
    }
}

} // namespace oxymp::client::game
