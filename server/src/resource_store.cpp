#include "resource_store.hpp"

#include <oxymp/shared/resource/vault.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <optional>

namespace oxymp::server {
namespace {

/// Читает файл целиком. Пусто — прочитать не вышло.
/// Читает файл целиком. Пусто — прочитать не удалось.
///
/// Ответ обёрнут в optional, а не отдан пустым вектором, и это не украшательство.
/// Пустой файл — законный файл: в собранной странице интерфейса такие есть
/// (заготовка значка, до которой не дошли руки у художника). Пока «не открылся»
/// и «пуст» отвечали одинаково, сервер жаловался на исправный файл и не раздавал
/// его вовсе — а клиент потом не находил его у себя и показывал страницу с
/// дырой, и по журналу это выглядело виной клиента.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readFile(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }

    const std::streamoff size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    if (size == 0) {
        return std::vector<std::uint8_t>{};
    }

    file.seekg(0);

    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);

    if (!file) {
        return std::nullopt;
    }

    return data;
}

} // namespace

void ResourceStore::load(const std::filesystem::path& directory) {
    items_.clear();
    packed_.clear();

    std::error_code ec;

    if (!std::filesystem::exists(directory, ec)) {
        spdlog::debug("the game files directory {} is missing: nothing to serve",
                     directory.string());
        return;
    }

    const std::vector<std::uint8_t> key = shared::Vault::builtInKey();

    for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }

        const std::filesystem::path& path = entry.path();

        const std::optional<std::vector<std::uint8_t>> plain = readFile(path);
        if (!plain) {
            spdlog::warn("file {} could not be read: skipping", path.filename().string());
            continue;
        }

        std::vector<std::uint8_t> packed = shared::Vault::pack(*plain, key);

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
        spdlog::debug("file {}: {} KB", item.name, item.size / 1024);
        spdlog::debug("  /resources/dlcpacks/{}.resource", hash);

        items_.push_back(std::move(item));
        packed_.emplace(hash, std::move(packed));
    }

    // Раскладка RAGE MP: каталог на пак, а внутри всегда `dlc.rpf`.
    //
    // Читается наравне с файлами, лежащими россыпью, и это не поблажка ради
    // удобства. Хозяин сервера, переходящий с RAGE MP, держит свои паки именно
    // так — `client_packages/game_resources/dlcpacks/имя/dlc.rpf`, — и требовать
    // от него разложить полсотни архивов заново, переименовав каждый, значило бы
    // требовать переделать сервер ради нас.
    //
    // Имя раздачи берётся у каталога, а не у файла: `dlc.rpf` у всех паков один
    // и тот же, и по нему они сошлись бы в один.
    for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
        if (!entry.is_directory(ec)) {
            continue;
        }

        const std::filesystem::path inside = entry.path() / "dlc.rpf";

        if (!std::filesystem::exists(inside, ec)) {
            continue;
        }

        const std::optional<std::vector<std::uint8_t>> plain = readFile(inside);
        if (!plain) {
            spdlog::warn("file {} could not be read: skipping", inside.string());
            continue;
        }

        std::vector<std::uint8_t> packed = shared::Vault::pack(*plain, key);
        const std::string hash = shared::fingerprint(packed);

        Item item;
        item.name = entry.path().filename().string() + ".rpf";
        item.hash = hash;
        item.size = packed.size();

        spdlog::debug("file {}: {} KB", item.name, item.size / 1024);
        spdlog::debug("  /resources/dlcpacks/{}.resource", hash);

        items_.push_back(std::move(item));
        packed_.emplace(hash, std::move(packed));
    }

    if (ec) {
        spdlog::warn("the resource directory was not read in full: {}", ec.message());
    }

    // Порядок задаётся именем, а не тем, как каталог лёг на диск: клиент
    // показывает список человеку, и он не должен меняться от перезапуска.
    std::ranges::sort(items_, {}, &Item::name);

    spdlog::info("Files ready to serve: {}", items_.size());
}

bool ResourceStore::add(const std::filesystem::path& path, std::string name) {
    const std::optional<std::vector<std::uint8_t>> plain = readFile(path);
    if (!plain) {
        return false;
    }

    std::vector<std::uint8_t> packed = shared::Vault::pack(*plain, shared::Vault::builtInKey());
    const std::string hash = shared::fingerprint(packed);

    Item item;
    item.name = std::move(name);
    item.hash = hash;
    item.size = packed.size();

    spdlog::debug("file {}: {} KB", item.name, item.size / 1024);
    spdlog::debug("  /resources/dlcpacks/{}.resource", hash);

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
