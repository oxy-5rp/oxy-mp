#include "menu.hpp"

#include "server_history.hpp"

#include <oxymp/cefui/browser.hpp>
#include <oxymp/config/settings.hpp>
#include <oxymp/config/skin.hpp>

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace oxymp::client {
namespace {

/// Откуда страница берётся.
///
/// Схема своя (`cefui/src/ui_scheme.hpp`) и отдаёт файлы из каталога `ui` рядом с
/// клиентом. Имя то же, что у alt:V: страница собрана их сборкой и ссылается на
/// своё хозяйство так, как её собрали.
constexpr const char* kPageUrl = "http://ui/index.html";

constexpr const char* kSettingsFile = "oxymp.toml";
constexpr const char* kSkinFile = "skin.bin";

/// Куда игрок заходил и что отметил звёздочкой.
constexpr const char* kHistoryFile = "history.servers";

/// Ветвь сборки. Страница показывает её рядом с версией.
constexpr const char* kBranch = "release";

/// Указатель мыши, который страница рисует себе сама.
///
/// Своего указателя у неё быть не может, а чужого не видно: страница рисуется в
/// память, указатель — дело Windows, и в её кадр он не попадает. Игра же прячет
/// системный указатель и делает это не однажды — переспорить её не вышло ни
/// счётчиком показа, ни ответом на просьбу поставить указатель.
///
/// Поэтому он рисуется страницей: стрелка следует за мышью, о которой странице и
/// так рассказывают. Так же поступает alt:V — в его меню указатель тоже нарисован,
/// а не системный.
///
/// Вписывается в окружение страницы, а не в её исходники: страница взята у alt:V
/// и правится дальше по своим правилам, и всякая наша строка в ней — это работа
/// при каждом её обновлении.
constexpr const char* kCursorScript = R"JS(
(() => {
  if (window.__oxyCursor) return;

  const cursor = document.createElement('div');
  cursor.style.cssText = 'position:fixed;left:0;top:0;z-index:2147483647;' +
    'pointer-events:none;will-change:transform;display:none';
  cursor.innerHTML =
    '<svg width="20" height="24" viewBox="0 0 20 24" xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M2 1.5 L2 19 L6.4 15 L9.2 21.4 L12.1 20.1 L9.4 13.9 L15.4 13.6 Z"' +
    ' fill="#fff" stroke="#000" stroke-width="1.4" stroke-linejoin="round"/></svg>';

  document.body.appendChild(cursor);
  window.__oxyCursor = cursor;

  addEventListener('mousemove', (event) => {
    if (!window.__oxyCursorShown) return;
    cursor.style.display = 'block';
    cursor.style.transform = 'translate(' + event.clientX + 'px,' + event.clientY + 'px)';
  }, true);
})();
)JS";

/// Показывает и прячет нарисованный указатель.
///
/// Прятать обязательно, и это выяснено на живой игре. Страница остаётся в кадре
/// и с закрытым меню — она просто не рисует ничего, — а вот наша стрелка рисуется
/// сама по себе и оставалась висеть поверх игры на том месте, где её застало
/// закрытие. Мышь при этом уходит игре, и стрелка даже не двигалась.
constexpr const char* kCursorShow = "window.__oxyCursorShown = true;";
constexpr const char* kCursorHide =
    "window.__oxyCursorShown = false;"
    "if (window.__oxyCursor) window.__oxyCursor.style.display = 'none';";

} // namespace

struct Menu::State {
    cefui::Browser* browser = nullptr;
    std::filesystem::path directory;
    Actions actions;

    /// Настройки правятся из потока CEF, а читаются и оттуда, и из потока игры.
    mutable std::mutex mutex;
    config::Settings settings;
    std::optional<config::Skin> skin;

    /// История серверов. Правится из потока CEF, оттуда же и пишется.
    ServerHistory history;

    /// Открыто ли меню. Читает поток игры — отсюда и счётчик вместо блокировки.
    std::atomic<bool> opened{true};

