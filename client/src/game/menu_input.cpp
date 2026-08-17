#include "menu_input.hpp"

#include "keyboard_layout.hpp"

#include <oxymp/cefui/browser.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>

namespace oxymp::client::game {
namespace {

/// Единственный перехват на процесс.
///
/// Оконный обработчик — свободная функция без своего состояния: Windows зовёт её
/// по указателю и ничего своего с ней не передаёт. Окно игры одно, и перехват на
/// нём тоже один.
MenuInput* g_input = nullptr;

/// Достаёт из сообщения точку, в которой оно случилось.
///
/// Windows кладёт её в младшее и старшее слово одного числа, и знак при этом
/// теряется: указатель, уведённый выше окна, приходит как очень большое
/// положительное. Приведение к short возвращает знак на место.
[[nodiscard]] int mouseX(LPARAM lparam) {
    return static_cast<short>(LOWORD(lparam));
}

[[nodiscard]] int mouseY(LPARAM lparam) {
    return static_cast<short>(HIWORD(lparam));
}

[[nodiscard]] bool held(int key) {
    return (::GetKeyState(key) & 0x8000) != 0;
}

/// Насколько глубоко разрешено поднимать счётчик показа указателя.
///
/// Предел нужен не ради красоты: ShowCursor(TRUE) не поднимет счётчика выше нуля,
/// если мыши в системе нет вовсе, — и цикл без предела стал бы вечным.
constexpr int kCursorRaiseLimit = 64;

/// Показывает или прячет указатель мыши.
///
/// ShowCursor — не переключатель, а счётчик, и это стоило целой правки. Игра
/// прячет указатель сама, и не однажды: к мгновению, когда открывается меню,
/// счётчик уже глубоко в минусе. Одного ShowCursor(TRUE) не хватало —
/// указателя не было видно, хотя страница мышь получала и на неё отзывалась.
///
/// Поэтому поднимаем, пока счётчик не станет неотрицательным, и опускаем ровно
/// на столько же, на сколько подняли.
void showCursor(bool visible, int& raised) {
    if (visible) {
        while (raised < kCursorRaiseLimit && ::ShowCursor(TRUE) < 0) {
            ++raised;
        }
        return;
    }

    while (raised > 0) {
        ::ShowCursor(FALSE);
        --raised;
    }
}

/// Клавиши, которые нельзя отбирать у Windows.
///
/// Это управляющие клавиши: Shift, Ctrl, Alt и Windows. Отобрав их, мы отобрали
/// бы и сочетания, которые Windows разбирает сама, — прежде всего смену
/// раскладки по Alt+Shift. Игре они при этом достаются, но игре от них вреда
/// нет: сами по себе они ничего не делают.
[[nodiscard]] bool isModifier(unsigned key) {
    switch (key) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_CAPITAL:
        return true;
    default:
        return false;
    }
}

/// Раскладка того окна, которое сейчас впереди.
///
/// Раскладка в Windows своя у каждого потока, а перехват работает в нашем — и
/// спроси мы раскладку у себя, получили бы ту, с которой запустился клиент, а не
/// ту, которую видит игрок.
[[nodiscard]] HKL foregroundLayout() {
    const HWND window = ::GetForegroundWindow();
    if (window == nullptr) {
        return ::GetKeyboardLayout(0);
    }

    return ::GetKeyboardLayout(::GetWindowThreadProcessId(window, nullptr));
}

/// Просят ли этой клавишей сменить язык ввода.
///
/// Три сочетания, потому что привычных сочетаний три: Alt+Shift и Ctrl+Shift —
/// давние, Win+Пробел — нынешнее умолчание Windows. Какое из них у игрока,
/// заранее не известно, а спрашивать негде.
///
/// Замечать их приходится самим: игра просит Windows не разбирать системные
/// сочетания — эту просьбу у неё отбирают (см. raw_input.hpp), — но и без неё
/// служба текстового ввода языка игре не меняет. Проверено на живой игре:
/// сочетание нажато, язык тот же.
[[nodiscard]] bool isLayoutSwitch(unsigned key) {
    const auto down = [](int code) { return (::GetAsyncKeyState(code) & 0x8000) != 0; };

    if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) {
        return down(VK_MENU) || down(VK_CONTROL);
    }

