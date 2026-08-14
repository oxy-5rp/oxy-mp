#include "resource_store.hpp"

#include <oxymp/shared/resource/vault.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>

namespace oxymp::server {
namespace {

/// Читает файл целиком. Пусто — прочитать не вышло.
std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }

    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }

    file.seekg(0);

    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (!file) {
        return {};
    }

    return data;
}

} // namespace

void ResourceStore::load(const std::filesystem::path& directory) {
    items_.clear();
    packed_.clear();

    std::error_code ec;

    if (!std::filesystem::exists(directory, ec)) {
        spdlog::info("каталог игровых файлов {} отсутствует — раздавать нечего",
                     directory.string());
        return;
    }

    const std::vector<std::uint8_t> key = shared::Vault::builtInKey();

    for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }

        const std::filesystem::path& path = entry.path();

        const std::vector<std::uint8_t> plain = readFile(path);
        if (plain.empty()) {
            spdlog::warn("ресурс {} прочитать не удалось — пропускаем", path.filename().string());
            continue;
        }

        std::vector<std::uint8_t> packed = shared::Vault::pack(plain, key);

        // Отпечаток считается по зашифрованному, а не по исходному, и это
        // важно: клиент проверяет им то, что скачал, — а скачивает он именно
        // зашифрованное. Отпечаток исходного он смог бы проверить только после
        // расшифровки, то есть уже приняв мусор за содержимое.
        const std::string hash = shared::fingerprint(packed);

        Item item;
        item.name = path.filename().string();
        item.hash = hash;
        item.size = packed.size();

        // Отпечаток целиком, а не началом: по нему строится ссылка, и хозяину
        // сервера нужно иметь возможность проверить раздачу браузером.
        spdlog::info("ресурс {}: {} КБ", item.name, item.size / 1024);
        spdlog::info("  /resources/dlcpacks/{}.resource", hash);

        items_.push_back(std::move(item));
        packed_.emplace(hash, std::move(packed));
    }

    if (ec) {
        spdlog::warn("каталог ресурсов прочитан не полностью: {}", ec.message());
    }

    // Порядок задаётся именем, а не тем, как каталог лёг на диск: клиент
    // показывает список человеку, и он не должен меняться от перезапуска.
    std::ranges::sort(items_, {}, &Item::name);

    spdlog::info("ресурсов готово к раздаче: {}", items_.size());
}

bool ResourceStore::add(const std::filesystem::path& path, std::string name) {
    const std::vector<std::uint8_t> plain = readFile(path);
    if (plain.empty()) {
        return false;
    }

    std::vector<std::uint8_t> packed = shared::Vault::pack(plain, shared::Vault::builtInKey());
    const std::string hash = shared::fingerprint(packed);

    Item item;
    item.name = std::move(name);
    item.hash = hash;
    item.size = packed.size();

    spdlog::info("ресурс {}: {} КБ", item.name, item.size / 1024);
    spdlog::info("  /resources/dlcpacks/{}.resource", hash);

    items_.push_back(std::move(item));

    // Содержимое кладётся один раз: два одинаковых файла дают один отпечаток, и
    // второй раз хранить то же самое незачем. В списке они при этом оба —
    // клиенту важно имя, а качать он будет один файл.
    packed_.emplace(hash, std::move(packed));

    return true;
}

const std::vector<std::uint8_t>* ResourceStore::find(const std::string& hash) const {
    const auto it = packed_.find(hash);
    return it == packed_.end() ? nullptr : &it->second;
}

} // namespace oxymp::server
