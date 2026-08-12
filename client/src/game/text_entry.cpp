#include "text_entry.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <spdlog/spdlog.h>

#include <cstdint>

namespace oxymp::client::game {
namespace {

/// Подмена одна на процесс: обработчик окна — простая функция без состояния, и
/// связать её с объектом иначе нельзя.
TextEntry* g_entry = nullptr;

/// Коды управляющих символов, приходящих через WM_CHAR.
constexpr wchar_t kBackspace = 0x08;
constexpr wchar_t kEnter = 0x0D;
constexpr wchar_t kEscape = 0x1B;

/// Ниже этого кода лежат управляющие символы, которым в строке не место.
constexpr wchar_t kFirstPrintable = 0x20;

/// Какая латинская буква или цифра лежит на этой клавише.
///
/// Ноль — значит на клавише нет ничего, что годилось бы в название модели.
///
/// Коды клавиш описывают место на клавиатуре, а не то, что на ней нарисовано:
/// VK_A — это клавиша слева от S, чем бы она ни была подписана в текущей
/// раскладке. Отсюда и всё свойство: набор идёт латиницей при любой раскладке,
/// и переключать её не нужно — а в GTA и невозможно.
char latinFromKey(WPARAM key) {
    if (key >= 'A' && key <= 'Z') {
        // Строчные: названия моделей в игре записаны строчными, и приводить
        // регистр потом — лишний шаг, на котором можно ошибиться.
        return static_cast<char>(key - 'A' + 'a');
    }

    if (key >= '0' && key <= '9') {
        return static_cast<char>(key);
    }

    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        return static_cast<char>(key - VK_NUMPAD0 + '0');
    }

    switch (key) {
    case VK_SPACE:
        return ' ';

    // Подчёркивание — главный разделитель в названиях моделей игры:
    // mp_m_freemode_01, s_m_y_cop_01, a_c_chop. Лежит оно там же, где дефис, и
    // различает их Shift — ровно как в любой раскладке.
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        return (::GetKeyState(VK_SHIFT) & 0x8000) != 0 ? '_' : '-';

    default:
        return 0;
    }
}

bool isHighSurrogate(wchar_t symbol) {
    return symbol >= 0xD800 && symbol <= 0xDBFF;
}

bool isLowSurrogate(wchar_t symbol) {
    return symbol >= 0xDC00 && symbol <= 0xDFFF;
}

/// Отрезает последний символ строки UTF-8 целиком.
///
/// Не последний байт: буква кириллицы занимает два байта, и отрезанная по байту
/// строка перестаёт быть текстом.
void eraseLastCharacter(std::string& text) {
    while (!text.empty()) {
        const auto byte = static_cast<unsigned char>(text.back());
        text.pop_back();

        // Продолжающие байты последовательности начинаются с 10xxxxxx. Всё
        // остальное — начало символа, и на нём останавливаемся.
        if ((byte & 0xC0U) != 0x80U) {
            return;
        }
    }
}

} // namespace

std::unique_ptr<TextEntry> TextEntry::install(HWND window, std::string& error) {
    if (window == nullptr) {
        error = "окно игры не найдено";
        return nullptr;
    }

    if (g_entry != nullptr) {
        error = "перехват клавиатуры уже поставлен";
        return nullptr;
    }

    std::unique_ptr<TextEntry> entry{new TextEntry};
    entry->window_ = window;

    // Порядок обязателен: обработчик вправе прийти сразу же, поэтому объект
    // должен быть виден до подмены.
    g_entry = entry.get();

    entry->previous_ = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&TextEntry::proc)));

    if (entry->previous_ == nullptr) {
        g_entry = nullptr;
        error = "не удалось подменить обработчик окна игры";
        return nullptr;
    }

    spdlog::debug("перехват клавиатуры поставлен на окно {:#x}",
                 reinterpret_cast<std::uintptr_t>(window));

    return entry;
}

TextEntry::~TextEntry() {
    // Сперва отвязываем себя, и только потом возвращаем обработчик: между этими
    // двумя действиями сообщение может прийти, и лучше пусть оно пройдёт мимо
    // нас, чем в разрушаемый объект.
    g_entry = nullptr;

    if (window_ != nullptr && previous_ != nullptr && ::IsWindow(window_) != FALSE) {
        ::SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_));
    }
}

