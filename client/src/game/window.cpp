#include "window.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <cwchar>
#include <thread>
#include <utility>

namespace oxymp::client::game {
namespace {

/// Номер значка в ресурсах модуля. Тот же, что в oxymp-client.rc.
constexpr int kIconId = 1;

/// Сколько ждать ответа от хозяина окна, в миллисекундах.
///
/// Немного и намеренно: не ответил — значит занят, и подождём до следующего
/// раза. Заголовок окна не стоит ни одного пропущенного кадра игры.
constexpr UINT kMessageTimeout = 200;

/// Как часто трогать уже названное окно.
constexpr auto kApplyInterval = std::chrono::seconds{1};

/// Как часто искать окно, которое ещё не названо.
///
/// Окно игра создаёт не в первое мгновение, а клиент внедряется раньше него.
/// Секундный шаг здесь означал бы до секунды с рокстаровским именем и значком в
/// панели задач — то самое, что видно как «скин встаёт не сразу».
constexpr auto kSearchInterval = std::chrono::milliseconds{100};

/// Сколько ждать окна, прежде чем жаловаться на его отсутствие.
///
/// Полминуты: столько игра идёт от запуска до первого своего кадра, и всё это
/// время окна может не быть по совершенно законной причине.
constexpr auto kSearchPatience = std::chrono::seconds{30};

/// Класс главного окна GTA V.
///
/// Опознание по классу, а не по виду. Отбор «видимое окно верхнего уровня без
/// хозяина» казался достаточным и оказался нет: игра меняет окно, входя в мир, и
/// в этот промежуток под описание не подходит ни одно — обход возвращал пустоту,
/// а заголовок молча не ставился.
///
/// Класс же у окна игры один и тот же всегда, и по нему оно опознаётся, даже
/// пока не показано.
constexpr const wchar_t* kGameWindowClass = L"grcWindow";

/// Что ищет обходчик окон.
struct Search {
    DWORD process = 0;

    /// Окно с правильным классом. Ему верим безоговорочно.
    HWND byClass = nullptr;

    /// Видимое окно верхнего уровня. Запасной ответ на случай, если класс у
    /// игры однажды сменится: пусть лучше заголовок встанет не туда, чем не
    /// встанет никуда, — ошибку видно сразу, а молчание нет.
    HWND byLook = nullptr;
};

BOOL CALLBACK pickWindow(HWND window, LPARAM parameter) {
    auto* const search = reinterpret_cast<Search*>(parameter);

    DWORD process = 0;
    ::GetWindowThreadProcessId(window, &process);

    if (process != search->process) {
        return TRUE;
    }

    std::array<wchar_t, 64> className{};
    ::GetClassNameW(window, className.data(), static_cast<int>(className.size()));

    if (::wcscmp(className.data(), kGameWindowClass) == 0) {
        search->byClass = window;
        return FALSE;
    }

    // Своё окно-холст пропускается: под запасной отбор оно подходит идеально —
    // видимое, верхнего уровня, без хозяина, — и приняв его за окно игры, мы
    // повесили бы на него заголовок и перехват клавиатуры.
    if (::wcscmp(className.data(), Window::kSurfaceClass) == 0) {
        return TRUE;
    }

    if (search->byLook == nullptr && ::IsWindowVisible(window) != FALSE &&
        ::GetWindow(window, GW_OWNER) == nullptr) {
        search->byLook = window;
    }

    return TRUE;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          size);

    return wide;
}

} // namespace

HWND Window::findOwnWindow() noexcept {
    Search search{.process = ::GetCurrentProcessId()};
    ::EnumWindows(&pickWindow, reinterpret_cast<LPARAM>(&search));
    return search.byClass != nullptr ? search.byClass : search.byLook;
}

Window::Window() noexcept {
    // Окна может не быть вовсе, и это не беда: клиент внедряется раньше, чем
    // игра его создаёт. Жалоба на пропажу живёт в apply и уходит лишь тогда,
    // когда ожидание затянулось.
    window_ = findOwnWindow();

    // LoadImage, а не LoadIcon: последний отдаёт значок системного размера, и
    // крупный вариант для панели задач пришлось бы догружать отдельно. Нулевые
    // размеры вместе с LR_DEFAULTSIZE означают «взять из файла как есть».
    // Описатель своего модуля — по адресу собственного кода: другого надёжного
    // способа у библиотеки нет, а GetModuleHandle(nullptr) отдал бы саму игру,
    // в ресурсах которой нашего значка, разумеется, нет.
    HMODULE module = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&findOwnWindow), &module);

    icon_ = static_cast<HICON>(::LoadImageW(module, MAKEINTRESOURCEW(kIconId), IMAGE_ICON, 0, 0,
                                            LR_DEFAULTSIZE | LR_SHARED));

    if (icon_ == nullptr) {
        spdlog::warn("the icon was not found in the module resources: the window keeps Rockstar's");
    }
}

