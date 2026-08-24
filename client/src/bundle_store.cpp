#include "bundle_store.hpp"

#include <oxymp/shared/resource/vault.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <utility>

namespace oxymp::client {
namespace {

/// Читает кусок файла с указанного места. Пусто — не вышло.
///
/// Отдельной функцией, а не строчкой на месте, потому что коротких чтений здесь
/// два — оглавление и тело, — и промах в смещении у одного из них выглядел бы
/// как испорченный свёрток.
std::vector<std::uint8_t> readPart(const std::filesystem::path& file, std::uint64_t at,
                                   std::size_t length) {
    if (length == 0) {
        return {};
    }

    std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        return {};
    }

    stream.seekg(static_cast<std::streamoff>(at));
    if (!stream) {
        return {};
    }

    std::vector<std::uint8_t> part(length);
    stream.read(reinterpret_cast<char*>(part.data()), static_cast<std::streamsize>(length));

    // gcount, а не признак потока: чтение до конца файла выставляет eof, но
    // прочитанное при этом верно, и отвергать его было бы неправдой.
    if (static_cast<std::size_t>(stream.gcount()) != length) {
        return {};
    }

    return part;
}

} // namespace

BundleStore::BundleStore(std::filesystem::path directory)
    : directory_{std::move(directory)}, key_{shared::Vault::builtInKey()} {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    if (ec) {
        spdlog::warn("bundle directory {} was not created: {}", directory_.string(), ec.message());
    }
}

std::filesystem::path BundleStore::pathOf(const std::string& hash) const {
    return directory_ / (hash + ".resource");
}

bool BundleStore::has(const std::string& hash) const {
    const std::filesystem::path file = pathOf(hash);

    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return false;
    }

    // Заголовок читается, а не подразумевается. Оборванная закачка оставляет
    // файл нужного имени и неверной длины, и без этой проверки клиент считал бы
    // его готовым до конца существования кеша: имя-то сходится.
    const std::vector<std::uint8_t> head = readPart(file, 0, shared::Bundle::kHeaderLength);
    if (head.empty()) {
        return false;
    }

    const auto header = shared::Bundle::readHeader(head);
    if (!header) {
        return false;
    }

    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        return false;
    }

    return size == header->bodyStart() + header->bodyLength;
}

bool BundleStore::keep(const std::string& hash, std::span<const std::uint8_t> packed) {
    const std::filesystem::path file = pathOf(hash);

    // Сначала во временный, потом переименованием. Оборвись запись на середине —
    // под настоящим именем останется прежний файл или не останется ничего, но не
    // половина нового: половину `has` признал бы негодной только по длине, а
    // совпади длина случайно — клиент читал бы мусор.
    const std::filesystem::path partial = pathOf(hash + ".partial");

    {
        std::ofstream stream{partial, std::ios::binary | std::ios::trunc};
        if (!stream) {
            spdlog::error("bundle {} could not be written to the cache", hash);
            return false;
        }

        stream.write(reinterpret_cast<const char*>(packed.data()),
                     static_cast<std::streamsize>(packed.size()));

        if (!stream) {
            spdlog::error("bundle {} was not written in full", hash);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(partial, file, ec);

    if (ec) {
        // Переименование поверх существующего на Windows отказывает, если файл
        // держат открытым. Прежний свёрток с тем же отпечатком — это тот же
        // свёрток, и заменять его незачем.
        std::filesystem::remove(partial, ec);
        return has(hash);
    }

    return true;
}

bool BundleStore::bind(std::string resource, const std::string& hash,
                       std::span<const std::uint8_t> key) {
    const std::filesystem::path file = pathOf(hash);

    const std::vector<std::uint8_t> head = readPart(file, 0, shared::Bundle::kHeaderLength);
    if (head.empty()) {
        spdlog::error("bundle {} of resource {} cannot be read", hash, resource);
        return false;
    }

    const auto header = shared::Bundle::readHeader(head);
    if (!header) {
        spdlog::error("bundle {} of resource {} is not a bundle", hash, resource);
        return false;
    }

    const std::vector<std::uint8_t> index =
        readPart(file, shared::Bundle::kHeaderLength, header->indexLength);

    if (index.size() != header->indexLength) {
        spdlog::error("bundle {} of resource {} has an unreadable index", hash, resource);
        return false;
    }

    const std::vector<shared::Bundle::Entry> entries =
        shared::Bundle::readIndex(*header, index, key);

    if (entries.empty()) {
        spdlog::error("bundle {} of resource {} has an empty index", hash, resource);
        return false;
    }

    Opened opened;
    opened.file = file;
    opened.header = *header;
    opened.index.reserve(entries.size());

    for (const shared::Bundle::Entry& entry : entries) {
        opened.index.emplace(entry.path, entry);
    }

    spdlog::debug("resource {} reads {} files from its bundle", resource, opened.index.size());

    // Пользование обновляет время записи: по нему `prune` и отличает свёрток,
    // к которому ходят, от брошенного. Неудача здесь ничему не мешает — свёрток
    // просто состарится раньше срока и уедет, а клиент скачает его заново.
    std::error_code touched;
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), touched);

    const std::lock_guard guard{lock_};
    bundles_.insert_or_assign(std::move(resource), std::move(opened));

    return true;
}

