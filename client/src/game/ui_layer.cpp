#include "ui_layer.hpp"

#include "menu_input.hpp"
#include "overlay_renderer.hpp"
#include "page_source.hpp"
#include "present_hook.hpp"
#include "window.hpp"

#include "../menu.hpp"
#include "../ui_feed.hpp"

#include <oxymp/cefui/browser.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
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

/// Одна страница в кадре игры: её точки и то, чем они рисуются.
///
/// Заведено потому, что страниц стало две. Первая — слой загрузки, чата и худа;
/// вторая — меню. Общего кадра у них быть не может: каждая рисует своё и
/// обновляется в свой срок, а сложить их в один буфер значило бы складывать
/// прозрачности вручную там, где это умеет видеокарта.
struct Surface {
    std::unique_ptr<oxymp::cefui::Browser> browser;
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

    /// Принимает кадр от CEF. Зовётся из его потока.
    void accept(const std::uint8_t* pixels, int width, int height) {
        const std::size_t bytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;

        const std::lock_guard guard{mutex};

        incoming.resize(bytes);
        std::memcpy(incoming.data(), pixels, bytes);

        incomingWidth = width;
        incomingHeight = height;
        fresh = true;
    }

    /// Выводит последний кадр поверх кадра игры. Только из потока отрисовки.
    void draw(IDXGISwapChain* swapchain) {
        bool changed = false;

        {
            const std::lock_guard guard{mutex};

            if (fresh) {
                // Обмен, а не копия: буфер занимает до восьми мегабайт, и
                // копировать их в потоке отрисовки игры значило бы отнимать у
                // кадра миллисекунду на ровном месте.
                shown.swap(incoming);
                shownWidth = incomingWidth;
                shownHeight = incomingHeight;

                fresh = false;
                changed = true;
            }
        }

        renderer.draw(swapchain, shown.data(), shownWidth, shownHeight, changed);
    }
};

/// Всё хозяйство слоя.
///
/// Вынесено из заголовка целиком: иначе всякий, кто подключит ui_layer.hpp,
/// получил бы следом Direct3D и половину Chromium.
struct UiLayer::State {
    UiFeed* feed = nullptr;

    std::unique_ptr<PresentHook> hook;

    /// Слой загрузки, чата и худа — тот, что был здесь всегда.
    Surface overlay;

    /// Меню. Рисуется поверх первого: пока оно открыто, оно закрывает собой всё.
    Surface menuSurface;

    std::unique_ptr<Menu> menu;

    /// Перехват ввода для меню.
    ///
    /// Ставится не сразу: окна игры в мгновение, когда поднимается слой, ещё
    /// нет — до него остаются секунды. Поэтому его дожидается поток, ведущий
    /// страницы, и ставит перехват первым же кадром, когда окно нашлось.
    std::unique_ptr<MenuInput> input;

    /// Сказали ли уже меню, что игрок в игре. Один раз за запуск: второе такое
    /// слово ничего не меняет, а слать его двадцать раз в секунду незачем.
    bool toldConnected = false;

    /// Размер, под который подогнаны страницы.
    int width = kInitialWidth;
    int height = kInitialHeight;

    /// Поток, который ведёт страницу: подгоняет её под размер кадра и отдаёт ей
    /// состояние. Свой, а не чужой, потому что чужие заняты: поток игры рисует
    /// кадр, а сетевой минуту дожидается готовности движка.
    std::thread keeper;
    std::atomic<bool> stopped{false};

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

        overlay.browser->resize(wanted, high);

        if (menuSurface.browser != nullptr) {
            menuSurface.browser->resize(wanted, high);
        }