void Window::apply(const std::string& title) {
    const auto now = std::chrono::steady_clock::now();
    const auto interval = named_ ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       kApplyInterval)
                                 : std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       kSearchInterval);

    if (appliedAt_ != std::chrono::steady_clock::time_point{} && now - appliedAt_ < interval) {
        return;
    }
    appliedAt_ = now;

    if (window_ == nullptr || ::IsWindow(window_) == FALSE) {
        // Окно игра меняет, входя в мир, поэтому потерянное ищется заново, а не
        // считается пропавшим навсегда.
        window_ = findOwnWindow();
        applied_.clear();
        iconApplied_ = false;

        // Окно у игры новое — значит и названо оно снова её именем, и искать
        // его до тех пор нужно часто.
        named_ = false;

        if (window_ == nullptr) {
            // Жалоба по сроку, а не по первой же пропаже, и по разу, а не
            // каждую проверку. Окна нет в двух совсем разных случаях: в первые
            // секунды запуска, когда игра его ещё не создала, и в те мгновения,
            // когда она его пересоздаёт, входя в мир. Оба нормальны, и жаловаться
            // на них значило бы завести в журнале пугало, за которым ничего нет.
            if (missingSince_ == std::chrono::steady_clock::time_point{}) {
                missingSince_ = now;
            }

            if (!lostReported_ && now - missingSince_ >= kSearchPatience) {
                lostReported_ = true;
                spdlog::warn("the game window was not found: title and icon cannot be set yet");
            }
            return;
        }

        missingSince_ = {};
        lostReported_ = false;
        spdlog::debug("окно игры найдено заново: {}", static_cast<const void*>(window_));
    }

    const std::wstring wanted = widen(title);

    // Сверка с тем, что стоит в окне сейчас, а не с тем, что мы ставили: игра
    // могла переписать заголовок сама, и наша память об этом не знает.
    std::wstring current(wanted.size() + 1, L'\0');
    const int copied =
        ::GetWindowTextW(window_, current.data(), static_cast<int>(current.size()));
    current.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);

    if (current != wanted) {
        // SendMessageTimeout, а не SetWindowText и не SendMessage.
        //
        // Окно принадлежит одному потоку игры, а мы работаем в другом, и обе
        // эти функции в таком случае ждут, пока хозяин окна разберёт очередь
        // сообщений. Если он в это время занят или ждёт чего-то нашего —
        // ожидание становится взаимным, и игра замирает целой и не отвечающей.
        // Так и вышло, когда это делалось из скриптового тика.
        //
        // SMTO_ABORTIFHUNG плюс срок означают: не отвечает — и не надо,
        // попробуем через секунду. Заголовок не та вещь, ради которой стоит
        // останавливать игру.
        ::SendMessageTimeoutW(window_, WM_SETTEXT, 0,
                              reinterpret_cast<LPARAM>(wanted.c_str()),
                              SMTO_ABORTIFHUNG | SMTO_NORMAL, kMessageTimeout, nullptr);

        if (applied_ != wanted) {
            spdlog::debug("заголовок окна игры: \"{}\"", title);
            applied_ = wanted;
        }
    }

    if (icon_ != nullptr && !iconApplied_) {
        // Оба размера: маленький берёт заголовок окна и переключатель задач,
        // большой — панель задач и Alt+Tab. Поставив один, второй оставляешь
        // рокстаровским, и они разойдутся на глазах у игрока.
        ::SendMessageTimeoutW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_),
                              SMTO_ABORTIFHUNG | SMTO_NORMAL, kMessageTimeout, nullptr);
        ::SendMessageTimeoutW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_),
                              SMTO_ABORTIFHUNG | SMTO_NORMAL, kMessageTimeout, nullptr);

        iconApplied_ = true;
        spdlog::debug("значок окна игры заменён");
    }

    // Названо. Дальше сверяться раз в секунду довольно: игра переписывает
    // заголовок считаное число раз за запуск, и лишней секунды с её именем на
    // таком переходе никто не заметит — в отличие от первой, самой видной.
    named_ = true;
}

WindowName::~WindowName() = default;

/// Что видит поток, держащий имя окна.
struct WindowName::State {
    /// Имя объявлено до потока: поток читает его, и переживать его оно обязано.
    std::string title;

    std::atomic<bool> stop{false};
    std::thread worker;

    ~State() {
        stop.store(true, std::memory_order_relaxed);

        if (worker.joinable()) {
            worker.join();
        }
    }
};

std::unique_ptr<WindowName> WindowName::hold(std::string title) {
    std::unique_ptr<WindowName> owner{new WindowName};

    owner->state_ = std::make_unique<State>();
    owner->state_->title = std::move(title);

    State* const state = owner->state_.get();

    state->worker = std::thread{[state] {
        // Окно заводится здесь, а не снаружи: искать его вправе только тот, кто
        // с ним и работает, а работает с ним один этот поток.
        Window window;

        while (!state->stop.load(std::memory_order_relaxed)) {
            window.apply(state->title);

            // Шаг сна короче любого из шагов самой Window: она сама решает,
            // трогать окно или пропустить, а нам довольно не проспать
            // мгновение, когда окно появится.
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }};

    return owner;
}

} // namespace oxymp::client::game
