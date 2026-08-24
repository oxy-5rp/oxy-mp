#include "setup_window.hpp"

#include "game_choice.hpp"
#include "game_store.hpp"
#include "page_text.hpp"
#include "skin_assets.hpp"
#include "text.hpp"

#include <oxymp/gamesig/catalog.hpp>
#include <oxymp/webui/browser.hpp>

#include <spdlog/spdlog.h>

#include <charconv>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include <shlobj.h>
#include <wrl/client.h>

#include "setup_page.hpp"

namespace oxymp::launcher {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kWindowClass = L"oxyMPSetup";

constexpr const wchar_t* kDefaultTitle = L"oxyMP";

constexpr const wchar_t* kExecutableName = L"GTA5.exe";

/// Сообщение «человек нажал строку „выберу сам“».
///
/// Окно выбора каталога открывается из обработчика окна, а не прямо из
/// обработчика сообщения страницы, и это не педантизм: то вызывается движком
/// интерфейса изнутри его собственной работы, а окно выбора каталога — модальное
/// и крутит свой цикл сообщений. Открытое оттуда, оно крутит его внутри чужого
/// вызова.
constexpr UINT kBrowseMessage = WM_APP + 1;

/// Сообщение «поиск установленных копий закончился».
///
/// LPARAM — список найденного. Поиск идёт в своём потоке: он перебирает список
/// Rockstar, три значения реестра, библиотеки Steam и описи Epic, и на медленном
/// диске это заметно. Окно всё это время обязано быть на экране.
constexpr UINT kFoundMessage = WM_APP + 2;

/// Строка списка.
struct Entry {
    /// Чем строка называется: площадкой или словами «свой каталог».
    std::string name;

    std::filesystem::path directory;
    GameStore store = GameStore::Rockstar;

    /// Строка «выберу сам». Пути у неё нет и выбранной она не бывает.
    bool browse = false;
};

/// Состояние окна. Одно на процесс: окон выбора каталога не бывает двух.
struct Window {
    HWND handle = nullptr;
    std::unique_ptr<webui::Browser> browser;

    /// Что нашлось само.
    std::vector<GameLocation> found;

    /// Что человек указал руками. Пусто, пока не указывал.
    std::optional<GameLocation> custom;

    /// Какая строка выбрана. Отрицательное — не выбрана ни одна.
    int selected = -1;

    /// Выбрал ли человек площадку сам.
    ///
    /// Пока не выбрал, площадка идёт следом за строкой списка: у найденной копии
    /// она известна из её же каталога. После первого нажатия по площадке
    /// подстановка прекращается, иначе выбор человека стирался бы следующим же
    /// нажатием по списку.
    bool storePicked = false;

    GameStore store = GameStore::Rockstar;

    /// Что сказать человеку под списком и каким цветом.
    std::string note;
    std::string kind;

    /// Закончился ли поиск. До этого мгновения пустой список означает не «игры
    /// нет», а «её ещё ищут», и говорить «не нашли» рано.
    bool searched = false;

    /// Согласился ли человек. Ложь означает, что окно закрыли.
    bool accepted = false;