    /// Сказала ли страница, что готова принимать события.
    std::atomic<bool> ready{false};

    /// Открыта ли консоль страницы. Читается оттуда же и по той же причине.
    ///
    /// Ведётся у нас, а не спрашивается у страницы: открываем её мы, по F8, и
    /// `ui:open` про неё ничего не говорит — она живёт поверх меню и вне его.
    /// Страница сообщает обратно только о том, что закрыла её сама.
    std::atomic<bool> consoleOpen{false};

    /// События страницы, на которые нам нечем ответить.
    ///
    /// Хранятся, чтобы сказать о каждом ровно один раз: страница шлёт их по
    /// нажатию, а нажимают подолгу.
    std::set<std::string, std::less<>> unanswered;

    [[nodiscard]] std::filesystem::path settingsPath() const { return directory / kSettingsFile; }
    [[nodiscard]] std::filesystem::path historyPath() const { return directory / kHistoryFile; }

    /// Показывает или прячет нарисованный указатель.
    ///
    /// Считаются оба признака сразу: указатель нужен и открытому меню, и одной
    /// открытой консоли — в ней есть и кнопки, и выделение текста мышью. Прежде
    /// он смотрел только на меню, и в консоли, открытой поверх игры, мыши не
    /// было видно вовсе.
    void updateCursor() {
        const bool wanted = opened.load() || consoleOpen.load();
        browser->evaluate(wanted ? kCursorShow : kCursorHide);
    }

    /// Отдаёт странице событие с готовым списком доводов.
    ///
    /// Запись с заменой негодных знаков, а не с отказом. Строки сюда приходят из
    /// файла настроек и файла оформления, то есть от человека и от стороннего
    /// средства, и байт, не складывающийся в UTF-8, среди них однажды окажется.
    /// По умолчанию такая строка вызвала бы исключение — а мы внутри чужого
    /// процесса, где неперехваченное исключение означает вылет игры.
    void emit(std::string_view name, const nlohmann::json& arguments) {
        browser->emit(name, arguments.dump(-1, ' ', false,
                                           nlohmann::json::error_handler_t::replace));
    }

    void emit(std::string_view name) { browser->emit(name, "[]"); }

    /// Список недавних серверов, каким его видит страница.
    ///
    /// Отдельно от sendInitialState, потому что спрашивают его дважды: при
    /// загрузке страницы и по кнопке обновления на странице серверов.
    [[nodiscard]] static nlohmann::json toJson(const std::vector<ServerHistory::Entry>& list) {
        nlohmann::json array = nlohmann::json::array();

        for (const ServerHistory::Entry& entry : list) {
            array.push_back(nlohmann::json{
                {"name", entry.name},
                {"id", entry.id},
                {"url", entry.url},
            });
        }

        return array;
    }

    [[nodiscard]] nlohmann::json recentServers() {
        const std::lock_guard guard{mutex};

        // История ведётся сама и лежит в history.servers рядом с клиентом.
        // Прежде «недавним» звался ровно один адрес — `lastip` из настроек, — и
        // список из одной строки был не историей, а её обещанием.
        //
        // Страница разворачивает присланное задом наперёд: она ждёт список от
        // старых к новым, а у нас он от новых к старым. Разворачиваем здесь, а
        // не там: в её исходники мы не пишем.
        std::vector<ServerHistory::Entry> recent = history.recent();
        std::ranges::reverse(recent);

        return toJson(recent);
    }

