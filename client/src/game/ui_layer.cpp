#include "ui_layer.hpp"

#include "overlay_renderer.hpp"
#include "page_source.hpp"
#include "pointer.hpp"
#include "present_hook.hpp"
#include "window.hpp"

#include "../ui_feed.hpp"
#include "../ui_mail.hpp"

#include <oxymp/cefui/browser.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace oxymp::client::game {
namespace {

/// Предел размера страницы.
///
/// Точки страницы каждый раз проходят через обычную память, и на четырёх тысячах
/// точек по ширине это стало бы заметно. Интерфейс при этом ничего не теряет:
/// растянутый до 4K, он выглядит ровно так же, как нарисованный в нём.
constexpr int kMaxWidth = 1920;
constexpr int kMaxHeight = 1080;

/// Размер страницы, пока размер кадра игры неизвестен.
constexpr int kInitialWidth = 1280;
constexpr int kInitialHeight = 720;

/// Как часто страница получает состояние.
///
/// Двадцать раз в секунду: чаще ей нечего показывать — ни задержка до сервера,
/// ни список игроков, ни ход загрузки не меняются быстрее.
constexpr auto kStateInterval = std::chrono::milliseconds{50};

/// Как часто опрашивается мышь.
///
/// Чаще состояния, и намного: состояние меняется в такт событиям, а указатель —
/// в такт руке. Двадцать шагов в секунду рука замечает как рывки.
constexpr auto kPointerInterval = std::chrono::milliseconds{16};

/// Значение поля в сообщении страницы.
///
/// Разбор нарочно короткий. Сообщения складывает та же страница, которую пишем
/// мы, и вид у них один на все: имя, а за ним строка или число. Полноценный
/// разбор JSON ради двух полей был бы библиотекой, которую никто не просил.
std::string_view valueOf(std::string_view json, std::string_view name) {
    const std::string key = std::string{"\""} + std::string{name} + "\":";

    const std::size_t at = json.find(key);
    if (at == std::string_view::npos) {
        return {};
    }

    std::string_view rest = json.substr(at + key.size());

    if (!rest.empty() && rest.front() == '"') {
        rest.remove_prefix(1);

        const std::size_t end = rest.find('"');
        return end == std::string_view::npos ? std::string_view{} : rest.substr(0, end);
    }

    const std::size_t end = rest.find_first_of(",}");
    return rest.substr(0, end == std::string_view::npos ? rest.size() : end);
}

/// Разбирает нажатие, о котором рассказала страница.
///
/// Пусто — значит сказанное нам непонятно. Молча, без жалобы в журнал: страница
/// своя, и незнакомое сообщение от неё означает не беду, а несобранную заново
/// пару «страница и клиент».
std::optional<UiClick> readClick(std::string_view json) {
    const std::string_view kind = valueOf(json, "kind");

    UiClick click;

    if (kind == "point") {
        click.kind = UiClick::Kind::Point;
    } else if (kind == "press") {
        click.kind = UiClick::Kind::Press;
    } else if (kind == "alternate") {
        click.kind = UiClick::Kind::Alternate;
    } else if (kind == "back") {
        click.kind = UiClick::Kind::Back;
    } else if (kind == "close") {
        click.kind = UiClick::Kind::Close;
    } else if (kind == "ask") {
        click.kind = UiClick::Kind::Ask;
    } else {
        return std::nullopt;
    }

    const std::string_view index = valueOf(json, "index");
    std::from_chars(index.data(), index.data() + index.size(), click.index);

    return click;
}

/// Каталог с хозяйством CEF.
///
/// Ищется рядом с самим модулем, а не рядом с исполняемым файлом: исполняемый
/// файл здесь — GTA5.exe, и её папки oxyMP не касается вовсе. Свой же модуль
/// лежит в каталоге клиента, где рядом лежит и cef.
std::filesystem::path cefDirectory() {
    HMODULE module = nullptr;

    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&cefDirectory), &module) == 0) {
        return {};
    }

    std::wstring path(MAX_PATH, L'\0');
    const DWORD written =
        ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0 || written >= path.size()) {
        return {};
    }
    path.resize(written);

    return std::filesystem::path{path}.parent_path() / "cef";
}

} // namespace

/// Всё хозяйство слоя.
///
/// Вынесено из заголовка целиком: иначе всякий, кто подключит ui_layer.hpp,
/// получил бы следом Direct3D и половину Chromium.
struct UiLayer::State {
    UiFeed* feed = nullptr;
    UiMail* mail = nullptr;

    std::unique_ptr<oxymp::cefui::Browser> browser;
    std::unique_ptr<PresentHook> hook;
    OverlayRenderer renderer;

    /// Последний кадр страницы.
    ///
    /// Пишет его поток CEF, читает поток отрисовки игры — отсюда и блокировка.
    /// Под ней происходит только обмен двух буферов, и удерживают её обе стороны
    /// на считаные микросекунды.
    std::mutex mutex;
    std::vector<std::uint8_t> incoming;
    int incomingWidth = 0;
    int incomingHeight = 0;
    bool fresh = false;

    /// Кадр, который сейчас рисуется. Живёт только в потоке игры.
    std::vector<std::uint8_t> shown;
    int shownWidth = 0;
    int shownHeight = 0;

    /// Размер, под который подогнана страница.
    int width = kInitialWidth;
    int height = kInitialHeight;

