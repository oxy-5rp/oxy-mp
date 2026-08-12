#include "ui_layer.hpp"

#include "overlay_renderer.hpp"
#include "present_hook.hpp"
#include "window.hpp"

#include "../ui_feed.hpp"

#include <oxymp/cefui/browser.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "hud_page.hpp"

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
};

std::unique_ptr<UiLayer> UiLayer::create(UiFeed& feed, std::string& error) {
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

    state.browser->show(ui::hudPage);

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

    spdlog::info("игровой интерфейс поднят внутри кадра");
    return layer;
}

void UiLayer::pump() {
    if (state_ == nullptr || state_->browser == nullptr) {
        return;
    }

    // Размер страницы держится равным размеру кадра игры. Спрашивается он у
    // окна, а не у swapchain: до окна отсюда дотянуться можно, а до swapchain —
    // только из потока отрисовки, где заниматься этим незачем.
    if (const HWND game = Window::findOwnWindow(); game != nullptr) {
        RECT bounds{};
        ::GetClientRect(game, &bounds);

        const int width = std::min(static_cast<int>(bounds.right - bounds.left), kMaxWidth);
        const int height = std::min(static_cast<int>(bounds.bottom - bounds.top), kMaxHeight);

        if (width > 0 && height > 0 && (width != state_->width || height != state_->height)) {
            state_->width = width;
            state_->height = height;

            state_->browser->resize(width, height);
            spdlog::info("страница интерфейса подогнана: {}x{}", width, height);
        }
    }

    state_->browser->post(state_->feed->takeUpdate());
}

UiLayer::~UiLayer() {
    if (state_ == nullptr) {
        return;
    }

    // Порядок обратный сборке и обязателен: сперва перестаём рисовать, потом
    // закрываем страницу. Наоборот — значит на один кадр остаться с текстурой,
    // собранной из освобождённой памяти.
    state_->hook.reset();
    state_->browser.reset();

    // Chromium при этом не останавливается: остановленный, он не поднимается
    // заново, а модуль вправе пережить не одну сессию. Уйдёт он вместе с
    // процессом игры.
}

} // namespace oxymp::client::game