        spdlog::debug("страница интерфейса подогнана: {}x{}", wanted, high);
    }

    /// Ставит перехват ввода, как только окно игры появилось.
    void catchInput() {
        if (input != nullptr || menu == nullptr || menuSurface.browser == nullptr) {
            return;
        }

        const HWND game = Window::findOwnWindow();
        if (game == nullptr) {
            return;
        }

        std::string error;

        input = MenuInput::install(
            game, *menuSurface.browser, [this] { return menu->opened(); },
            [this] { menu->toggle(); }, error);

        if (input == nullptr) {
            spdlog::error("ввод для меню не перехвачен, меню убрано: {}", error);

            // Меню убирается целиком, а не остаётся показанным. Второе было бы
            // хуже отсутствия: страница закрывает собой весь кадр, и не имея
            // ввода, игрок не смог бы её ни закрыть, ни обойти.
            //
            // Второй попытки не будет и по другой причине: окно мы уже нашли,
            // значит отказ не в нём, и повторять его двадцать раз в секунду
            // значило бы завалить журнал одной строкой.
            menu.reset();
            menuSurface.browser.reset();
        }
    }

    /// Говорит меню, что игрок в игре, — и оно убирается с экрана.
    ///
    /// Пока страница считает себя неподключённой, она держит себя открытой
    /// поверх всего, и никакое переключение её не свернёт: так она устроена, и
    /// так же ведёт себя alt:V. Слово «подключились» для неё — единственный
    /// способ уйти.
    ///
    /// Мгновение выбрано то же, по которому уходит экран загрузки: игрок в мире,
    /// и с сервером всё решилось.
    void tellConnected() {
        if (menu == nullptr || toldConnected || !feed->ready()) {
            return;
        }

        menu->connected();
        toldConnected = true;
    }

    void keep() {
        while (!stopped.load()) {
            fitToWindow();
            catchInput();
            tellConnected();
            overlay.browser->post(feed->takeUpdate());

            std::this_thread::sleep_for(kStateInterval);
        }
    }
};

std::unique_ptr<UiLayer> UiLayer::create(UiFeed& feed, Menu::Actions actions, std::string& error) {
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

    state.overlay.browser = oxymp::cefui::Browser::create(
        kInitialWidth, kInitialHeight,
        [&state](const std::uint8_t* pixels, int width, int height) {
            state.overlay.accept(pixels, width, height);
        },
        error);

    if (state.overlay.browser == nullptr) {
        return nullptr;
    }

    state.overlay.browser->show(composePage());

    // Меню — вторая страница, и её отсутствие не отменяет первую: экран загрузки
    // и чат должны работать даже тогда, когда меню не завелось.
    std::string menuError;

    state.menuSurface.browser = oxymp::cefui::Browser::create(
        kInitialWidth, kInitialHeight,
        [&state](const std::uint8_t* pixels, int width, int height) {
            state.menuSurface.accept(pixels, width, height);
        },
        menuError);

    if (state.menuSurface.browser != nullptr) {
        state.menu = Menu::create(*state.menuSurface.browser, directory.parent_path(),
                                  std::move(actions));
    } else {
        spdlog::error("меню не поднялось: {}", menuError);
    }

    state.hook = PresentHook::install(
        [&state](IDXGISwapChain* swapchain) {
            state.overlay.draw(swapchain);

            // Меню рисуется вторым, то есть поверх: открытое, оно закрывает
            // собой всё, включая экран загрузки.
            if (state.menuSurface.browser != nullptr) {
                state.menuSurface.draw(swapchain);
            }
        },
        [&state] {
            state.overlay.renderer.releaseFrameResources();
            state.menuSurface.renderer.releaseFrameResources();
        },
        error);

    if (state.hook == nullptr) {
        return nullptr;
    }

    // Поток заводится последним: до этого мгновения ему нечего вести.
    state.keeper = std::thread{[&state] { state.keep(); }};

    spdlog::info("игровой интерфейс поднят внутри кадра");
    return layer;
}

Menu* UiLayer::menu() const noexcept {
    return state_ == nullptr ? nullptr : state_->menu.get();
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
    // потом закрываем страницы. Наоборот — значит на один кадр остаться с
    // текстурой, собранной из освобождённой памяти.
    state_->hook.reset();

    // Перехват ввода снимается раньше всего остального: он ссылается и на
    // страницу меню, и на само меню, и работает в потоке окна игры — то есть в
    // чужом, который о нашем разрушении не знает.
    state_->input.reset();

    // Меню уходит раньше своей страницы: оно на неё ссылается.
    state_->menu.reset();

    state_->menuSurface.browser.reset();
    state_->overlay.browser.reset();

    // Chromium при этом не останавливается: остановленный, он не поднимается
    // заново, а модуль вправе пережить не одну сессию. Уйдёт он вместе с
    // процессом игры.
}

} // namespace oxymp::client::game