    /// Поток, который ведёт страницу: подгоняет её под размер кадра, отдаёт ей
    /// состояние и водит по ней указателем. Свой, а не чужой, потому что чужие
    /// заняты: поток игры рисует кадр, а сетевой минуту дожидается готовности
    /// движка.
    std::thread keeper;
    std::atomic<bool> stopped{false};

    /// Мышь, доведённая до страницы. Живёт только в этом потоке.
    Pointer pointer;

    /// Подгоняет страницу под размер кадра игры.
    ///
    /// Размер спрашивается у окна, а не у swapchain: до окна отсюда дотянуться
    /// можно, а до swapchain — только из потока отрисовки, где заниматься этим
    /// незачем.
    void fitToWindow() {
        const HWND game = Window::findOwnWindow();
        if (game == nullptr) {
            return;
        }

        RECT bounds{};
        ::GetClientRect(game, &bounds);

        const int wanted = std::min(static_cast<int>(bounds.right - bounds.left), kMaxWidth);
        const int high = std::min(static_cast<int>(bounds.bottom - bounds.top), kMaxHeight);

        if (wanted <= 0 || high <= 0 || (wanted == width && high == height)) {
            return;
        }

        width = wanted;
        height = high;

        browser->resize(wanted, high);
        spdlog::debug("страница интерфейса подогнана: {}x{}", wanted, high);
    }

    void keep() {
        auto stateDue = std::chrono::steady_clock::now();

        while (!stopped.load()) {
            const auto now = std::chrono::steady_clock::now();

            if (now >= stateDue) {
                stateDue = now + kStateInterval;

                fitToWindow();
                browser->post(feed->takeUpdate());
            }

            // Указатель ведём, только пока открыто меню. В остальное время мышь
            // принадлежит игре: ею водят камерой, и события от неё страница
            // получала бы впустую — а курсор, нарисованный поверх игры, стоял бы
            // посреди боя.
            if (feed->menuOpen()) {
                pointer.follow(*browser, width, height);
            } else {
                pointer.release(*browser);
            }

            std::this_thread::sleep_for(kPointerInterval);
        }
    }
};

std::unique_ptr<UiLayer> UiLayer::create(UiFeed& feed, UiMail& mail, std::string& error) {
    const std::filesystem::path directory = cefDirectory();
    if (directory.empty()) {
        error = "не удалось найти каталог с хозяйством CEF";
        return nullptr;
    }

    if (!oxymp::cefui::startRuntime(directory.wstring(), error)) {
        return nullptr;
    }

    std::unique_ptr<UiLayer> layer{new UiLayer};
    layer->state_ = std::make_unique<State>();

    State& state = *layer->state_;
    state.feed = &feed;
    state.mail = &mail;

    state.browser = oxymp::cefui::Browser::create(
        kInitialWidth, kInitialHeight,
        [&state](const std::uint8_t* pixels, int width, int height) {
            const std::size_t bytes =
                static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;

            const std::lock_guard guard{state.mutex};

            state.incoming.resize(bytes);
            std::memcpy(state.incoming.data(), pixels, bytes);

            state.incomingWidth = width;
            state.incomingHeight = height;
            state.fresh = true;
        },
        error);

    if (state.browser == nullptr) {
        return nullptr;
    }

    // Обратный путь: страница называет пункт, на который нажали мышью, а решает
    // по-прежнему игра. Приходит это из потока CEF, поэтому дальше почты здесь
    // ничего не делается.
    state.browser->onMessage([&state](std::string_view json) {
        if (const std::optional<UiClick> click = readClick(json)) {
            state.mail->post(*click);
        }
    });

    state.browser->show(composePage());

    state.hook = PresentHook::install(
        [&state](IDXGISwapChain* swapchain) {
            bool changed = false;

            {
                const std::lock_guard guard{state.mutex};

                if (state.fresh) {
                    // Обмен, а не копия: буфер занимает до восьми мегабайт, и
                    // копировать их в потоке отрисовки игры значило бы отнимать
                    // у кадра миллисекунду на ровном месте.
                    state.shown.swap(state.incoming);
                    state.shownWidth = state.incomingWidth;
                    state.shownHeight = state.incomingHeight;

                    state.fresh = false;
                    changed = true;
                }
            }

            state.renderer.draw(swapchain, state.shown.data(), state.shownWidth,
                                state.shownHeight, changed);
        },
        [&state] { state.renderer.releaseFrameResources(); }, error);

    if (state.hook == nullptr) {
        return nullptr;
    }

    // Поток заводится последним: до этого мгновения ему нечего вести.
    state.keeper = std::thread{[&state] { state.keep(); }};

    spdlog::info("игровой интерфейс поднят внутри кадра");
    return layer;
}

UiLayer::~UiLayer() {
    if (state_ == nullptr) {
        return;
    }

    // Первым останавливается поток, ведущий страницу: он обращается и к
    // браузеру, и к ленте состояния, и работать ему после их разрушения было бы
    // не с чем.
    state_->stopped.store(true);

    if (state_->keeper.joinable()) {
        state_->keeper.join();
    }

    // Дальше порядок обратный сборке и обязателен: сперва перестаём рисовать,
    // потом закрываем страницу. Наоборот — значит на один кадр остаться с
    // текстурой, собранной из освобождённой памяти.
    state_->hook.reset();
    state_->browser.reset();

    // Chromium при этом не останавливается: остановленный, он не поднимается
    // заново, а модуль вправе пережить не одну сессию. Уйдёт он вместе с
    // процессом игры.
}

} // namespace oxymp::client::game