    /// Отдаёт странице оба её списка серверов: недавние и общий.
    ///
    /// Общий у нас пуст, и это не забытая работа: каталога серверов у oxyMP нет
    /// и не предвидится — некому его вести. Но послать пустой список обязательно,
    /// и это выяснено по исходникам страницы: пока `servers:update` не пришёл,
    /// она держит список «загружающимся» и крутит бесконечное колесо. Пустой
    /// список она показывает честной надписью «серверов нет».
    void sendServers() {
        emit("servers:recent:update", nlohmann::json::array({recentServers()}));
        emit("servers:favorite:update", nlohmann::json::array({favoriteServers()}));

        // Общий список серверов — из оформления, если оно есть, и пустой, если
        // нет. Своего каталога у oxyMP нет и не предвидится: некому его вести.
        // Но послать список обязательно — пока `servers:update` не пришёл,
        // страница держит его «загружающимся» и крутит колесо до конца запуска.
        emit("servers:update", nlohmann::json::array({skinServers()}));
    }

    [[nodiscard]] nlohmann::json favoriteServers() {
        const std::lock_guard guard{mutex};
        return toJson(history.favorite());
    }

    /// Серверы из оформления — тот самый список, что показывает страница.
    [[nodiscard]] nlohmann::json skinServers() {
        const std::lock_guard guard{mutex};

        nlohmann::json array = nlohmann::json::array();
        if (!skin.has_value()) {
            return array;
        }

        for (const config::Skin::Server& server : skin->servers) {
            // Поля те же, что у страницы: она ждёт узел и порт отдельно, а у нас
            // адрес один строкой. Порт отделяется по последнему двоеточию — так
            // же, как его отделяет клиент при подключении.
            std::string host = server.address;
            int port = 0;

            if (const std::size_t colon = host.rfind(':'); colon != std::string::npos) {
                const char* const begin = host.c_str() + colon + 1;
                const char* const end = host.c_str() + host.size();

                // Разбор без исключений и без правки числа при неудаче: адрес
                // приходит из чужого файла оформления, и двоеточие в нём может
                // оказаться не перед портом вовсе.
                if (std::from_chars(begin, end, port).ec == std::errc{}) {
                    host.resize(colon);
                } else {
                    port = 0;
                }
            }

            // Поля перечислены все, какие ждёт страница, и пустые — тоже: она
            // читает их без проверки, и отсутствующее поле превратилось бы у неё
            // в `undefined` посреди разметки.
            array.push_back(nlohmann::json{
                {"id", server.id},
                {"name", server.name},
                {"host", host},
                {"port", port},
                {"players", 0},
                {"maxPlayers", 0},
                {"locked", false},
                {"gameMode", ""},
                {"website", ""},
                {"language", ""},
                {"description", ""},
                {"verified", false},
                {"promoted", false},
                {"useEarlyAuth", false},
                {"earlyAuthUrl", ""},
                {"useCdn", false},
                {"cdnUrl", ""},
                {"useVoiceChat", false},
                {"tags", nlohmann::json::array()},
                {"bannerUrl", ""},
                {"branch", "release"},
                {"build", 0},
                {"version", ""},
                {"lastUpdate", 0},
            });
        }

        return array;
    }

    /// Отдаёт странице перечень устройств записи.
    ///
    /// Он пуст, и будет пуст, пока у oxyMP нет голоса: перечислять нечего.
    /// Промолчать всё же нельзя — не получив ответа, страница держит список
    /// «загружающимся» и ждёт его до конца запуска.
    void sendRecordingDevices() {
        emit("settings:devices:update", nlohmann::json::array({nlohmann::json::object()}));
    }