LRESULT CALLBACK TextEntry::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    TextEntry* const entry = g_entry;

    if (entry != nullptr && entry->handle(message, wparam)) {
        return 0;
    }

    if (entry != nullptr && entry->previous_ != nullptr) {
        return ::CallWindowProcW(entry->previous_, window, message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

bool TextEntry::handle(UINT message, WPARAM wparam) {
    const std::lock_guard guard{mutex_};

    if (!active_) {
        return false;
    }

    switch (message) {
    case WM_CHAR:
        // В латинском режиме символы приходят из кодов клавиш, а не отсюда:
        // здесь они уже испорчены раскладкой. Съедаем, чтобы их не увидела игра.
        if (mode_ == Mode::Latin) {
            return true;
        }

        switch (const auto symbol = static_cast<wchar_t>(wparam)) {
        case kBackspace:
            eraseLastCharacter(text_);
            return true;

        case kEnter:
            active_ = false;
            outcome_ = Outcome::Submitted;
            return true;

        case kEscape:
            active_ = false;
            text_.clear();
            outcome_ = Outcome::Cancelled;
            return true;

        default:
            if (symbol >= kFirstPrintable) {
                append(symbol);
            }
            return true;
        }

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_DEADCHAR:
        // Съедаются, чтобы игра не увидела набранного вторым путём. Escape при
        // этом обрабатывается здесь, а не в WM_CHAR: до символа он доходит не
        // всегда, а меню паузы по нему открывается всегда.
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
            active_ = false;
            text_.clear();
            outcome_ = Outcome::Cancelled;
            return true;
        }

        if (message == WM_KEYDOWN && mode_ == Mode::Latin) {
            switch (wparam) {
            case VK_RETURN:
                active_ = false;
                outcome_ = Outcome::Submitted;
                return true;

            case VK_BACK:
                eraseLastCharacter(text_);
                return true;

            default:
                if (const char symbol = latinFromKey(wparam); symbol != 0) {
                    append(static_cast<wchar_t>(symbol));
                }
                return true;
            }
        }

        return true;

    default:
        return false;
    }
}

void TextEntry::append(wchar_t symbol) {
    if (isHighSurrogate(symbol)) {
        pendingSurrogate_ = symbol;
        return;
    }

    char32_t code = symbol;

    if (isLowSurrogate(symbol)) {
        if (pendingSurrogate_ == 0) {
            return;
        }

        code = 0x10000U + ((static_cast<char32_t>(pendingSurrogate_) - 0xD800U) << 10U) +
               (static_cast<char32_t>(symbol) - 0xDC00U);
        pendingSurrogate_ = 0;
    }

    // Длина ограничена той же величиной, что и в протоколе: набранное сверх неё
    // всё равно обрезал бы сервер, и честнее не дать набрать вовсе.
    if (text_.size() + 4 > shared::kMaxChatLength) {
        return;
    }

    if (code < 0x80U) {
        text_ += static_cast<char>(code);
    } else if (code < 0x800U) {
        text_ += static_cast<char>(0xC0U | (code >> 6U));
        text_ += static_cast<char>(0x80U | (code & 0x3FU));
    } else if (code < 0x10000U) {
        text_ += static_cast<char>(0xE0U | (code >> 12U));
        text_ += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        text_ += static_cast<char>(0x80U | (code & 0x3FU));
    } else {
        text_ += static_cast<char>(0xF0U | (code >> 18U));
        text_ += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
        text_ += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        text_ += static_cast<char>(0x80U | (code & 0x3FU));
    }
}

void TextEntry::begin(Mode mode) {
    const std::lock_guard guard{mutex_};

    active_ = true;
    mode_ = mode;
    text_.clear();
    outcome_ = Outcome::Typing;
    pendingSurrogate_ = 0;
}

void TextEntry::cancel() {
    const std::lock_guard guard{mutex_};

    active_ = false;
    text_.clear();
    outcome_ = Outcome::Cancelled;
}

bool TextEntry::active() const {
    const std::lock_guard guard{mutex_};
    return active_;
}

std::string TextEntry::text() const {
    const std::lock_guard guard{mutex_};
    return text_;
}

TextEntry::Outcome TextEntry::takeOutcome(std::string& text) {
    const std::lock_guard guard{mutex_};

    const Outcome outcome = outcome_;
    outcome_ = Outcome::Typing;

    if (outcome == Outcome::Submitted) {
        text = std::move(text_);
        text_.clear();
    }

    return outcome;
}

} // namespace oxymp::client::game
