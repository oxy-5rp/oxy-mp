#include "server_history.hpp"

#include <nlohmann/json.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

namespace oxymp::client {
namespace {

/// Читает один сервер из записи JSON. Пусто, если записи не хватает адреса.
[[nodiscard]] std::optional<ServerHistory::Entry> readEntry(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }

    ServerHistory::Entry entry;

    if (const auto url = value.find("url"); url != value.end() && url->is_string()) {
        entry.url = url->get<std::string>();
    }

    // Без адреса запись бесполезна: по ней некуда вернуться.
    if (entry.url.empty()) {
        return std::nullopt;
    }

    if (const auto name = value.find("name"); name != value.end() && name->is_string()) {
        entry.name = name->get<std::string>();
    }

    if (const auto id = value.find("id"); id != value.end() && id->is_string()) {
        entry.id = id->get<std::string>();
    }

    if (entry.name.empty()) {
        entry.name = entry.url;
    }

    return entry;
}

void readList(const nlohmann::json& source, std::string_view key,
              std::vector<ServerHistory::Entry>& into) {
    const auto found = source.find(key);
    if (found == source.end() || !found->is_array()) {
        return;
    }

    for (const nlohmann::json& value : *found) {
        if (std::optional<ServerHistory::Entry> entry = readEntry(value)) {
            into.push_back(std::move(*entry));
        }
    }
}

[[nodiscard]] nlohmann::json writeList(const std::vector<ServerHistory::Entry>& list) {
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

} // namespace

ServerHistory ServerHistory::load(const std::filesystem::path& file) {
    ServerHistory history;

    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        return history;
    }

    const std::string text{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};

    // Разбор без исключений: файл правится руками, и однажды он окажется
    // испорченным. Испорченная история — это пустая история, а не отказ
    // запускаться.
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false, true);
    if (!parsed.is_object()) {
        spdlog::warn("история серверов не разобралась — начинаем с чистой");
        return history;
    }

    readList(parsed, "recent", history.recent_);
    readList(parsed, "favorite", history.favorite_);

    return history;
}

void ServerHistory::save(const std::filesystem::path& file) const {
    const nlohmann::json document{
        {"recent", writeList(recent_)},
        {"favorite", writeList(favorite_)},
    };

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::ofstream stream{file, std::ios::binary | std::ios::trunc};
    if (!stream) {
        spdlog::warn("историю серверов не удалось записать: {}", file.string());
        return;
    }

    stream << document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

void ServerHistory::visited(Entry entry) {
    if (entry.url.empty()) {
        return;
    }

    if (entry.name.empty()) {
        entry.name = entry.url;
    }

    std::erase_if(recent_, [&entry](const Entry& known) { return known.url == entry.url; });

    recent_.insert(recent_.begin(), std::move(entry));

    if (recent_.size() > kMaxRecent) {
        recent_.resize(kMaxRecent);
    }
}

void ServerHistory::addFavorite(Entry entry) {
    if (entry.id.empty() && entry.url.empty()) {
        return;
    }

    const auto same = [&entry](const Entry& known) {
        return !entry.id.empty() ? known.id == entry.id : known.url == entry.url;
    };

    if (std::ranges::any_of(favorite_, same)) {
        return;
    }

    favorite_.push_back(std::move(entry));
}

void ServerHistory::removeFavorite(std::string_view id) {
    std::erase_if(favorite_, [id](const Entry& known) { return known.id == id; });
}

} // namespace oxymp::client
