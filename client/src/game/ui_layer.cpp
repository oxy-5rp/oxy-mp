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

    // --- Окна ресурсов --------------------------------------------------------

    /// Одно окно игрового режима.
    ///
    /// Своя страница и своя поверхность у каждого: два окна ресурса — это две
    /// независимые страницы, и складывать их в одну значило бы решать за режим,
    /// какая из них поверх.
    struct ResourceView {
        std::uint32_t id = 0;
        std::string url;
        Surface surface;

        /// Показывать ли. Заведённое окно показывается сразу — так же ведёт себя
        /// и alt:V: `new alt.WebView(...)` появляется на экране без просьбы.
        bool visible = true;

        /// Берёт ли оно ввод. По умолчанию нет: страница, молча забравшая
        /// клавиатуру, оставила бы игрока без управления и без объяснения.
        bool focused = false;

        /// Приходил ли от страницы хоть один кадр. Только ради строки в журнал.
        bool painted = false;

        /// Было ли в прошлом кадре хоть что-то видимое. По перемене этого
        /// признака и говорится в журнал: каждый кадр говорить о том же незачем.
        bool hadContent = false;
    };

    /// Просьба, отложенная до потока, ведущего страницы.
    ///
    /// Просьбы копятся, а не исполняются на месте, и это не усложнение ради
    /// стройности. Зовут их из игрового потока — оттуда, где идёт кадр, — а
    /// заводить браузер CEF означает дождаться чужого процесса. Кадр игры не
    /// имеет права ждать: он длится шестнадцать миллисекунд.
    struct ViewRequest {
        enum class Kind { Create, Destroy, Emit, Show, Focus } kind = Kind::Create;

        std::uint32_t view = 0;
        std::string first;
        std::string second;
        bool flag = false;
    };

    std::mutex viewsMutex;
    std::vector<ViewRequest> viewRequests;

    /// Живые окна. Трогает их только поток, ведущий страницы, и поток отрисовки
    /// — последний лишь читает, под той же блокировкой.
    std::vector<std::unique_ptr<ResourceView>> views;

    /// Номера выдаются подряд и не переиспользуются: ресурс мог запомнить номер
    /// закрытого окна, и выданный заново он показал бы ему чужую страницу.
    std::atomic<std::uint32_t> nextViewId{1};

    ViewEventHandler onViewEvent;
    GameKeyHandler onGameKey;

    /// Находит окно по номеру. Зовётся под viewsMutex.
    [[nodiscard]] ResourceView* findView(std::uint32_t id) {
        for (const std::unique_ptr<ResourceView>& each : views) {
            if (each->id == id) {
                return each.get();
            }
        }

        return nullptr;
    }

    /// Исполняет накопленные просьбы. Только из потока, ведущего страницы.
    void serveViewRequests() {
        std::vector<ViewRequest> batch;

        {
            const std::lock_guard guard{viewsMutex};
            batch.swap(viewRequests);
        }

        for (ViewRequest& request : batch) {
            switch (request.kind) {
            case ViewRequest::Kind::Create: {
                auto view = std::make_unique<ResourceView>();
                view->id = request.view;
                view->url = std::move(request.first);

                std::string error;
                view->surface.browser = oxymp::cefui::Browser::create(
                    width, height,
                    [kept = view.get()](const std::uint8_t* pixels, int w, int h) {
                        // О первом кадре говорится один раз, и это не
                        // многословие. Страница, которая загрузилась, отработала
                        // и не появилась на экране, выглядит точно так же, как
                        // страница, которую не рисуют вовсе, — а чинить это два
                        // разных места. По этой строке видно, какое из двух.
                        // Считается не сам кадр, а то, есть ли в нём хоть что-то
                        // видимое: страница, залитая прозрачным, приходит такими
                        // же кадрами, как и страница с картинкой, и отличить их
                        // снаружи больше нечем. Пустая страница — это её
                        // собственное решение, а не наша потеря, и по этой
                        // строке видно, чьё именно.
                        std::size_t opaque = 0;

                        for (std::size_t at = 3; at < static_cast<std::size_t>(w) *
                                                       static_cast<std::size_t>(h) * 4;
                             at += 4) {
                            opaque += pixels[at] != 0 ? 1 : 0;
                        }

                        if (!kept->painted || (opaque != 0) != kept->hadContent) {
                            kept->painted = true;
                            kept->hadContent = opaque != 0;

                            spdlog::info("страница окна {}: кадр {}x{}, видимых точек {}",
                                         kept->id, w, h, opaque);
                        }

                        kept->surface.accept(pixels, w, h);
                    },
                    error);

                if (view->surface.browser == nullptr) {
                    spdlog::error("окно ресурса \"{}\" не завелось: {}", view->url, error);
                    break;
                }

                // Событие от страницы уходит наверх вместе с номером окна: два
                // окна одного ресурса иначе не различить.
                view->surface.browser->onEvent(
                    [this, id = view->id](std::string_view name, std::string_view arguments) {
                        if (onViewEvent) {
                            onViewEvent(id, name, arguments);
                        }
                    });

                view->surface.browser->open(view->url);

                spdlog::info("окно ресурса открыто: {}", view->url);

                const std::lock_guard guard{viewsMutex};
                views.push_back(std::move(view));
                break;
            }

            case ViewRequest::Kind::Destroy: {
                const std::lock_guard guard{viewsMutex};

                std::erase_if(views, [&](const std::unique_ptr<ResourceView>& each) {
                    return each->id == request.view;
                });
                break;
            }

            case ViewRequest::Kind::Emit: {
                const std::lock_guard guard{viewsMutex};

                if (ResourceView* const view = findView(request.view);
                    view != nullptr && view->surface.browser != nullptr) {
                    view->surface.browser->emit(request.first, request.second);
                }
                break;
            }

            case ViewRequest::Kind::Show: {
                const std::lock_guard guard{viewsMutex};

                if (ResourceView* const view = findView(request.view); view != nullptr) {
                    view->visible = request.flag;
                }
                break;
            }

            case ViewRequest::Kind::Focus: {
                const std::lock_guard guard{viewsMutex};

                if (ResourceView* const view = findView(request.view); view != nullptr) {
                    view->focused = request.flag;

                    if (view->surface.browser != nullptr) {
                        view->surface.browser->setFocus(request.flag);
                    }
                }
                break;
            }
            }
        }
    }

    /// Просит ли ввод хоть одно окно ресурса.
    [[nodiscard]] bool viewWantsInput() {
        const std::lock_guard guard{viewsMutex};

        for (const std::unique_ptr<ResourceView>& each : views) {
            if (each->focused) {
                return true;
            }
        }

        return false;
    }

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

    /// Просит выйти из игры так, как об этом просят у alt:V.
    ///
    /// Не выходит, а спрашивает: по Alt+F4 у него появляется его же окно
    /// подтверждения, то самое, что открывается кнопкой в углу меню. Ответит на
    /// него страница обратно, и вот тогда выйдем.
    ///
    /// Своим путём — только когда спрашивать некому: без страницы игроку иначе
    /// нечем было бы закрыть игру, кроме диспетчера задач.
    void askExit() {
        if (menu != nullptr) {
            menu->askExit();
            return;
        }

        if (quit) {
            quit();
        }
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

        MenuInput::Actions actions;

        actions.wanted = [this] { return pageWantsInput(); };
        actions.toggle = [this] { menu->toggle(); };
        actions.console = [this] { menu->toggleConsole(); };
        actions.askExit = [this] { askExit(); };
        actions.exitNow = quit;

        // Клавиши игры уходят клиентским половинам ресурсов. Обработчик
        // спрашивается на каждое нажатие, а не запоминается: слой поднимается
        // раньше сессии, и в мгновение установки перехвата его может не быть.
        actions.gameKey = [this](unsigned key, bool down) {
            if (onGameKey) {
                onGameKey(key, down);
            }
        };

        input = MenuInput::install(game, *menuSurface.browser, std::move(actions), error);

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
            serveViewRequests();

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
            // Тот же вопрос, что и везде: нужен ли странице ввод. Открытая
            // консоль считается наравне с открытым меню — она живёт на той же
            // странице и точно так же закрывает собой игру.
            const bool menuOpen = state.pageWantsInput();

            // Открытое меню паузы прячет всё, что нарисовали мы.
            //
            // Слой лежит поверх геймплея, но не поверх игры целиком: меню паузы
            // принадлежит ей, и чат с интерфейсом режима поверх него — это то,
            // чем накладка выдаёт себя за накладку. Так же ведут себя RAGE MP и
            // alt:V.
            //
            // Спрашивается признак у самой игры, а не выводится из давности
            // кадра: тик при открытом меню идёт, и вывод по давности однажды уже
            // оказался неверным. Кладёт его сюда скриптовый тик — единственное
            // место, откуда позволено звать нативы, — а поток отрисовки только
            // читает.
            const bool gameMenuOpen = state.feed != nullptr && state.feed->gameMenuOpen();

            // Пока открыто меню, свой экран загрузки не рисуется вовсе.
            //
            // Он рисовался и просвечивал сквозь меню — два разных экрана
            // загрузки друг под другом. Показывать их вместе незачем и нечем:
            // ход подключения меню рассказывает само, своей строкой, — «идёт
            // подключение», «ресурсы», «входим в игру». Наш экран остаётся для
            // того времени, когда меню закрыто: чат, консоль и разрыв связи.
            if (!menuOpen && !gameMenuOpen) {
                state.overlay.draw(swapchain);
            }

            // Окна ресурсов — поверх своего слоя, но под меню.
            //
            // Порядок именно такой: меню принадлежит клиенту и обязано открыться
            // поверх чего угодно, что нарисовал режим. Иначе страница режима,
            // занявшая весь кадр, лишила бы игрока возможности отключиться.
            if (!gameMenuOpen) {
                const std::lock_guard guard{state.viewsMutex};

                for (const std::unique_ptr<State::ResourceView>& view : state.views) {
                    if (view->visible) {
                        view->surface.draw(swapchain);
                    }
                }
            }

            // Меню рисуется последним, то есть поверх всего — и поверх меню
            // паузы тоже. Прятать его под ним нельзя: оно забирает себе ввод
            // целиком, и спрятанное оставило бы игрока перед игровым меню без
            // единого способа вернуться к своему. Спорить им при этом не о чем:
            // пока наше меню открыто, Escape достаётся ему, и игровое меню за
            // ним не открывается вовсе.
            if (state.menuSurface.browser != nullptr) {
                state.menuSurface.draw(swapchain);
            }
        },
        [&state] {
            state.overlay.renderer.releaseFrameResources();
            state.menuSurface.renderer.releaseFrameResources();

            // Окна ресурсов держат такие же текстуры, и пережить пересоздание
            // цепочки показа они не могут: собранная из освобождённой памяти,
            // она даёт вылет на первом же кадре.
            const std::lock_guard guard{state.viewsMutex};

            for (const std::unique_ptr<State::ResourceView>& view : state.views) {
                view->surface.renderer.releaseFrameResources();
            }
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

std::uint32_t UiLayer::createView(std::string url) {
    if (state_ == nullptr) {
        return 0;
    }

    // Номер выдаётся здесь и сразу, а окно заводится потом: ресурс подписывается
    // на события окна той же строкой, которой его завёл, и ждать ему нечего.
    const std::uint32_t id = state_->nextViewId.fetch_add(1);

    const std::lock_guard guard{state_->viewsMutex};

    state_->viewRequests.push_back(State::ViewRequest{
        .kind = State::ViewRequest::Kind::Create, .view = id, .first = std::move(url)});

    return id;
}

void UiLayer::destroyView(std::uint32_t view) {
    if (state_ == nullptr) {
        return;
    }

    const std::lock_guard guard{state_->viewsMutex};

    state_->viewRequests.push_back(
        State::ViewRequest{.kind = State::ViewRequest::Kind::Destroy, .view = view});
}

void UiLayer::emitView(std::uint32_t view, std::string name, std::string arguments) {
    if (state_ == nullptr) {
        return;
    }

    const std::lock_guard guard{state_->viewsMutex};

    state_->viewRequests.push_back(State::ViewRequest{.kind = State::ViewRequest::Kind::Emit,
                                                      .view = view,
                                                      .first = std::move(name),
                                                      .second = std::move(arguments)});
}

void UiLayer::showView(std::uint32_t view, bool visible) {
    if (state_ == nullptr) {
        return;
    }

    const std::lock_guard guard{state_->viewsMutex};

    state_->viewRequests.push_back(State::ViewRequest{
        .kind = State::ViewRequest::Kind::Show, .view = view, .flag = visible});
}

void UiLayer::focusView(std::uint32_t view, bool focused) {
    if (state_ == nullptr) {
        return;
    }

    const std::lock_guard guard{state_->viewsMutex};

    state_->viewRequests.push_back(State::ViewRequest{
        .kind = State::ViewRequest::Kind::Focus, .view = view, .flag = focused});
}

void UiLayer::onGameKey(GameKeyHandler handler) {
    if (state_ != nullptr) {
        state_->onGameKey = std::move(handler);
    }
}

void UiLayer::onViewEvent(ViewEventHandler handler) {
    if (state_ != nullptr) {
        state_->onViewEvent = std::move(handler);
    }
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

    // Окна ресурсов уходят первыми: их страницы завёл поднявшийся ресурс, и
    // пережить его им незачем.
    {
        const std::lock_guard guard{state_->viewsMutex};
        state_->views.clear();
    }

    // Меню уходит раньше своей страницы: оно на неё ссылается.
    state_->menu.reset();

    state_->menuSurface.browser.reset();
    state_->overlay.browser.reset();

    // Chromium при этом не останавливается: остановленный, он не поднимается
    // заново, а модуль вправе пережить не одну сессию. Уйдёт он вместе с
    // процессом игры.
}

} // namespace oxymp::client::game