    if (key == VK_SPACE) {
        return down(VK_LWIN) || down(VK_RWIN);
    }

    return false;
}

} // namespace

std::unique_ptr<MenuInput> MenuInput::install(HWND window, cefui::Browser& browser,
                                              Actions actions, std::string& error) {
    if (window == nullptr) {
        error = "окна игры нет: ввод для меню не перехватить";
        return nullptr;
    }

    if (g_input != nullptr) {
        error = "перехват ввода для меню уже стоит";
        return nullptr;
    }

    std::unique_ptr<MenuInput> input{new MenuInput};

    input->window_ = window;
    input->browser_ = &browser;
    input->actions_ = std::move(actions);

    // Указатель ставится раньше подмены: подменённый обработчик вправе получить
    // сообщение в тот же миг, и застать он должен уже готовое.
    g_input = input.get();

    input->previous_ = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuInput::proc)));

    if (input->previous_ == nullptr) {
        g_input = nullptr;

        error = "не удалось подменить обработчик окна, ошибка " + std::to_string(::GetLastError());
        return nullptr;
    }

    // Модуль, в котором лежат обработчики перехвата, — наш собственный, а не
    // исполняемый файл игры. Windows требует именно его: обработчик должен
    // оставаться на месте, пока перехват стоит.
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&MenuInput::keyboardProc), &self);

    // Смена раскладки перехватывается на очереди сообщений окна.
    //
    // Windows просит окно сменить раскладку сообщением, и исполняет её тот, кто
    // передаст просьбу обычному обработчику. Игра до него просьбу не доводит —
    // оттого раскладка «переключается и возвращается обратно». Через подменённый
    // обработчик окна её не достать: игра разбирает очередь сама и до окна это
    // сообщение не доводит вовсе.
    //
    // Перехват очереди видит сообщение в тот миг, когда игра его забирает, — то
    // есть раньше, чем она успевает его потерять.
    input->messages_ = ::SetWindowsHookExW(WH_GETMESSAGE, &MenuInput::messageProc, self,
                                           ::GetWindowThreadProcessId(window, nullptr));

    if (input->messages_ == nullptr) {
        spdlog::error("перехват очереди сообщений не поставлен, ошибка {}: раскладка в игре "
                      "переключаться не будет",
                      ::GetLastError());
    }

    // Клавиатура перехватывается отдельно от мыши, и это не прихоть.
    //
    // Мышь до подменённого обработчика доходит, а клавиатура — нет: игра читает
    // её мимо очереди сообщений, через DirectInput, и WM_KEYDOWN до окна просто
    // не добирается. Проверено на живой игре: щелчки работали, страницы
    // переключались, поле принимало внимание — и не принимало ни буквы.
    //
    // Низкоуровневый перехват стоит раньше всех и видит нажатие до того, как его
    // увидит кто-либо ещё, включая DirectInput. Им же нажатие и отбирается у
    // игры, пока открыто меню: набранное имя игрока не должно заодно вести
    // машину.
    input->keyboard_ = ::SetWindowsHookExW(WH_KEYBOARD_LL, &MenuInput::keyboardProc, self, 0);

    if (input->keyboard_ == nullptr) {
        // Не смертельно: без него меню останется без клавиатуры, но мышь и
        // страница работают. Отказываться от всего перехвата из-за этого нельзя.
        spdlog::error("клавиатура для меню не перехвачена, ошибка {}", ::GetLastError());
    }

    spdlog::debug("ввод для меню перехвачен");
    return input;
}

MenuInput::~MenuInput() {
    if (keyboard_ != nullptr) {
        ::UnhookWindowsHookEx(keyboard_);
        keyboard_ = nullptr;
    }

    if (messages_ != nullptr) {
        ::UnhookWindowsHookEx(messages_);
        messages_ = nullptr;
    }

    if (window_ != nullptr && previous_ != nullptr) {
        ::SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_));
    }

    showCursor(false, raised_);

    g_input = nullptr;
}

