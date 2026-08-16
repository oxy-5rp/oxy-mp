#include "ui_layer.hpp"

#include "environment.hpp"
#include "menu_input.hpp"
#include "overlay_renderer.hpp"
#include "overlay.hpp"
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
#include <functional>
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

/// Как часто поток разбирает свою очередь сообщений.
///
/// Пять миллисекунд — заметно меньше того предела, после которого Windows
/// считает перехват клавиатуры неотвечающим и снимает его.
constexpr auto kPumpInterval = std::chrono::milliseconds{5};

/// Сколько таких долей укладывается в один оборот состояния.
constexpr int kPumpsPerUpdate = 10;

/// Каталог с хозяйством CEF.
///
/// Лежит рядом с самим модулем, а не рядом с исполняемым файлом: исполняемый
/// файл здесь — GTA5.exe, и её папки oxyMP не касается вовсе.
std::filesystem::path cefDirectory() {
    const std::filesystem::path directory = clientDirectory();
    return directory.empty() ? std::filesystem::path{} : directory / "cef";
}

/// Номер ресурса, под которым в модуль встроена страница меню.
///
/// Единица — то же число, что стоит в menu_page.rc.in. Их два, и разъехаться им
/// нельзя: ресурс, которого нет, ищется молча и находится пустым.
constexpr int kMenuPageResource = 1;

/// Достаёт страницу меню из своего же модуля.
///
/// Страница лежит ресурсом, а не файлом рядом с клиентом: файл рядом можно
/// потерять при переносе, подменить или забыть положить, и тогда меню окажется
/// пустым — а понять почему, глядя на пустой экран, нельзя.
///
/// Копия делается сразу: ресурс живёт, пока загружен модуль, но отдавать наружу
/// вид на чужую память ради экономии четырёх мегабайт один раз за запуск —
/// плохой размен.
std::string menuPage() {
    HMODULE self = nullptr;

    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&menuPage), &self) == 0) {
        return {};
    }

    // RT_RCDATA — число, обёрнутое макросом для однобайтовых строк; широкому
    // поиску нужно то же число, но своим макросом.
    const HRSRC found = ::FindResourceW(self, MAKEINTRESOURCEW(kMenuPageResource),
                                        MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (found == nullptr) {
        return {};
    }

    const DWORD size = ::SizeofResource(self, found);
    const HGLOBAL loaded = ::LoadResource(self, found);

    if (size == 0 || loaded == nullptr) {
        return {};
    }

    const void* const bytes = ::LockResource(loaded);
    if (bytes == nullptr) {
        return {};
    }

    return std::string{static_cast<const char*>(bytes), size};
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

    /// Выход из игры. Копия того же, что ушло меню: по Alt+F4 выходят тем же
    /// путём, что и по кнопке в меню, а перехват ввода про меню не знает.
    std::function<void()> quit;

    /// Перехват ввода для меню.
    ///
    /// Ставится не сразу: окна игры в мгновение, когда поднимается слой, ещё
    /// нет — до него остаются секунды. Поэтому его дожидается поток, ведущий
    /// страницы, и ставит перехват первым же кадром, когда окно нашлось.
    std::unique_ptr<MenuInput> input;

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

    /// Нужен ли странице ввод прямо сейчас.
    ///
    /// Не то же самое, что «открыто меню»: консоль страницы живёт поверх меню и
    /// вне его — открытая при закрытом меню, она всё равно принимает набор.
    [[nodiscard]] bool pageWantsInput() const {
        return menu != nullptr && (menu->opened() || menu->consoleOpen());
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
            game, *menuSurface.browser, [this] { return pageWantsInput(); },
            [this] { menu->toggle(); }, [this] { menu->toggleConsole(); }, quit, error);

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

    void keep() {
        while (!stopped.load()) {
            fitToWindow();
            catchInput();

            // Про открытое меню знают все, кому это важно: свой слой по нему
            // прячется, игровая сессия по нему отбирает у игры ввод.
            feed->setMenuOpen(pageWantsInput());

            overlay.browser->post(feed->takeUpdate());

            // Журнал уходит в консоль страницы. Своей консоли у клиента больше
            // нет: у страницы alt:V она есть, умеет больше и открывается той же
            // клавишей.
            //
            // Не раньше, чем страница объявит себя готовой: обработчики она
            // заводит, пока строит свои хранилища, и присланное до этого уходит
            // в никуда. Накопленное подождёт — журнал держит у себя четыреста
            // последних строк, а до готовности их набирается около восьмидесяти.
            if (menu != nullptr && menu->pageReady()) {
                for (const UiFeed::Line& line : feed->takeConsole()) {
                    menu->pushLog(line.kind, line.text);
                }
            }

            // Ожидание разбито на короткие доли, и между ними разбирается
            // очередь сообщений. Это не украшение: низкоуровневый перехват
            // клавиатуры Windows зовёт в том потоке, который его поставил, и
            // зовёт через его очередь. Поток, спящий пятьдесят миллисекунд
            // подряд, отвечал бы на нажатия с той же задержкой, а не ответивший
            // вовремя перехват Windows снимает вовсе.
            for (int i = 0; i < kPumpsPerUpdate && !stopped.load(); ++i) {
                MSG message{};
                while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
                    ::TranslateMessage(&message);
                    ::DispatchMessageW(&message);
                }

                std::this_thread::sleep_for(kPumpInterval);
            }
        }
    }
};

std::unique_ptr<UiLayer> UiLayer::create(UiFeed& feed, Menu::Actions actions, std::string& error) {
    const std::filesystem::path directory = cefDirectory();
    if (directory.empty()) {
        error = "не удалось найти каталог с хозяйством CEF";
        return nullptr;
    }

    if (!oxymp::cefui::startRuntime(directory.wstring(), menuPage(), error)) {
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

    state.overlay.browser->show(std::string{ui::overlayPage});

    // Меню — вторая страница, и её отсутствие не отменяет первую: экран загрузки
    // и чат должны работать даже тогда, когда меню не завелось.
    std::string menuError;

    state.menuSurface.browser = oxymp::cefui::Browser::create(
        kInitialWidth, kInitialHeight,
        [&state](const std::uint8_t* pixels, int width, int height) {
            state.menuSurface.accept(pixels, width, height);
        },
        menuError);

    // Выход берётся себе до того, как остальное уедет в меню: по Alt+F4 выходить
    // нужно тем же путём, что и по кнопке, а перехват ввода к меню не обращается.
    state.quit = actions.quit;

    if (state.menuSurface.browser != nullptr) {
        state.menu = Menu::create(*state.menuSurface.browser, directory.parent_path(),
                                  std::move(actions));
    } else {
        spdlog::error("меню не поднялось: {}", menuError);
    }

    state.hook = PresentHook::install(
        [&state](IDXGISwapChain* swapchain) {
            const bool menuOpen = state.menu != nullptr && state.menu->opened();

            // Пока открыто меню, свой экран загрузки не рисуется вовсе.
            //
            // Он рисовался и просвечивал сквозь меню — два разных экрана
            // загрузки друг под другом. Показывать их вместе незачем и нечем:
            // ход подключения меню рассказывает само, своей строкой, — «идёт
            // подключение», «ресурсы», «входим в игру». Наш экран остаётся для
            // того времени, когда меню закрыто: чат, консоль и разрыв связи.
            if (!menuOpen) {
                state.overlay.draw(swapchain);
            }

            // Меню рисуется вторым, то есть поверх: открытое, оно закрывает
            // собой всё.
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
