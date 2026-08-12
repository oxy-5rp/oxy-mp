#include "window.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cwchar>

namespace oxymp::client::game {
namespace {

/// Номер значка в ресурсах модуля. Тот же, что в oxymp-client.rc.
constexpr int kIconId = 1;

/// Сколько ждать ответа от хозяина окна, в миллисекундах.
///
/// Немного и намеренно: не ответил — значит занят, и подождём до следующего
/// раза. Заголовок окна не стоит ни одного пропущенного кадра игры.
constexpr UINT kMessageTimeout = 200;

/// Как часто вообще трогать окно.
constexpr auto kApplyInterval = std::chrono::seconds{1};

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
    window_ = findOwnWindow();

    if (window_ == nullptr) {
        spdlog::warn("окно игры не найдено — заголовок и значок останутся рокстаровскими");
        return;
    }

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
        spdlog::warn("значок не найден в ресурсах модуля — окну останется рокстаровский");
    }

    spdlog::info("окно игры найдено: {}", static_cast<const void*>(window_));
}

void Window::apply(const std::string& title) {
    const auto now = std::chrono::steady_clock::now();
    if (appliedAt_ != std::chrono::steady_clock::time_point{} && now - appliedAt_ < kApplyInterval) {
        return;
    }
    appliedAt_ = now;

    if (window_ == nullptr || ::IsWindow(window_) == FALSE) {
        // Окно игра меняет, входя в мир, поэтому потерянное ищется заново, а не
        // считается пропавшим навсегда.
        window_ = findOwnWindow();
        applied_.clear();
        iconApplied_ = false;

        if (window_ == nullptr) {
            // Жаловаться по разу, а не каждую секунду: окна может не быть
            // считаные мгновения, пока игра его пересоздаёт, и превращать это в
            // поток жалоб незачем.
            if (!lostReported_) {
                lostReported_ = true;
                spdlog::warn("окно игры не найдено — заголовок и значок пока не поставить");
            }
            return;
        }

        lostReported_ = false;
        spdlog::info("окно игры найдено заново: {}", static_cast<const void*>(window_));
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
            spdlog::info("заголовок окна игры: \"{}\"", title);
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
        spdlog::info("значок окна игры заменён");
    }
}

} // namespace oxymp::client::game