LRESULT CALLBACK MenuInput::messageProc(int code, WPARAM wparam, LPARAM lparam) {
    MenuInput* const input = g_input;

    if (code != HC_ACTION || input == nullptr || wparam != PM_REMOVE) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    auto* const message = reinterpret_cast<MSG*>(lparam);

    // Просьба переключить язык исполняется здесь: из трёх перехватов этот
    // единственный зовётся потоком игры, а язык ввода в Windows свой у каждого
    // потока.
    if (input->switchWanted_.exchange(false, std::memory_order_relaxed)) {
        switchInputLanguage();
    }

    // Просьбу сменить раскладку записываем всегда: по ней видно, разбирает ли
    // Windows сочетание вообще. Не разбирает — беда снаружи игры, а не в ней.
    if (message->message == WM_INPUTLANGCHANGEREQUEST && !input->sawLanguageRequest_) {
        input->sawLanguageRequest_ = true;

        spdlog::info("Windows просит сменить раскладку на {:#x}",
                     static_cast<std::uintptr_t>(message->lParam));
    }

    // Клавиши здесь — запасной путь, и он нужен: низкоуровневый перехват в игре
    // может не сработать вовсе (окно игры выше по правам, а такому окну Windows
    // не отдаёт чужих перехватов). Очередь же игра разбирает сама, и в этот миг
    // мы видим всё, что в ней лежит, — даже то, что игра потом выбросит.
    //
    // Работает только один из двух путей: если нажатия доходят низкоуровневым
    // перехватом, здесь они не разбираются — страница получила бы каждую букву
    // дважды.
    if (!input->sawKey_ && input->handleQueuedKey(*message)) {
        message->message = WM_NULL;
        message->wParam = 0;
        message->lParam = 0;

        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    if (message->message == WM_INPUTLANGCHANGEREQUEST) {
        // Просьбу исполняем сами, обычным обработчиком Windows, — и на этом её
        // жизнь заканчивается: игре она не достанется. Она бы её потеряла.
        ::DefWindowProcW(message->hwnd, message->message, message->wParam, message->lParam);

        message->message = WM_NULL;
        message->wParam = 0;
        message->lParam = 0;
    }

    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK MenuInput::keyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    MenuInput* const input = g_input;

    if (code != HC_ACTION || input == nullptr) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    const auto* const key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;

    // Первая запись — до всякого разбора: по ней видно, что перехват вообще
    // зовут. Без неё «клавиатура не работает» одинаково означает и то, что
    // Windows нас не зовёт, и то, что мы сами отбросили нажатие.
    if (!input->calledBack_) {
        input->calledBack_ = true;

        DWORD process = 0;
        ::GetWindowThreadProcessId(::GetForegroundWindow(), &process);

        spdlog::info("низкоуровневый перехват клавиатуры зовётся; впереди процесс {}, наш {}",
                     process, ::GetCurrentProcessId());
    }

    // Разосланные нажатия — от экранной клавиатуры, макросов, средств
    // доступности — принимаются наравне с настоящими. Своих мы не рассылаем, и
    // услышать самих себя нам нечем; а игроку, который набирает имя экранной
    // клавиатурой, отказывать не за что.
    if (!input->handleKey(key->vkCode, key->scanCode, down)) {
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // Единица означает «дальше не передавать»: игра нажатия не увидит вовсе.
    return 1;
}

bool MenuInput::handleQueuedKey(const MSG& message) {
    const bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    const bool up = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;

    if (!down && !up && message.message != WM_CHAR) {
        return false;
    }

    if (!sawQueuedKey_) {
        sawQueuedKey_ = true;
        spdlog::info("нажатия доходят очередью сообщений");
    }

    if (down && message.message != WM_CHAR &&
        isLayoutSwitch(static_cast<unsigned>(message.wParam))) {
        switchWanted_.store(true, std::memory_order_relaxed);
    }

    const auto key = static_cast<unsigned>(message.wParam);
    const auto scan = static_cast<unsigned>((message.lParam >> 16) & 0xFF);

    // Свои клавиши разбираются прежде вопроса «нужен ли странице ввод», и
    // порядок здесь тот же, что у двух других путей. Иначе получалось бы, что
    // закрытое меню уже не открыть: спросив сперва, мы отвечали бы «ввод не
    // нужен» и уходили, не дойдя до клавиши, которой его как раз и просят.
    if (message.message != WM_CHAR) {
        if (key == VK_F1) {
            if (down && actions_.toggle) {
                actions_.toggle();
            }
            return true;
        }

        if (key == VK_F8) {
            if (down && actions_.console) {
                actions_.console();
            }
            return true;
        }

        if (key == VK_F4 && held(VK_MENU)) {
            if (down && actions_.askExit) {
                spdlog::info("Alt+F4 — выходим из игры");
                actions_.askExit();
            }
            return true;
        }
    }

    if (!actions_.wanted || !actions_.wanted()) {
        return false;
    }

    // Готовая буква из очереди только съедается, а странице не отдаётся.
    //
    // Отдать её значило бы отдать дважды: букву мы делаем сами, из кода клавиши
    // и своей раскладки, — а эта пришла бы по раскладке игрового потока, которую
    // игра держит своей. Съедать её при этом обязательно: игре она не нужна, а
    // оставленная в очереди, она дошла бы до её же разбора ввода.
    if (message.message == WM_CHAR) {
        return true;
    }

    browser_->sendKey(down ? cefui::Browser::KeyAction::Down : cefui::Browser::KeyAction::Up, key,
                      scan, held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));

    if (down && !isModifier(key)) {
        sendCharacters(key, scan);
    }

    // Съедается всё, включая Alt и Shift, и это исправление по живой игре.
    //
    // Оставленный игре Alt уводит её окно в системное меню — то самое, что
    // открывается по одному Alt в любом окне Windows. Пока оно открыто, окно не
    // разбирает обычных нажатий вовсе, и набор в меню обрывается на первой же
    // попытке сменить раскладку. Ровно это и случилось: сперва набиралось, потом
    // Alt+Shift — и всё.
    //
    // Смену раскладки это не ломает: круг раскладок ведём мы сами, выше.
    return true;
}

bool MenuInput::handleKey(unsigned key, unsigned scan, bool down) {
    // Перехват стоит на всю систему — иначе он не увидел бы клавиш вовсе, — и
    // потому первым делом спрашивается, в игре ли мы сейчас. Не спроси мы этого,
    // игрок, переключившийся в браузер при открытом меню, набирал бы в нём
    // вслепую: буквы уходили бы к нам и до браузера не доходили.
    //
    // Спрашивается процесс, а не само окно, и это важно: окон у игры несколько,
    // главным становится не всегда то, на которое повешен обработчик, и сверка
    // указателей отсекала бы нажатия молча.
    DWORD process = 0;
    ::GetWindowThreadProcessId(::GetForegroundWindow(), &process);

    if (process != ::GetCurrentProcessId()) {
        return false;
    }

    // Одна запись за запуск: по ней видно, что нажатия до перехвата доходят.
    // Без неё «клавиатура не работает» означает сразу две разные беды — нажатие
    // не пришло или не дошло до страницы, — и различить их нечем.
    if (!sawKey_) {
        sawKey_ = true;
        spdlog::info("перехват клавиатуры видит нажатия");
    }

    // Смена языка ввода замечается прежде вопроса «нужен ли ввод странице»:
    // раскладка нужна в игре не меньше, чем в меню, а в игре меню закрыто.
    //
    // Здесь только помечается: переключать язык можно лишь в потоке игры, а этот
    // перехват работает в нашем.
    if (down && isLayoutSwitch(key)) {
        switchWanted_.store(true, std::memory_order_relaxed);
    }

    // F1 разбирается прежде всего остального и в обе стороны: закрытым меню он
    // открывает, открытым — закрывает. Своей клавиши у страницы нет.
    if (key == VK_F1) {
        if (down && actions_.toggle) {
            actions_.toggle();
        }
        return true;
    }

    // F8 — консоль страницы. Тоже в обе стороны и независимо от меню: консоль
    // живёт поверх него и вне его.
    if (key == VK_F8) {
        if (down && actions_.console) {
            actions_.console();
        }
        return true;
    }

    // Alt+F4 — тоже в обе стороны, открыто меню или нет. У игры на него свой
    // разговор с вопросом «выйти?», и разговор этот здесь лишний: выход из
    // oxyMP один, тот же, что по кнопке в меню, и он умеет то, чего не умеет
    // игровой, — дождаться и добить, если игра не закрылась сама.
    if (key == VK_F4 && held(VK_MENU)) {
        if (down && actions_.askExit) {
            spdlog::info("Alt+F4 — выходим из игры");
            actions_.askExit();
        }
        return true;
    }

    if (!actions_.wanted || !actions_.wanted()) {
        return false;
    }

    // Управляющие клавиши доходят до страницы, но не отбираются: ими Windows
    // разбирает свои сочетания.
    if (isModifier(key)) {
        browser_->sendKey(down ? cefui::Browser::KeyAction::Down : cefui::Browser::KeyAction::Up,
                          key, scan, held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));
        return false;
    }

    browser_->sendKey(down ? cefui::Browser::KeyAction::Down : cefui::Browser::KeyAction::Up, key,
                      scan, held(VK_SHIFT), held(VK_CONTROL), held(VK_MENU));

    if (down) {
        sendCharacters(key, scan);
    }

    return true;
}

