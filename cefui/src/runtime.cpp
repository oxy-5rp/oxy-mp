#include <oxymp/cefui/browser.hpp>

#include "app.hpp"
#include "ui_scheme.hpp"

#include <include/cef_app.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>

#include <windows.h>

namespace oxymp::cefui {
namespace {

std::atomic<bool> g_started{false};

} // namespace

bool startRuntime(const std::wstring& root, std::string& error) {
    if (g_started.load()) {
        return true;
    }

    const std::filesystem::path directory{root};
    const std::filesystem::path library = directory / "libcef.dll";

    std::error_code ec;
    if (!std::filesystem::exists(library, ec)) {
        error = "не найден " + library.string();
        return false;
    }

    // Загрузка вручную и до первого вызова — обязательное условие. Клиент
    // внедрён в чужой процесс, и Windows искала бы libcef.dll рядом с GTA5.exe,
    // то есть в папке игры, куда oxyMP не кладёт ничего. Отложенная загрузка,
    // заданная при сборке, подхватит уже загруженный модуль по имени.
    //
    // LOAD_WITH_ALTERED_SEARCH_PATH: у libcef.dll есть свои соседи —
    // chrome_elf.dll и прочие, — и искать их нужно там же, где лежит она сама,
    // а не рядом с игрой.
    if (::LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr) {
        error = "не удалось загрузить libcef.dll, ошибка " + std::to_string(::GetLastError());
        return false;
    }

    CefMainArgs args{::GetModuleHandleW(nullptr)};

    CefSettings settings;
    settings.no_sandbox = 1;

    // Отрисовка в память. Ради неё всё и затевалось: с этим признаком CEF
    // работает в стиле Alloy и отдаёт кадр в OnPaint, а не в окно.
    settings.windowless_rendering_enabled = 1;

    // Свой поток сообщений у Chromium. Иначе его очередь пришлось бы разбирать
    // нам — из кадра игры, который не имеет права ждать чужой механизм.
    settings.multi_threaded_message_loop = 1;

    // Своих окон CEF не заводит вовсе, и уж тем более не должен спрашивать
    // разрешения у оболочки Windows.
    settings.command_line_args_disabled = 0;

    const std::filesystem::path subprocess = directory / "oxymp-cefsub.exe";
    const std::filesystem::path cache = directory / "cache";
    const std::filesystem::path logFile = directory.parent_path() / "logs" / "cef.log";

    std::filesystem::create_directories(cache, ec);
    std::filesystem::create_directories(logFile.parent_path(), ec);

    CefString(&settings.browser_subprocess_path).FromWString(subprocess.wstring());
    CefString(&settings.root_cache_path).FromWString(cache.wstring());
    CefString(&settings.cache_path).FromWString(cache.wstring());
    CefString(&settings.resources_dir_path).FromWString(directory.wstring());
    CefString(&settings.locales_dir_path).FromWString((directory / "locales").wstring());
    CefString(&settings.log_file).FromWString(logFile.wstring());

    settings.log_severity = LOGSEVERITY_WARNING;

    if (!::CefInitialize(args, settings, new App{}, nullptr)) {
        error = "CefInitialize отказал";
        return false;
    }

    g_started.store(true);

    // Схема заводится только после того, как CEF поднят: до этого регистрировать
    // её негде.
    //
    // Каталог страницы — соседний с каталогом CEF, а не внутри него: чужое
    // хозяйство и наше лежат порознь, и обновление CEF не должно уносить с собой
    // меню.
    registerUiScheme(directory.parent_path() / "ui");

    spdlog::debug("CEF поднят: {}", directory.string());
    return true;
}

void stopRuntime() {
    if (!g_started.exchange(false)) {
        return;
    }

    ::CefShutdown();
    spdlog::info("CEF остановлен");
}

} // namespace oxymp::cefui