    std::thread search;
};

Window* g_window = nullptr;

/// Строки списка в том порядке, в каком их видит человек.
std::vector<Entry> entries() {
    std::vector<Entry> rows;

    if (g_window == nullptr) {
        return rows;
    }

    for (const GameLocation& game : g_window->found) {
        rows.push_back(Entry{std::string{storeName(game.store)}, game.directory, game.store, false});
    }

    if (g_window->custom) {
        rows.push_back(Entry{text::kAlternativeDirectory, g_window->custom->directory,
                             g_window->custom->store, false});
    }

    rows.push_back(Entry{text::kChooseLocationMyself, {}, GameStore::Rockstar, true});

    return rows;
}

/// Выбранная строка. Пусто, если не выбрана ни одна.
const Entry* chosen(const std::vector<Entry>& rows) {
    if (g_window == nullptr || g_window->selected < 0 ||
        static_cast<std::size_t>(g_window->selected) >= rows.size()) {
        return nullptr;
    }

    const Entry& row = rows[static_cast<std::size_t>(g_window->selected)];
    return row.browse ? nullptr : &row;
}

/// Рассказывает странице всё, что известно о выборе.
void publish() {
    if (g_window == nullptr || !g_window->browser) {
        return;
    }

    const std::vector<Entry> rows = entries();

    std::string items;
    for (const Entry& row : rows) {
        if (!items.empty()) {
            items += ',';
        }

        items += std::format(R"({{"name":"{}","path":"{}","browse":{}}})", escapeJson(row.name),
                             escapeJson(row.directory.string()), row.browse ? "true" : "false");
    }

    // Приглашение показывается ровно тогда, когда искать больше нечего и не
    // нашлось ничего: над непустым списком оно было бы неправдой.
    const char* lead =
        g_window->searched && g_window->found.empty() ? text::kUnableToFindInstallation : "";

    g_window->browser->post(std::format(
        R"({{"action":"games","items":[{}],"selected":{},"platform":"{}","lead":"{}",)"
        R"("note":"{}","kind":"{}","ok":{}}})",
        items, g_window->selected, storeKey(g_window->store), escapeJson(lead),
        escapeJson(g_window->note), g_window->kind, chosen(rows) != nullptr ? "true" : "false"));
}

/// Берёт строку списка выбранной и говорит о ней всё, что стоит сказать.
///
/// Отказы названы словами alt:V: человек, у которого что-то не так, ищет строку
/// в поиске, и своя формулировка того же самого не находит ничего.
void examine(int index) {
    if (g_window == nullptr) {
        return;
    }

    g_window->selected = index;
    g_window->note.clear();
    g_window->kind.clear();

    const std::vector<Entry> rows = entries();

    const Entry* row = chosen(rows);
    if (row == nullptr) {
        return;
    }

    if (!g_window->storePicked) {
        g_window->store = row->store;
    }

    // Версия сверяется здесь, а не только перед запуском, и это не двойная
    // работа: человек с игрой другой сборки узнаёт об этом в то самое мгновение,
    // когда указывает на неё, а не через минуту ожидания.
    //
    // Продолжить ему при этом не запрещают: путь всё равно нужно записать, иначе
    // окно будет спрашивать одно и то же при каждом запуске, — а полный разбор
    // случившегося выдаст запуск.
    const std::string version = readGameVersion(row->directory / kExecutableName);

    if (!version.empty() && version != gamesig::kTargetGameVersion) {
        g_window->note = std::format("{}: {}, oxyMP is built for {}", text::kErrGameOutdated,
                                     version, gamesig::kTargetGameVersion);
        g_window->kind = "bad";
    }
}

/// Спрашивает каталог у Windows. Пусто — человек отказался.
///
/// Через IFileOpenDialog, а не через SHBrowseForFolder: второе — окно времён
/// Windows 2000, с деревом на пол-экрана и без строки пути, и вставить в него
/// путь из буфера обмена нельзя вовсе.
std::filesystem::path askForDirectory(HWND owner, const std::filesystem::path& startAt) {
    ComPtr<IFileOpenDialog> dialog;

    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog)))) {
        spdlog::error("the folder dialog could not be created");
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    dialog->SetTitle(widen(text::kSelectGtavFolder).c_str());

    // Начинать с того, что уже нашлось: человеку останется подтвердить, а не
    // разыскивать заново.
    if (!startAt.empty()) {
        ComPtr<IShellItem> item;

        if (SUCCEEDED(::SHCreateItemFromParsingName(startAt.c_str(), nullptr,
                                                    IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item.Get());
        }
    }

    if (FAILED(dialog->Show(owner))) {
        // Отказ — не поломка, а решение человека: он передумал.
        return {};
    }

    ComPtr<IShellItem> picked;
    if (FAILED(dialog->GetResult(&picked))) {
        return {};
    }

    PWSTR path = nullptr;
    if (FAILED(picked->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) {
        return {};
    }

    std::filesystem::path chosenPath{path};
    ::CoTaskMemFree(path);

    return chosenPath;
}

/// Принимает каталог, указанный руками.
void takeCustom(const std::filesystem::path& directory) {
    if (g_window == nullptr || directory.empty()) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        g_window->note = text::kErrPathDoesNotExist;
        g_window->kind = "bad";
        return;
    }

    std::string trouble;

    std::optional<GameLocation> location = gameInDirectory(directory, trouble);
    if (!location) {
        spdlog::debug("{}", trouble);

        g_window->custom.reset();
        g_window->selected = -1;
        g_window->note = text::kErrNotGtavPath;
        g_window->kind = "bad";
        return;
    }

    g_window->custom = std::move(location);

    // Своя строка встаёт следом за найденным, и она же сразу выбирается: человек
    // указал каталог затем, чтобы играть с него.
    examine(static_cast<int>(g_window->found.size()));
}

void handlePageMessage(std::string_view json) {
    if (g_window == nullptr || g_window->handle == nullptr) {
        return;
    }

    const std::string action = jsonField(json, "action");

    if (action == "ready") {
        // Страница поднялась и только теперь способна принимать сообщения.
        // Отправленное до этого мгновения не принимает никто, и список от
        // поиска, успевшего закончиться раньше, пропал бы бесследно.
        publish();
        return;
    }

    if (action == "window") {
        const std::string command = jsonField(json, "command");

        if (command == "close") {
            ::PostMessageW(g_window->handle, WM_CLOSE, 0, 0);
        } else if (command == "minimize") {
            ::ShowWindow(g_window->handle, SW_MINIMIZE);
        }

        return;
    }

    if (action == "drag") {
        ::ReleaseCapture();
        ::SendMessageW(g_window->handle, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return;
    }

    if (action == "browse") {
        ::PostMessageW(g_window->handle, kBrowseMessage, 0, 0);
        return;
    }

    if (action == "pick") {
        const std::string value = jsonField(json, "value");

        int index = -1;
        std::from_chars(value.data(), value.data() + value.size(), index);

        examine(index);
        publish();
        return;
    }

    if (action == "platform") {
        g_window->storePicked = true;
        g_window->store = storeFromKey(jsonField(json, "value"));
        publish();
        return;
    }

    if (action == "accept") {
        if (chosen(entries()) == nullptr) {
            return;
        }

        g_window->accepted = true;
        ::PostMessageW(g_window->handle, WM_CLOSE, 0, 0);
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kBrowseMessage: {
        if (g_window == nullptr) {
            return 0;
        }

        const std::vector<Entry> rows = entries();
        const Entry* const current = chosen(rows);

        takeCustom(askForDirectory(
            window, current != nullptr ? current->directory : std::filesystem::path{}));

        publish();
        return 0;
    }

    case kFoundMessage: {
        const std::unique_ptr<std::vector<GameLocation>> found{
            reinterpret_cast<std::vector<GameLocation>*>(lparam)};

        if (g_window == nullptr) {
            return 0;
        }

        g_window->searched = true;

        // Найденное подставляется только тогда, когда человек ещё ничего не
        // указал сам: он мог успеть сделать это, пока мы искали, и перебивать
        // его выбор нашей догадкой нельзя.
        if (found != nullptr && !g_window->custom) {
            g_window->found = *found;

            if (!g_window->found.empty()) {
                examine(0);
            }
        }

        publish();
        return 0;
    }

    case WM_SIZE:
        if (g_window != nullptr && g_window->browser) {
            g_window->browser->resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

std::optional<GameLocation> SetupWindow::ask(const Paths& paths) {
    Window window;
    g_window = &window;

    const SkinAssets skin = SkinAssets::load(paths.root / "skin.bin");

    const std::wstring title = skin.name.empty() ? std::wstring{kDefaultTitle} : widen(skin.name);

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.lpfnWndProc = &windowProcedure;
    description.hInstance = instance;
    description.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    description.hbrBackground = ::CreateSolidBrush(RGB(10, 12, 16));
    description.lpszClassName = kWindowClass;

    if (::RegisterClassExW(&description) == 0) {
        spdlog::error("the setup window class could not be registered");
        g_window = nullptr;
        return std::nullopt;
    }

    const DWORD style = WS_POPUP | WS_CLIPCHILDREN;

    const int width = skin.installerWidth;
    const int height = skin.installerHeight;

    POINT cursor{};
    ::GetCursorPos(&cursor);

    MONITORINFO screen{sizeof(screen)};
    ::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &screen);

    const int left = screen.rcWork.left + ((screen.rcWork.right - screen.rcWork.left) - width) / 2;
    const int top = screen.rcWork.top + ((screen.rcWork.bottom - screen.rcWork.top) - height) / 2;

    window.handle = ::CreateWindowExW(0, kWindowClass, title.c_str(), style, left, top, width,
                                      height, nullptr, nullptr, instance, nullptr);

    if (window.handle == nullptr) {
        spdlog::error("the setup window could not be created");
        g_window = nullptr;
        return std::nullopt;
    }

    if (skin.largeIcon != nullptr) {
        ::SendMessageW(window.handle, WM_SETICON, ICON_BIG,
                       reinterpret_cast<LPARAM>(skin.largeIcon));
    }
    if (skin.smallIcon != nullptr) {
        ::SendMessageW(window.handle, WM_SETICON, ICON_SMALL,
                       reinterpret_cast<LPARAM>(skin.smallIcon));
    }

    std::string error;
    window.browser =
        webui::Browser::create(window.handle, paths.browserCache().wstring(), false, error);

    if (window.browser == nullptr) {
        ::MessageBoxW(window.handle, L"The interface engine could not be started.\n"
                                     L"Install the Microsoft Edge WebView2 Runtime.",
                      title.c_str(), MB_ICONERROR | MB_OK);
        spdlog::error("{}", error);
        g_window = nullptr;
        return std::nullopt;
    }

    std::string page{ui::setupPage};

    page = fillPage(std::move(page), "{{background}}",
                    skin.installerBackground.empty()
                        ? "none"
                        : "url(\"" + skin.installerBackground + "\")");
    page = fillPage(std::move(page), "{{accent}}", skin.accent.empty() ? "#4f8ef7" : skin.accent);
    page = fillPage(std::move(page), "{{title}}", text::kSelectGtavLocation);
    page = fillPage(std::move(page), "{{lead}}", "");
    page = fillPage(std::move(page), "{{location}}", text::kGtavLocation);
    page = fillPage(std::move(page), "{{platform}}", "Platform");
    page = fillPage(std::move(page), "{{version}}", OXYMP_VERSION);
    page = fillPage(std::move(page), "{{confirm}}", text::kConfirm);

    window.browser->onMessage(&handlePageMessage);
    window.browser->show(page);

    ::ShowWindow(window.handle, SW_SHOW);

    // Поиск начинается вместе с окном: пока человек читает, что от него хотят,
    // список обычно уже готов, и нажать останется одну кнопку.
    window.search = std::thread([handle = window.handle] {
        // На кучу, а не на стек: список разберут уже после того, как этот поток
        // уйдёт дальше.
        auto* const carried = new std::vector<GameLocation>{findGames()};

        if (::PostMessageW(handle, kFoundMessage, 0, reinterpret_cast<LPARAM>(carried)) == 0) {
            delete carried;
        }
    });

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    if (window.search.joinable()) {
        // Поиск переживает окно: человек вправе закрыть его раньше, чем тот
        // закончится. Ждать здесь нечего — сообщение уйдёт в никуда.
        window.search.detach();
    }

    // Класс снимается: окно выбора каталога поднимается в том же процессе, что
    // и окно лаунчера, и второй раз оно бывает — когда игру не нашли и человек
    // начинает сначала.
    ::UnregisterClassW(kWindowClass, instance);

    const std::vector<Entry> rows = entries();
    const Entry* const picked = window.accepted ? chosen(rows) : nullptr;

    if (picked == nullptr) {
        g_window = nullptr;
        spdlog::info("The game location was not chosen");
        return std::nullopt;
    }

    GameLocation location;
    location.directory = picked->directory;
    location.executable = picked->directory / kExecutableName;
    location.version = readGameVersion(location.executable);
    location.store = window.store;

    g_window = nullptr;

    rememberGame(paths.root / "oxymp.toml", location);

    return location;
}

} // namespace oxymp::launcher