void MenuInput::sendCharacters(unsigned key, unsigned scan) {
    // Какую букву даёт клавиша, решают раскладка, регистр и мёртвые знаки.
    // Раньше это решала Windows и присылала готовый WM_CHAR; теперь сообщения до
    // нас не доходят, и перевод приходится делать самим — тем же средством,
    // которым его делает сама Windows.
    //
    // Состояние клавиш собирается спросом у системы, а не берётся из очереди
    // нашего потока: перехват работает в стороне от неё, и там всё было бы
    // отпущено.
    BYTE keyboard[256]{};

    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        keyboard[VK_SHIFT] = 0x80;
    }
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        keyboard[VK_CONTROL] = 0x80;
    }
    if ((::GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        keyboard[VK_MENU] = 0x80;
    }
    if ((::GetKeyState(VK_CAPITAL) & 1) != 0) {
        keyboard[VK_CAPITAL] = 1;
    }

    wchar_t characters[8]{};

    // Раскладка спрашивается у Windows на каждую букву, а не помнится своей.
    //
    // Прежде здесь был свой круг раскладок, и он был вынужденным: игра глушила
    // системные сочетания, и Windows раскладку не меняла вовсе. Теперь глушение
    // снято (см. raw_input.hpp), переключает сама Windows — и свой круг из
    // помощи превратился во вред: переключали оба, игрок получал ноль, «туда и
    // сразу обратно».
    const HKL layout = foregroundLayout();

    const int written = ::ToUnicodeEx(key, scan, keyboard, characters,
                                      static_cast<int>(std::size(characters)), 0, layout);

    // Отрицательное означает мёртвый знак — тот, что ждёт следующей клавиши,
    // чтобы сложиться с ней в букву. Показывать его отдельно нечего.
    if (written <= 0) {
        // Одна запись за запуск: буквы не рождаются по разным причинам —
        // клавиша без знака, чужая раскладка, мёртвый знак, — и по числу видно,
        // по какой именно.
        if (!loggedSilentKey_) {
            loggedSilentKey_ = true;

            spdlog::info("клавиша {} (скан {}) при раскладке {:#x} не дала буквы: перевод "
                         "вернул {}",
                         key, scan, reinterpret_cast<std::uintptr_t>(layout), written);
        }
        return;
    }

    for (int i = 0; i < written; ++i) {
        const auto symbol = static_cast<unsigned>(characters[i]);

        // Управляющие знаки странице не нужны: возврат каретки, табуляцию и
        // забой она разбирает по коду клавиши, а не по знаку.
        if (symbol < 0x20 || symbol == 0x7F) {
            continue;
        }

        if (!sentCharacter_) {
            sentCharacter_ = true;
            spdlog::info("буквы отдаются странице: первая — {:#06x}", symbol);
        }

        browser_->sendKey(cefui::Browser::KeyAction::Char, symbol, scan, held(VK_SHIFT),
                          held(VK_CONTROL), held(VK_MENU));
    }
}

