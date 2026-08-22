#include "resource_cache.hpp"

#include <oxymp/shared/resource/vault.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>

#include <windows.h>

#include <wininet.h>

namespace oxymp::client {
namespace {

/// Опись разложенного: строка на файл, отпечаток и составное имя.
constexpr const char* kIndexName = "resources.index";

/// Сколько ждать сервера, в миллисекундах.
///
/// Полминуты. Ресурсы бывают в десятки мегабайт, и канал у людей разный, но
/// молчание дольше этого означает не медленный канал, а отсутствие ответа.
constexpr DWORD kTimeout = 30'000;

/// Размер куска, которым читается ответ.
constexpr DWORD kChunkLength = 64 * 1024;

/// Предел на размер одного ресурса, в байтах.
///
/// Двести мегабайт. Нужен не против злого умысла, а против испорченного поля
/// длины: без предела клиент попытался бы выделить столько, сколько в нём
/// написано.
constexpr std::uint64_t kMaxResourceLength = 200ULL * 1024 * 1024;

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          length);

    return wide;
}

bool writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    // Каталоги по пути заводятся сами, и это не удобство, а необходимость.
    // Файлы ресурса ложатся деревом — `cache/resources/main/client/ui/app.js`, —
    // и ни одного из этих каталогов заранее нет. Без этой строки сохранение
    // молча отказывало на каждом файле ресурса: ofstream не заводит каталогов.
    std::error_code failure;
    std::filesystem::create_directories(path.parent_path(), failure);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));

    return static_cast<bool>(file);
}

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

    return file ? data : std::vector<std::uint8_t>{};
}

/// Закрывает дескриптор интернета сам. Ручное закрытие на каждом выходе — то
/// место, где рано или поздно забывают.
struct InternetHandle {
    HINTERNET value = nullptr;

    ~InternetHandle() {
        if (value != nullptr) {
            ::InternetCloseHandle(value);
        }
    }

    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

} // namespace

ResourceCache::ResourceCache(std::filesystem::path directory) : directory_(std::move(directory)) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);

    if (ec) {
        spdlog::warn("cache directory {} was not created: {}", directory_.string(), ec.message());
    }

    readIndex();
}

std::vector<std::uint8_t> ResourceCache::download(const std::string& url,
                                                  std::uint64_t expectedSize,
                                                  std::string& error) const {
    const InternetHandle session{::InternetOpenW(L"oxyMP", INTERNET_OPEN_TYPE_PRECONFIG, nullptr,
                                                 nullptr, 0)};
    if (session.value == nullptr) {
        error = "не удалось начать загрузку";
        return {};
    }

    ::InternetSetOptionW(session.value, INTERNET_OPTION_CONNECT_TIMEOUT,
                         const_cast<DWORD*>(&kTimeout), sizeof(kTimeout));
    ::InternetSetOptionW(session.value, INTERNET_OPTION_RECEIVE_TIMEOUT,
                         const_cast<DWORD*>(&kTimeout), sizeof(kTimeout));

    // Кеш Windows здесь только мешает: у нас свой, и он умнее — он знает
    // отпечаток.
    const InternetHandle request{::InternetOpenUrlW(session.value, widen(url).c_str(), nullptr, 0,
                                                    INTERNET_FLAG_RELOAD |
                                                        INTERNET_FLAG_NO_CACHE_WRITE,
                                                    0)};
    if (request.value == nullptr) {
        error = std::format("сервер не ответил на запрос ресурса (код {})", ::GetLastError());
        return {};
    }

    DWORD status = 0;
    DWORD statusLength = sizeof(status);

    if (::HttpQueryInfoW(request.value, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status,
                         &statusLength, nullptr) != FALSE &&
        status != 200) {
        error = std::format("сервер ответил {} вместо содержимого", status);
        return {};
    }

    if (expectedSize > kMaxResourceLength) {
        error = "ресурс слишком велик";
        return {};
    }

    std::vector<std::uint8_t> content;
    content.reserve(static_cast<std::size_t>(expectedSize));

    std::array<std::uint8_t, kChunkLength> chunk{};

    while (true) {
        DWORD received = 0;

        if (::InternetReadFile(request.value, chunk.data(), kChunkLength, &received) == FALSE) {
            error = "загрузка оборвалась";
            return {};
        }

        if (received == 0) {
            break;
        }

        content.insert(content.end(), chunk.begin(), chunk.begin() + received);

        if (content.size() > kMaxResourceLength) {
            error = "сервер отдаёт больше, чем обещал";
            return {};
        }
    }

    return content;
}

std::filesystem::path ResourceCache::resourceRoot() const {
    return directory_ / "resources";
}

