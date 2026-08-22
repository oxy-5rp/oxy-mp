#include <oxymp/config/skin.hpp>

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include <fstream>
#include <string>

namespace oxymp::config {
namespace {

/// Достаёт строку по имени. Отсутствующее и не-строковое считаются пустым.
///
/// Именно считаются, а не приводят к отказу: файл оформления делают художники в
/// стороннем средстве, и недостающее поле в нём — обычное дело. Отказаться от
/// всего оформления из-за одной пропущенной картинки значило бы наказать игрока
/// за чужую невнимательность.
[[nodiscard]] std::string textField(const nlohmann::json& object, const char* name) {
    const auto found = object.find(name);
    if (found == object.end() || !found->is_string()) {
        return {};
    }

    return found->get<std::string>();
}

[[nodiscard]] bool flagField(const nlohmann::json& object, const char* name) {
    const auto found = object.find(name);
    return found != object.end() && found->is_boolean() && found->get<bool>();
}

} // namespace

std::optional<Skin> Skin::load(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return std::nullopt;
    }

    std::ifstream in{file, std::ios::binary};
    if (!in) {
        spdlog::debug("skin file could not be opened: {}", file.string());
        return std::nullopt;
    }

    // Разбор без исключений: испорченный файл оформления — не повод не запускать
    // игру, а повод играть без оформления.
    const nlohmann::json document = nlohmann::json::parse(in, nullptr, false, true);

    if (document.is_discarded() || !document.is_object()) {
        spdlog::debug("skin file did not parse: {}", file.string());
        return std::nullopt;
    }

    Skin skin;

    skin.gameName = textField(document, "customUiGameAppName");
    skin.launcherName = textField(document, "customUiLauncherAppName");
    skin.icon = textField(document, "customUiImageAppIcon");
    skin.uiBackground = textField(document, "uiBackground");
    skin.launcherBackground = textField(document, "launcherBackground");
    skin.installerBackground = textField(document, "installerBackground");
    skin.logo = textField(document, "logo");
    skin.rss = textField(document, "rss");
    skin.rssHidden = flagField(document, "isRssHidden");

    skin.primaryColor = textField(document, "primaryColor");

    // Решётка снимается здесь и только здесь: страница ждёт ровно шесть знаков и
    // молча отказывается от цвета, если их не шесть.
    if (!skin.primaryColor.empty() && skin.primaryColor.front() == '#') {
        skin.primaryColor.erase(0, 1);
    }

    if (const auto servers = document.find("servers");
        servers != document.end() && servers->is_array()) {
        for (const nlohmann::json& entry : *servers) {
            if (!entry.is_object()) {
                continue;
            }

            Server server;
            server.id = textField(entry, "id");
            server.name = textField(entry, "name");
            server.address = textField(entry, "ip");

            // Сервер без адреса показывать можно, а подключиться к нему нельзя.
            // Такие в списке не нужны: нажатие по ним ничем не кончится.
            if (!server.address.empty()) {
                skin.servers.push_back(std::move(server));
            }
        }
    }

    spdlog::debug("skin: {}, {} servers",
                 skin.gameName.empty() ? "без имени" : skin.gameName, skin.servers.size());

    return skin;
}

std::string Skin::toManifestJson() const {
    nlohmann::json manifest;

    manifest["name"] = gameName;
    manifest["rss"] = rssHidden ? std::string{} : rss;
    manifest["primaryColor"] = primaryColor;
    manifest["logo"] = logo;
    manifest["uiBackground"] = uiBackground;

    nlohmann::json list = nlohmann::json::array();

    for (const Server& server : servers) {
        // Имена полей — те, которых ждёт страница: у неё это name, id и url.
        // В файле оформления адрес назван ip, и переименование живёт здесь.
        list.push_back(nlohmann::json{
            {"name", server.name},
            {"id", server.id},
            {"url", server.address},
        });
    }

    manifest["servers"] = std::move(list);

    return manifest.dump();
}

} // namespace oxymp::config