LRESULT CALLBACK MenuInput::proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    MenuInput* const input = g_input;

    if (input != nullptr) {
        // Ответ разбирается, а не сводится к «съедено»: у WM_SETCURSOR своё
        // значение возврата, и нулём ему говорят «разбирайтесь дальше сами» —
        // после чего игра ставит свой пустой указатель, и всё возвращается к
        // тому, с чего начали.
        if (const std::optional<LRESULT> answer = input->handle(message, wparam, lparam)) {
            return *answer;
        }
    }

    if (input != nullptr && input->previous_ != nullptr) {
        return ::CallWindowProcW(input->previous_, window, message, wparam, lparam);
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
}

std::optional<LRESULT> MenuInput::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    const bool open = actions_.wanted && actions_.wanted();

    // F1 разбирается прежде всего остального и в обе стороны: закрытым меню он
    // открывает, открытым — закрывает. Иначе закрытое меню было бы уже не
    // достать, а открытое — не убрать: своей клавиши у страницы нет.
    //
    // Прежде этим занимался Escape, и его пришлось отдать обратно. Escape нужен
    // самой странице: им она закрывает свои разговоры — выбор сервера, спрос
    // разрешений, настройки, — и пока он уходил на открытие и закрытие всего
    // меню, выйти из такого разговора было нечем.
    //
    // Оба сообщения, и нажатие и отпускание: игра иначе увидит одно без другого
    // и сочтёт клавишу зажатой.
    if (message == WM_KEYDOWN && wparam == VK_F1) {
        if (actions_.toggle) {
            actions_.toggle();
        }

        return 0;
    }

    if (message == WM_KEYUP && wparam == VK_F1) {
        return 0;
    }

    if (message == WM_KEYDOWN && wparam == VK_F8) {
        if (actions_.console) {
            actions_.console();
        }

        return 0;
    }

    if (message == WM_KEYUP && wparam == VK_F8) {
        return 0;
    }

    // Alt+F4 — третьим путём к тому же выходу, и путь этот не запасной.
    //
    // Два других — низкоуровневый перехват и очередь сообщений — работают по
    // очереди: увидев нажатия первым, второй отключается. Стоит первому один
    // раз не увидеть Alt+F4 (окно игры выше по правам, чужой полноэкранный
    // режим), и нажатие не разберёт уже никто.
    //
    // Отданное игре, оно уходит в обычный обработчик Windows, а тот превращает
    // его в закрытие окна — то есть в её собственный вопрос «выйти?». Ровно от
    // него мы и уходим: спрашивает об этом наше окно, а выход один на всех.
    //
    // Оба сообщения, и нажатие и отпускание, — как у F1: игра иначе увидит одно
    // без другого и сочтёт клавишу зажатой.
    if (message == WM_SYSKEYDOWN && wparam == VK_F4 && held(VK_MENU)) {
        if (actions_.askExit) {
            spdlog::info("Alt+F4 — выходим из игры");
            actions_.askExit();
        }

        return 0;
    }

    if (message == WM_SYSKEYUP && wparam == VK_F4) {
        return 0;
    }

    // Просьба закрыть окно снаружи игры — панелью задач, диспетчером задач,
    // системным меню окна.
    //
    // Оставленная игре, она поднимает её собственный вопрос «выйти?»: именно так
    // игра отвечает на WM_CLOSE, и это проверено — пока мы слали его сами, она
    // не закрылась по нему ни разу. Своего окна здесь не показываем: игрок
    // просил закрыть окно снаружи, и к этому мгновению оно может быть свёрнуто —
    // вопрос он увидел бы не скоро.
    //
    // Младшие четыре бита у SC_CLOSE Windows держит за собой, отсюда и маска.
    if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_CLOSE)) {
        spdlog::info("Windows просит закрыть окно игры — выходим");

        if (actions_.exitNow) {
            actions_.exitNow();
        }

        return 0;
    }

    if (open != wasOpen_) {
        wasOpen_ = open;

        // Внимание отдаётся странице прямо: окна у неё нет, и Windows не может
        // отнять его в её пользу сама. Без этого поля ввода не показывают
        // курсора и не принимают набранного.
        browser_->setFocus(open);

        // Указатель показывается, пока меню открыто, и прячется, когда оно
        // закрылось. Подробности — у showCursor: там счётчик, а не выключатель.
        showCursor(open, raised_);

        if (!open) {
            browser_->releaseMouse();
        }
    }

    // Окно игры снова стало главным — внимание возвращается странице. Сообщение
    // при этом не съедается: игре оно нужно не меньше, чем нам.
    if (open && (message == WM_ACTIVATE || message == WM_SETFOCUS)) {
        browser_->setFocus(true);
    }

    // Раскладку игрового потока мы не трогаем, и это записано кровью.
    //
    // Попытка была: обработчик окна зовётся потоком игры, и отсюда раскладку
    // этого потока можно переставить — чтобы и указатель языка в панели задач
    // показывал выбранную. Кончилось это зависанием игры на несколько секунд:
    // игра возвращает свою раскладку всякий раз, как окно становится главным, и
    // мы с ней толкали её навстречу друг другу — по разу на каждое сообщение
    // мыши, то есть сотни раз в секунду. Отвиснув, игра всё равно оставалась при
    // своей.
    //
    // Поэтому раскладка для набора живёт только у нас (layout_), а указатель в
    // панели задач показывает то, что считает нужным игра. Набору это не мешает.

    if (!open) {
        return std::nullopt;
    }

    switch (message) {
    case WM_SETCURSOR:
        // Указатель приходится ставить самим, каждым сообщением.
        //
        // ShowCursor выше поднимает счётчик показа, но этого мало: игра сама
        // отвечает на WM_SETCURSOR и ставит пустой указатель — ей он не нужен,
        // она рисует своё прицеливание. Оттого меню и оказывалось без мыши:
        // страница мышь получала и на неё отзывалась, а видно указателя не было.
        //
        // Своего указателя у страницы быть не может: она рисуется в память, а
        // указатель — дело Windows, и в её кадр он не попадает.
        ::SetCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); // IDC_ARROW
        return TRUE;

    case WM_MOUSEMOVE:
        browser_->moveMouse(mouseX(lparam), mouseY(lparam));
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        // Внимание странице отдаётся на каждое нажатие, а не однажды при
        // открытии меню, и это исправление по живой игре.
        //
        // Отданное однажды, оно терялось: окно игры получает и отдаёт фокус не
        // раз за запуск — при переключении между окнами, при переходе в
        // полноэкранный режим, — и вместе с ним фокус терял браузер. Снаружи это
        // выглядело загадочно: щелчки работали, кнопки нажимались, страницы
        // переключались, а поля ввода не принимали ни буквы. Кнопке фокус не
        // нужен, полю нужен.
        //
        // Щелчок — то самое мгновение, когда фокус и требуется: игрок именно
        // что показывает, куда он собрался писать.
        browser_->setFocus(true);

        browser_->clickMouse(mouseX(lparam), mouseY(lparam), cefui::Browser::MouseButton::Left,
                             message == WM_LBUTTONDOWN);
        return 0;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        browser_->clickMouse(mouseX(lparam), mouseY(lparam), cefui::Browser::MouseButton::Right,
                             message == WM_RBUTTONDOWN);
        return 0;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        browser_->clickMouse(mouseX(lparam), mouseY(lparam), cefui::Browser::MouseButton::Middle,
                             message == WM_MBUTTONDOWN);
        return 0;

    case WM_MOUSEWHEEL: {
        // Колесо приходит в координатах экрана, а не окна: так устроено это
        // сообщение, в отличие от всех остальных мышиных. Странице нужны
        // оконные.
        POINT point{mouseX(lparam), mouseY(lparam)};
        ::ScreenToClient(window_, &point);

        browser_->scrollMouse(point.x, point.y, GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_CHAR:
        // Клавиши разбирает низкоуровневый перехват, а не окно: до окна они не
        // доходят вовсе. Здесь они только съедаются — на случай, если какое-то
        // сообщение всё-таки дойдёт: пройди оно дальше, страница получила бы
        // букву дважды.
        return 0;

    case WM_INPUTLANGCHANGEREQUEST:
        // Смену раскладки исполняем мы сами, и никому её не передаём.
        //
        // Просьбу о смене исполняет тот, кто отдаст её обычному обработчику
        // Windows. Игра её съедает — оттого раскладка в ней и «переключается и
        // возвращается обратно»: нажатие видно, а смены нет. Передать просьбу
        // игре и надеяться было нельзя — проверено, ничего не меняется.
        //
        // Здесь раскладка нужна: имя игрока и адрес сервера набирают буквами, и
        // кириллица среди них — обычное дело.
        return ::DefWindowProcW(window_, message, wparam, lparam);

    default:
        return std::nullopt;
    }
}

} // namespace oxymp::client::game