void ResourceCache::readIndex() {
    std::ifstream file{directory_ / kIndexName, std::ios::binary};
    if (!file) {
        return;
    }

    std::string line;

    while (std::getline(file, line)) {
        // Строка: отпечаток, пробел, составное имя. Имя идёт последним, потому
        // что в нём бывают пробелы, а в отпечатке — нет.
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }

        // Возврат каретки снимается: опись могли открыть Блокнотом.
        std::string name = line.substr(space + 1);
        while (!name.empty() && (name.back() == '\r' || name.back() == '\n')) {
            name.pop_back();
        }

        if (!name.empty()) {
            laid_[std::move(name)] = line.substr(0, space);
        }
    }
}

void ResourceCache::writeIndex() const {
    std::ofstream file{directory_ / kIndexName, std::ios::binary | std::ios::trunc};
    if (!file) {
        return;
    }

    for (const auto& [name, hash] : laid_) {
        file << hash << ' ' << name << '\n';
    }
}

std::vector<ResourceCache::Ready> ResourceCache::sync(
    const std::string& serverAddress, std::uint16_t serverPort,
    const std::vector<shared::ResourceEntry>& wanted, const Progress& report) {
    std::vector<Ready> ready;

    if (wanted.empty()) {
        return ready;
    }

    const std::vector<std::uint8_t> key = shared::Vault::builtInKey();

    spdlog::info("Server offers {} resource files", wanted.size());

    std::size_t done = 0;

    for (const shared::ResourceEntry& entry : wanted) {
        // Куда лечь — решает вид имени.
        //
        // Игровой файл (`amgone.rpf`) ложится под отпечатком: имя у разных
        // серверов может совпасть при разном содержимом, отпечаток — нет.
        //
        // Файл ресурса (`main/client/index.cjs`) ложится деревом, под своим
        // настоящим именем, и иначе нельзя. Ресурс — это не набор отдельных
        // файлов, а дерево: точка входа делает `require('./утилиты')`, страница
        // тянет `./assets/app.js`. Разложи мы их под отпечатками — ни один из
        // этих путей не нашёл бы ничего, а поправить их некому: писал их
        // хозяин сервера под alt:V, где дерево сохраняется.
        const bool partOfResource = entry.name.find('/') != std::string::npos;

        const std::filesystem::path unpacked =
            partOfResource ? resourceRoot() / entry.name
                           : directory_ / (entry.hash + "-" + entry.name);

        std::error_code ec;

        // У дерева одного существования файла мало: имя не говорит о содержимом
        // ничего, и сервер мог пересобрать ресурс. Отвечает опись.
        const auto laid = laid_.find(entry.name);

        const bool cached =
            std::filesystem::exists(unpacked, ec) &&
            (!partOfResource || (laid != laid_.end() && laid->second == entry.hash));

        if (report) {
            report(done, wanted.size(), !cached);
        }
        ++done;

        if (cached) {
            spdlog::debug("resource file {} is already cached", entry.name);
            ready.push_back(Ready{.name = entry.name, .path = unpacked});
            continue;
        }

        const std::string url = std::format("http://{}:{}/resources/dlcpacks/{}.resource",
                                            serverAddress, serverPort, entry.hash);

        spdlog::debug("downloading {} ({} KB)", entry.name, entry.size / 1024);

        std::string error;
        const std::vector<std::uint8_t> packed = download(url, entry.size, error);

        if (packed.empty()) {
            spdlog::error("resource file {} failed to download: {}", entry.name, error);
            continue;
        }

        // Проверка до расшифровки, а не после: отпечаток объявлен для
        // зашифрованного, и сверять его надо с тем, что пришло по проводу.
        if (const std::string actual = shared::fingerprint(packed); actual != entry.hash) {
            spdlog::error("resource file {} arrived corrupted: checksum mismatch", entry.name);
            continue;
        }

        const std::vector<std::uint8_t> plain = shared::Vault::unpack(packed, key, error);

        // Пустота считается отказом только если о нём сказано. Пустой файл —
        // файл законный: в собранной странице интерфейса такие есть, и отвергать
        // их значило бы оставлять на странице дыру без единого слова о причине.
        if (plain.empty() && !error.empty()) {
            spdlog::error("resource file {} could not be opened: {}", entry.name, error);
            continue;
        }

        if (!writeFile(unpacked, plain)) {
            spdlog::error("resource file {} could not be written to the cache", entry.name);
            continue;
        }

        laid_[entry.name] = entry.hash;

        spdlog::debug("resource file {} is ready", entry.name);
        ready.push_back(Ready{.name = entry.name, .path = unpacked});
    }

    // Опись пишется один раз, в конце: полторы тысячи записей на диск после
    // каждого файла стоили бы дороже самой закачки.
    writeIndex();

    // Последний доклад — о том, что разобраны все. Без него счётчик остановился
    // бы на предпоследнем: доклад идёт перед работой, а не после неё, потому что
    // закачка длится минутами и сказать о ней нужно до, а не потом.
    if (report) {
        report(wanted.size(), wanted.size(), false);
    }

    return ready;
}

} // namespace oxymp::client