    /// Рассказывает странице всё, что она должна знать к первому кадру.
    ///
    /// Зовётся по её же слову — `loaded`, — а не сразу после загрузки. Разница
    /// существенная: обработчики она заводит, пока строит свои хранилища, и
    /// присланное до этого ушло бы в никуда.
    void sendInitialState() {
        std::string settingsJson;
        std::string lastAddress;
        std::optional<std::string> manifest;

        {
            const std::lock_guard guard{mutex};

            settingsJson = settings.toJson();
            lastAddress = settings.text("lastip");

            if (skin.has_value()) {
                manifest = skin->toManifestJson();
            }
        }

        emit("version:update",
             nlohmann::json::array({OXYMP_VERSION, kBranch, lastAddress, false, false}));

        // Манифеста может не быть вовсе, и странице об этом говорят прямо: она
        // отличает «оформления нет» от «оформление пустое» и во втором случае
        // закрасила бы фон ничем.
        if (manifest.has_value()) {
            emit("version:setManifest", nlohmann::json::array({*manifest}));
        } else {
            emit("version:setManifest", nlohmann::json::array({nullptr}));
        }

        // Разбор без исключений, хотя строку эту породили мы сами: код живёт
        // внутри чужого процесса, и неперехваченное исключение здесь — это не
        // сообщение об ошибке, а вылет игры.
        const nlohmann::json settingsObject =
            nlohmann::json::parse(settingsJson, nullptr, false, true);

        if (settingsObject.is_object()) {
            emit("settings:update", nlohmann::json::array({settingsObject}));
        } else {
            spdlog::error("settings did not serialise: the page will show its own defaults");
        }

        sendServers();
        sendRecordingDevices();

        // Готовность объявляется последней, и порядок здесь обязателен: по
        // `ui:ready` страница показывает себя и спрашивает имя, если его нет, —
        // а имя приходит настройками строкой выше.
        emit("ui:ready");
        emit("version:ready");

        spdlog::debug("menu is ready");
    }

    /// Разбирает событие от страницы.
    void handle(std::string_view name, std::string_view argumentsJson) {
        const nlohmann::json arguments =
            nlohmann::json::parse(argumentsJson, nullptr, false, true);

        if (!arguments.is_array()) {
            spdlog::debug("событие {} пришло с непонятными доводами", name);
            return;
        }

        if (name == "loaded") {
            ready.store(true);

            // Указатель вписывается по слову страницы, а не по её загрузке: до
            // этого мгновения тела у неё ещё нет, и вписывать его некуда.
            browser->evaluate(kCursorScript);

            sendInitialState();
            return;
        }

        if (name == "ui:open") {
            const bool open = arguments.size() > 0 && arguments[0].is_boolean() &&
                              arguments[0].get<bool>();

            // Записывается каждое такое слово, и не зря: от него зависит всё
            // остальное — кому достаётся клавиатура, рисуется ли под меню экран
            // загрузки, слышит ли страница мышь. Пока эта строка не появилась в
            // журнале, любая жалоба на ввод означает сразу несколько разных бед.
            spdlog::debug("menu reports itself: {}", open ? "открыто" : "закрыто");

            opened.store(open);
            updateCursor();
            return;
        }

        if (name == "settings:change") {
            changeSetting(arguments);
            return;
        }

        if (name == "connection:connect") {
            connect(arguments);
            return;
        }

        if (name == "connection:reconnect") {
            reconnect();
            return;
        }

        if (name == "console:openLogFile") {
            if (actions.openLog) {
                actions.openLog();
            }
            return;
        }

        if (name == "connection:disconnect" || name == "connection:abort") {
            if (actions.disconnect) {
                actions.disconnect();
            }
            return;
        }

        if (name == "exit") {
            if (actions.quit) {
                actions.quit();
            }
            return;
        }

        if (name == "ui:resetSkin") {
            resetSkin();
            return;
        }

        if (name == "servers:reload") {
            sendServers();
            return;
        }

        if (name == "settings:devices:reload") {
            sendRecordingDevices();
            return;
        }

        if (name == "servers:favorite:add") {
            addFavorite(arguments);
            return;
        }

        if (name == "servers:favorite:remove") {
            removeFavorite(arguments);
            return;
        }

        if (name == "console:setState") {
            // Страница закрыла консоль сама — например уведя её в прозрачный
            // вид. Не узнав об этом, мы держали бы клавиатуру у страницы, а игра
            // осталась бы без управления.
            const bool open = !arguments.empty() && arguments[0].is_boolean() &&
                              arguments[0].get<bool>();

            consoleOpen.store(open);
            updateCursor();
            return;
        }

        if (name == "console:execute") {
            executeConsoleLine(arguments);
            return;
        }

        // Прочее страница шлёт по своему устройству — обновить список серверов,
        // перечислить устройства записи, выполнить строку в консоли. Отвечать ей
        // пока нечем, и молчание здесь честнее выдуманного ответа.
        //
        // Записывается один раз на имя события и в общий журнал, а не в
        // отладочный: жалоба «кнопка не работает» иначе не проверяется ничем —
        // по этой строке сразу видно, какая именно кнопка и чего она просила.
        if (unanswered.insert(std::string{name}).second) {
            spdlog::debug("menu asks for {} — nothing to answer with yet", name);
        }
    }

