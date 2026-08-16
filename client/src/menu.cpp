#include "menu.hpp"

#include <oxymp/cefui/browser.hpp>
#include <oxymp/config/settings.hpp>
#include <oxymp/config/skin.hpp>

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include <mutex>
#include <optional>
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

/// Ветвь сборки. Страница показывает её рядом с версией.
constexpr const char* kBranch = "release";

} // namespace

struct Menu::State {
    cefui::Browser* browser = nullptr;
    std::filesystem::path directory;
    Actions actions;

    /// Настройки правятся из потока CEF, а читаются и оттуда, и из потока игры.
    mutable std::mutex mutex;
    config::Settings settings;
    std::optional<config::Skin> skin;

    /// Открыто ли меню. Читает поток игры — отсюда и счётчик вместо блокировки.
    std::atomic<bool> opened{true};

    [[nodiscard]] std::filesystem::path settingsPath() const { return directory / kSettingsFile; }

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

    /// Рассказывает странице всё, что она должна знать к первому кадру.
    ///
    /// Зовётся по её же слову — `loaded`, — а не сразу после загрузки. Разница
    /// существенная: обработчики она заводит, пока строит свои хранилища, и
    /// присланное до этого ушло бы в никуда.
    void sendInitialState() {
        std::string settingsJson;
        std::string lastAddress;
        std::optional<std::string> manifest;
        nlohmann::json recent = nlohmann::json::array();

        {
            const std::lock_guard guard{mutex};

            settingsJson = settings.toJson();
            lastAddress = settings.text("lastip");

            if (skin.has_value()) {
                manifest = skin->toManifestJson();

                for (const config::Skin::Server& server : skin->servers) {
                    recent.push_back(nlohmann::json{
                        {"name", server.name},
                        {"id", server.id},
                        {"url", server.address},
                    });
                }
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
            spdlog::error("настройки не легли в JSON — страница увидит свои умолчания");
        }

        // Недавние — из оформления: своего списка серверов у нас нет и не будет,
        // пока нет общего каталога. Пустой список страница показывает как «вы
        // ещё никуда не заходили», и это правда.
        emit("servers:recent:update", nlohmann::json::array({recent}));

        // Готовность объявляется последней, и порядок здесь обязателен: по
        // `ui:ready` страница показывает себя и спрашивает имя, если его нет, —
        // а имя приходит настройками строкой выше.
        emit("ui:ready");
        emit("version:ready");

        spdlog::info("меню готово");
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
            sendInitialState();
            return;
        }

        if (name == "ui:open") {
            opened.store(arguments.size() > 0 && arguments[0].is_boolean() &&
                         arguments[0].get<bool>());
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

        // Прочее страница шлёт по своему устройству — обновить список серверов,
        // перечислить устройства записи, выполнить строку в консоли. Отвечать ей
        // пока нечем, и молчание здесь честнее выдуманного ответа.
        spdlog::debug("меню просит {} — пока нечем ответить", name);
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

        {
            // Адрес запоминается до попытки, а не после успеха: игрок, у
            // которого сервер не поднялся, при следующем запуске увидит тот же
            // адрес и попробует снова, а не станет набирать его заново.
            const std::lock_guard guard{mutex};

            settings.set("lastip", std::string_view{address});
            settings.save(settingsPath());
        }

        if (actions.connect) {
            actions.connect(address, password);
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

void Menu::connecting(std::string_view address) {
    if (state_ != nullptr) {
        state_->emit("connection:setServer",
                     nlohmann::json::array({nlohmann::json{{"name", address}, {"url", address}}}));
        state_->emit("connection:connecting");
    }
}

void Menu::connected() {
    if (state_ != nullptr) {
        state_->emit("connection:connected");
    }
}

void Menu::disconnected() {
    if (state_ != nullptr) {
        state_->emit("connection:disconnected");
    }
}

void Menu::failed(std::string_view reason) {
    if (state_ != nullptr) {
        state_->emit("connection:failed", nlohmann::json::array({std::string{reason}, false}));
    }
}

} // namespace oxymp::client
