#include "crash_log.hpp"

#include <spdlog/spdlog.h>

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
        return "обращение по недоступному адресу";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "недопустимая инструкция";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "запрещённая инструкция";
    case EXCEPTION_IN_PAGE_ERROR:
        return "страница памяти недоступна";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "деление на ноль";
    case EXCEPTION_STACK_OVERFLOW:
        return "переполнение стека";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "выход за границы";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "исключение без продолжения";
    case STATUS_FATAL_APP_EXIT:
        return "игра сама объявила о непоправимом";
    default:
        return "неизвестное";
    }
}

/// Куда именно пришёлся адрес: имя модуля и смещение в нём.
///
/// Голый адрес почти бесполезен: игра грузится по разным адресам, и одно и то
/// же место в коде выглядит от запуска к запуску по-разному. Смещение же
/// сравнивается напрямую — и с прошлыми падениями, и с адресами из каталога
/// сигнатур.
std::string locate(const void* address) {
    HMODULE module = nullptr;

    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(address), &module) == 0) {
        return "вне известных модулей";
    }

    std::wstring path(MAX_PATH, L'\0');
    const DWORD written =
        ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(written);

    const auto offset = static_cast<std::uintptr_t>(static_cast<const std::uint8_t*>(address) -
                                                    reinterpret_cast<std::uint8_t*>(module));

    return std::format("{}+{:#x}", std::filesystem::path{path}.filename().string(), offset);
}

LONG CALLBACK onException(EXCEPTION_POINTERS* pointers) {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;

    if (!fatal(record.ExceptionCode) || g_reported.fetch_add(1) >= kMaxReports) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    spdlog::critical("игра упала: {} ({:#010x}) по адресу {:#x} — {}",
                     describe(record.ExceptionCode),
                     static_cast<std::uint32_t>(record.ExceptionCode),
                     reinterpret_cast<std::uintptr_t>(record.ExceptionAddress),
                     locate(record.ExceptionAddress));

    // У обращения по недоступному адресу есть подробности: читали или писали и
    // по какому адресу. Ради них исключение и разбирается: «писали в ноль» и
    // «читали из освобождённой памяти» — разные поломки с разными причинами.
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        constexpr std::array<const char*, 9> kOperations = {"чтение", "запись", "", "", "",
                                                            "", "", "", "исполнение"};

        const auto operation = record.ExceptionInformation[0];

        spdlog::critical("  {} по адресу {:#x}",
                         operation < kOperations.size() ? kOperations[operation] : "доступ",
                         record.ExceptionInformation[1]);
    }

    if (pointers->ContextRecord != nullptr) {
        const CONTEXT& context = *pointers->ContextRecord;

        spdlog::critical("  rip={:#x} rsp={:#x} rcx={:#x} rdx={:#x} r8={:#x} r9={:#x}", context.Rip,
                         context.Rsp, context.Rcx, context.Rdx, context.R8, context.R9);
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
        spdlog::warn("ловушка падений не поставлена — крах оборвёт журнал молча");
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