    void changeSetting(const nlohmann::json& arguments) {
        if (arguments.size() < 2 || !arguments[0].is_string()) {
            return;
        }

        const std::string key = arguments[0].get<std::string>();

        const std::lock_guard guard{mutex};

        // С заменой негодных знаков, по той же причине, что и в emit: имя игрока
        // приходит сюда набранным вручную.
        const std::string value =
            arguments[1].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

        if (!settings.applyJson(key, value)) {
            return;
        }

        settings.save(settingsPath());
    }

    void connect(const nlohmann::json& arguments) {
        if (arguments.empty() || !arguments[0].is_string()) {
            return;
        }

        const std::string address = arguments[0].get<std::string>();
        const std::string password =
            arguments.size() > 1 && arguments[1].is_string() ? arguments[1].get<std::string>() : "";

        // Страница присылает следом опознаватель и имя сервера, если знает их, —
        // то есть когда игрок выбрал сервер из списка, а не набрал адрес руками.
        const std::string id =
            arguments.size() > 2 && arguments[2].is_string() ? arguments[2].get<std::string>() : "";
        const std::string name =
            arguments.size() > 3 && arguments[3].is_string() ? arguments[3].get<std::string>() : "";

        {
            // Адрес запоминается до попытки, а не после успеха: игрок, у
            // которого сервер не поднялся, при следующем запуске увидит тот же
            // адрес и попробует снова, а не станет набирать его заново. По той
            // же причине сюда же попадает и история.
            const std::lock_guard guard{mutex};

            settings.set("lastip", std::string_view{address});
            settings.save(settingsPath());

            history.visited(ServerHistory::Entry{
                .name = name.empty() ? address : name,
                .id = id,
                .url = address,
            });
            history.save(historyPath());
        }

        // Список недавних меняется прямо сейчас — страница должна увидеть это
        // сразу, а не при следующем запуске.
        sendServers();

        if (actions.connect) {
            actions.connect(address, password);
        }
    }

    /// Повтор по последнему адресу.
    ///
    /// Довода страница не присылает вовсе — и не по забывчивости: адрес, к
    /// которому шли, записан в настройках, и alt:V берёт его оттуда же. Взять
    /// его из состояния страницы было бы нельзя: к мгновению, когда игрок жмёт
    /// «повторить», она успела показать разрыв и забыть, куда шла.
    void reconnect() {
        std::string address;

        {
            const std::lock_guard guard{mutex};
            address = settings.text("lastip");
        }

        if (address.empty()) {
            spdlog::warn("menu asks to reconnect, but no previous address is known");
            return;
        }

        if (actions.connect) {
            actions.connect(address, "");
        }
    }

    /// Отмечает сервер звёздочкой. Страница присылает опознаватель и имя.
    void addFavorite(const nlohmann::json& arguments) {
        if (arguments.empty() || !arguments[0].is_string()) {
            return;
        }

        ServerHistory::Entry entry;
        entry.id = arguments[0].get<std::string>();
        entry.name = arguments.size() > 1 && arguments[1].is_string()
                         ? arguments[1].get<std::string>()
                         : entry.id;

        {
            const std::lock_guard guard{mutex};

            // Адрес берётся из оформления: страница его не присылает, а без него
            // по звёздочке некуда было бы вернуться.
            if (skin.has_value()) {
                for (const config::Skin::Server& server : skin->servers) {
                    if (server.id == entry.id) {
                        entry.url = server.address;
                        break;
                    }
                }
            }

            history.addFavorite(std::move(entry));
            history.save(historyPath());
        }

        sendServers();
    }