std::string BundleStore::normalize(std::string_view file) {
    std::string path{file};

    // Свёрток собирали на Windows, а пути в нём — через прямую косую черту (см.
    // ResourceStore::addBundle). Спросить же могут как угодно: от игры и от
    // Chromium приходит и та и другая.
    std::ranges::replace(path, '\\', '/');

    // Приставка «./» — то, чем начинается всякий относительный путь в require, и
    // в оглавлении её нет.
    while (path.starts_with("./")) {
        path.erase(0, 2);
    }

    while (!path.empty() && path.front() == '/') {
        path.erase(0, 1);
    }

    return path;
}

bool BundleStore::read(std::string_view resource, std::string_view file,
                       std::vector<std::uint8_t>& contents) const {
    const std::string wanted = normalize(file);

    std::filesystem::path bundleFile;
    shared::Bundle::Header header;
    shared::Bundle::Entry entry;

    {
        const std::lock_guard guard{lock_};

        const auto bundle = bundles_.find(std::string{resource});
        if (bundle == bundles_.end()) {
            return false;
        }

        const auto found = bundle->second.index.find(wanted);
        if (found == bundle->second.index.end()) {
            return false;
        }

        bundleFile = bundle->second.file;
        header = bundle->second.header;
        entry = found->second;
    }

    // Само чтение — вне замка: оно идёт с диска и занимает столько, сколько
    // занимает, а держать на это время второй поток незачем. Дескриптор здесь
    // свой на каждое чтение, и делить нечего.
    contents = readPart(bundleFile, header.bodyStart() + entry.offset,
                        static_cast<std::size_t>(entry.size));

    if (contents.size() != entry.size) {
        spdlog::error("file {} of resource {} could not be read from its bundle", wanted, resource);
        contents.clear();
        return false;
    }

    shared::Bundle::openBody(contents, entry.offset, header, key_);

    return true;
}

void BundleStore::prune(std::chrono::hours maxAge) const {
    std::error_code ec;

    // Свёртки этого входа — те, что привязаны к ресурсам, — не трогаются: они
    // нужны прямо сейчас, и возраст здесь ни при чём.
    std::vector<std::filesystem::path> kept;
    {
        const std::lock_guard guard{lock_};

        kept.reserve(bundles_.size());

        for (const auto& [resource, opened] : bundles_) {
            kept.push_back(opened.file);
        }
    }

    const auto now = std::filesystem::file_time_type::clock::now();

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{directory_, ec}) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".resource") {
            continue;
        }

        if (std::ranges::find(kept, entry.path()) != kept.end()) {
            continue;
        }

        const std::filesystem::file_time_type written = entry.last_write_time(ec);
        if (ec) {
            continue;
        }

        if (now - written < maxAge) {
            continue;
        }

        if (std::filesystem::remove(entry.path(), ec)) {
            spdlog::debug("bundle {} was not used for a long time and is gone",
                          entry.path().filename().string());
        }
    }
}

bool BundleStore::knows(std::string_view resource) const {
    const std::lock_guard guard{lock_};
    return bundles_.contains(std::string{resource});
}

std::vector<std::string> BundleStore::list(std::string_view resource) const {
    const std::lock_guard guard{lock_};

    const auto bundle = bundles_.find(std::string{resource});
    if (bundle == bundles_.end()) {
        return {};
    }

    std::vector<std::string> paths;
    paths.reserve(bundle->second.index.size());

    for (const auto& [path, entry] : bundle->second.index) {
        paths.push_back(path);
    }

    // Порядок задаётся именем, а не тем, как легли записи в таблице: список
    // читает человек в журнале, и он не должен меняться от запуска к запуску.
    std::ranges::sort(paths);

    return paths;
}

} // namespace oxymp::client
