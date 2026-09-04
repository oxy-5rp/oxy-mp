#include "resource_store.hpp"

#include "rpf7.hpp"

#include <oxymp/shared/resource/bundle.hpp>
#include <oxymp/shared/resource/vault.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
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

std::string ResourceStore::addBundle(const std::filesystem::path& root,
                                    std::span<const std::string> files) {
    std::vector<shared::Bundle::File> inside;
    inside.reserve(files.size());

    for (const std::string& file : files) {
        std::optional<std::vector<std::uint8_t>> plain = readFile(root / file);
        if (!plain) {
            spdlog::warn("file {} could not be read: it will not be in the bundle", file);
            continue;
        }

        // Путь внутри свёртка — от корня ресурса и всегда через прямую косую
        // черту: свёрток собирают на Windows, а читать его будет тот же клиент,
        // но искать в нём файл будет по пути, который написал автор ресурса.
        std::string path = file;
        std::ranges::replace(path, '\\', '/');

        inside.push_back(shared::Bundle::File{.path = std::move(path),
                                              .contents = std::move(*plain)});
    }

    if (inside.empty()) {
        return {};
    }

    std::vector<std::uint8_t> packed =
        shared::Bundle::pack(inside, shared::Vault::builtInKey());

    std::string hash = shared::fingerprint(packed);

    spdlog::debug("bundle of {} files: {} KB", inside.size(), packed.size() / 1024);
    spdlog::debug("  /resources/dlcpacks/{}.resource", hash);

    packed_.emplace(hash, std::move(packed));

    return hash;
}

bool ResourceStore::addContents(std::string name, const std::vector<std::uint8_t>& plain) {
    if (plain.empty()) {
        return false;
    }

    std::vector<std::uint8_t> packed = shared::Vault::pack(plain, shared::Vault::builtInKey());
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

bool ResourceStore::add(const std::filesystem::path& path, std::string name) {
    const std::optional<std::vector<std::uint8_t>> plain = readFile(path);
    if (!plain) {
        return false;
    }

    return addContents(std::move(name), *plain);
}

bool ResourceStore::addMaybeArchive(const std::filesystem::path& path, std::string name) {
    const std::optional<std::vector<std::uint8_t>> plain = readFile(path);
    if (!plain) {
        return false;
    }

    // Архив мода — не файл для игры, а короб с россыпью. Игра чужой .rpf не
    // читает (оглавление она оставляет зашифрованным, метку OPEN не принимает),
    // поэтому короб с меткой OPEN распаковывается здесь, а содержимое уходит
    // клиенту россыпью — той же дорогой, что и модели, лежащие файлами. NG-архивы
    // (в том числе настоящие DLC самой игры) распаковка отвергает — они уходят
    // как есть, и их монтирует уже игра. Подробности — в памяти проекта
    // custom-rpf-load-via-open-parse и docs/altv-parity.md.
    if (name.ends_with(".rpf")) {
        std::string error;
        std::vector<RpfEntry> unpacked = unpackOpenRpf7(plain->data(), plain->size(), error);

        if (!unpacked.empty()) {
            for (RpfEntry& entry : unpacked) {
                addContents(std::format("{}/{}", name, entry.path), entry.data);
            }
            spdlog::info("archive {} unpacked into {} loose files", name, unpacked.size());
            return true;
        }

        spdlog::debug("archive {} is not an OPEN archive ({}); served as-is", name, error);
        // Дальше — как обычный файл: не наш случай, пусть с ним разбирается игра.
    }

    return addContents(std::move(name), *plain);
}

const std::vector<std::uint8_t>* ResourceStore::find(const std::string& hash) const {
    const auto it = packed_.find(hash);
    return it == packed_.end() ? nullptr : &it->second;
}

} // namespace oxymp::server