    void removeFavorite(const nlohmann::json& arguments) {
        if (arguments.empty() || !arguments[0].is_string()) {
            return;
        }

        {
            const std::lock_guard guard{mutex};

            history.removeFavorite(arguments[0].get<std::string>());
            history.save(historyPath());
        }

        sendServers();
    }

    /// Исполняет строку, набранную в консоли страницы.
    ///
    /// Отправляется она в чат — тем же путём, каким игрок отправил бы её сам.
    /// Своих правил игры у клиента нет и не должно появиться (см. CLAUDE.md), а
    /// команды принадлежат ресурсам сервера: `/help` разбирает он. Строка,
    /// уходящая в никуда, была бы хуже — набранная команда молча пропадала бы.
    void executeConsoleLine(const nlohmann::json& arguments) {
        if (arguments.empty() || !arguments[0].is_string()) {
            return;
        }

        std::string line = arguments[0].get<std::string>();
        if (line.empty()) {
            return;
        }

        if (actions.say) {
            actions.say(std::move(line));
        }
    }

    void resetSkin() {
        {
            const std::lock_guard guard{mutex};

            settings.set("launcherSkin", std::string_view{});
            settings.save(settingsPath());

            skin.reset();
        }

        emit("version:setManifest", nlohmann::json::array({nullptr}));
    }
};

std::unique_ptr<Menu> Menu::create(cefui::Browser& browser, std::filesystem::path directory,
                                   Actions actions) {
    std::unique_ptr<Menu> menu{new Menu};
    menu->state_ = std::make_unique<State>();

    State& state = *menu->state_;
    state.browser = &browser;
    state.directory = std::move(directory);
    state.actions = std::move(actions);

    state.settings = config::Settings::load(state.settingsPath());
    state.skin = config::Skin::load(state.directory / kSkinFile);
    state.history = ServerHistory::load(state.historyPath());

    // Подписка раньше загрузки: страница шлёт `loaded` в конце своего запуска, и
    // подпишись мы после — первое же её слово ушло бы в никуда.
    browser.onEvent([&state](std::string_view name, std::string_view arguments) {
        state.handle(name, arguments);
    });

    browser.open(kPageUrl);

    return menu;
}

Menu::~Menu() {
    if (state_ != nullptr && state_->browser != nullptr) {
        // Обработчик снимается прежде, чем уйдёт то, на что он ссылается: поток
        // CEF о нашем разрушении не знает и вправе прислать событие в тот же миг.
        state_->browser->onEvent(nullptr);
    }
}

bool Menu::opened() const noexcept {
    return state_ != nullptr && state_->opened.load();
}

bool Menu::pageReady() const noexcept {
    return state_ != nullptr && state_->ready.load();
}

bool Menu::consoleOpen() const noexcept {
    return state_ != nullptr && state_->consoleOpen.load();
}

void Menu::askExit() {
    if (state_ != nullptr) {
        state_->emit("exit");
    }
}

void Menu::toggleConsole() {
    if (state_ == nullptr) {
        return;
    }

    // Своим счётчиком, а не просьбой перевернуть: страница отвечает о консоли
    // только тогда, когда закрывает её сама, и переворот вслепую разошёлся бы с
    // ней при первом же таком случае.
    const bool open = !state_->consoleOpen.load();
    state_->consoleOpen.store(open);

    state_->emit("console:open", nlohmann::json::array({open}));
    state_->updateCursor();
}

void Menu::pushLog(unsigned int level, std::string_view text) {
    if (state_ == nullptr) {
        return;
    }

    // Строка собирается из двух событий: `console:push` кладёт кусок в буфер,
    // `console:end` закрывает строку и приписывает ей источник и важность. Так
    // устроена консоль страницы — она умеет раскрашивать строку по кускам, а нам
    // раскрашивать нечего: у журнала цвет один на строку.
    constexpr int kWhite = 14;

    // Уровни spdlog: trace, debug, info, warn, err, critical, off. Виды строк у
    // страницы: 0 обычная, 1 предупреждение, 2 ошибка, 3 отладочная.
    const int kind = [level] {
        switch (level) {
        case 0:
        case 1:
            return 3;
        case 3:
            return 1;
        case 4:
        case 5:
            return 2;
        default:
            return 0;
        }
    }();

    state_->emit("console:push", nlohmann::json::array({kWhite, std::string{text}}));
    state_->emit("console:end", nlohmann::json::array({"oxymp", kind}));
}

std::string Menu::playerName() const {
    if (state_ == nullptr) {
        return {};
    }

    const std::lock_guard guard{state_->mutex};
    return state_->settings.text("name");
}

void Menu::toggle() {
    if (state_ != nullptr) {
        // Без довода: страница сама переворачивает своё состояние. Пошли мы ей
        // «открыть» или «закрыть», нам пришлось бы держать своё представление о
        // том, открыта ли она, — а оно разошлось бы с её собственным при первом
        // же разговоре о разрешениях, который она открывает поверх себя сама.
        state_->emit("ui:toggle");
    }
}

void Menu::connecting(std::string_view name) {
    if (state_ != nullptr) {
        // Строкой, а не объектом. Объект здесь был, и страница показывала его
        // как есть — игрок видел в заголовке `{"name":"127.0.0.1:7788",...}`.
        // Она ждёт именно строку и подставляет её в заголовок без разбора.
        state_->emit("connection:setServer", nlohmann::json::array({std::string{name}}));
        state_->emit("connection:connecting");
    }
}

void Menu::nameServer(std::string_view name) {
    if (state_ != nullptr) {
        state_->emit("connection:setServer", nlohmann::json::array({std::string{name}}));
    }
}

void Menu::validatingResources(std::size_t done, std::size_t total) {
    if (state_ != nullptr) {
        state_->emit("connection:validatingResources", nlohmann::json::array({done, total}));
    }
}

void Menu::downloadingResources(std::uint64_t bytesDone, std::uint64_t bytesTotal,
                                std::uint64_t bytesPerSecond) {
    if (state_ == nullptr) {
        return;
    }

    // Скорость — довод необязательный, и пустой она быть не должна: страница
    // отличает «скорость не названа» от «скорость ноль» по самому наличию
    // довода.
    if (bytesPerSecond == 0) {
        state_->emit("connection:downloadingResources",
                     nlohmann::json::array({bytesDone, bytesTotal}));
        return;
    }

    state_->emit("connection:downloadingResources",
                 nlohmann::json::array({bytesDone, bytesTotal, bytesPerSecond}));
}

void Menu::loadingResources() {
    if (state_ != nullptr) {
        state_->emit("connection:startingResources");
    }
}

void Menu::startingGame(unsigned int stage, unsigned int stages) {
    if (state_ != nullptr) {
        state_->emit("connection:startingGame", nlohmann::json::array({stage, stages}));
    }
}

void Menu::joining() {
    if (state_ != nullptr) {
        state_->emit("connection:joining");
    }
}

void Menu::connected() {
    if (state_ != nullptr) {
        state_->emit("connection:connected");
    }
}

void Menu::disconnected(std::string_view reason) {
    if (state_ != nullptr) {
        state_->emit("connection:disconnected", nlohmann::json::array({std::string{reason}}));
    }
}

void Menu::failed(std::string_view reason) {
    if (state_ != nullptr) {
        state_->emit("connection:failed", nlohmann::json::array({std::string{reason}, false}));
    }
}

} // namespace oxymp::client
