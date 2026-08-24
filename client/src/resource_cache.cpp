#include "resource_cache.hpp"

#include <oxymp/shared/resource/bundle.hpp>
#include <oxymp/shared/resource/vault.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <span>
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

/// Сколько свёрток лежит в кеше, если к нему не обращаются.
///
/// Две недели. Не меньше — человек играет на нескольких серверах и возвращается
/// на прежний через неделю, а стёртый свёрток означает повторную закачку в сотню
/// мегабайт. И не больше — иначе кеш растёт от каждого обновления режима, и
/// растёт незаметно.
constexpr auto kBundleLifetime = std::chrono::hours{24 * 14};

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

ResourceCache::ResourceCache(std::filesystem::path directory)
    : directory_(std::move(directory)),
      bundles_(std::make_shared<BundleStore>(directory_ / "bundles")) {
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

bool ResourceCache::ensureBundle(const std::string& serverAddress, std::uint16_t serverPort,
                                 const std::string& hash, const std::string& resource,
                                 std::map<std::string, bool>& attempted) {
    // Ключ у свёртка тот же, что и у всего остального: он один на сборку.
    const std::vector<std::uint8_t> key = shared::Vault::builtInKey();

    // Пара «свёрток и ресурс», а не один свёрток: один и тот же свёрток может
    // достаться двум ресурсам только по совпадению содержимого, но привязать его
    // надо к обоим, а качать — один раз.
    const std::string mark = hash + ' ' + resource;

    if (const auto known = attempted.find(mark); known != attempted.end()) {
        return known->second;
    }

    const auto remember = [&attempted, &mark](bool outcome) {
        attempted.emplace(mark, outcome);
        return outcome;
    };

    if (!bundles_->has(hash)) {
        const std::string url = std::format("http://{}:{}/resources/dlcpacks/{}.resource",
                                            serverAddress, serverPort, hash);

        spdlog::debug("downloading bundle {} of resource {}", hash, resource);

        std::string error;

        // Размер заранее неизвестен: в списке названы файлы, а не свёрток. Предел
        // тот же, что и у прочей закачки, — его назначает download.
        const std::vector<std::uint8_t> packed = download(url, 0, error);

        if (packed.empty()) {
            spdlog::error("bundle {} failed to download: {}", hash, error);
            return remember(false);
        }

        if (const std::string actual = shared::fingerprint(packed); actual != hash) {
            spdlog::error("bundle {} arrived corrupted: checksum mismatch", hash);
            return remember(false);
        }

        if (!shared::Bundle::readHeader(packed).has_value()) {
            spdlog::error("bundle {} is not a bundle", hash);
            return remember(false);
        }

        if (!bundles_->keep(hash, packed)) {
            return remember(false);
        }
    }

    return remember(bundles_->bind(resource, hash, key));
}

void ResourceCache::sweep(const std::set<std::string>& sealed,
                          const std::set<std::string>& offered) {
    if (sealed.empty()) {
        return;
    }

    std::error_code ec;
    std::size_t gone = 0;

    for (auto item = laid_.begin(); item != laid_.end();) {
        const std::size_t slash = item->first.find('/');

        // Имя без косой черты — игровой файл, а не часть ресурса: он лежит под
        // отпечатком и к этой уборке отношения не имеет.
        if (slash == std::string::npos || offered.contains(item->first) ||
            !sealed.contains(item->first.substr(0, slash))) {
            ++item;
            continue;
        }

        if (std::filesystem::remove(resourceRoot() / item->first, ec)) {
            ++gone;
        }

        item = laid_.erase(item);
    }

    if (gone != 0) {
        spdlog::info("Removed {} stale files left by an earlier version of a resource", gone);
    }
}

void ResourceCache::shed(const std::string& name) {
    const auto laid = laid_.find(name);

    // Опись — единственное, что помнит о разложенном. Не значившегося в ней мы
    // не раскладывали, и трогать чужое в кеше незачем.
    if (laid == laid_.end()) {
        return;
    }

    std::error_code ec;

    if (std::filesystem::remove(resourceRoot() / name, ec)) {
        spdlog::debug("{} no longer lies on the disk: it is read from the bundle", name);
    }

    laid_.erase(laid);
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

    // Разобранные свёртки этого обхода: качать один свёрток по разу на каждый
    // лежащий в нём файл значило бы четыре тысячи закачек вместо одной.
    std::map<std::string, bool> attempted;

    // Что сервер предложил и у каких ресурсов есть свёрток — по ним в конце
    // подчищаются остатки прошлых сборок режима.
    std::set<std::string> offered;
    std::set<std::string> sealed;

    for (const shared::ResourceEntry& entry : wanted) {
        const std::size_t slash = entry.name.find('/');

        offered.insert(entry.name);

        // Файл, лежащий в свёртке, на диск не ложится вовсе — ради этого свёрток
        // и заведён. Клиент кладёт себе свёрток целиком и читает из него по
        // одному файлу, когда спросят: исходники режима так и не становятся у
        // игрока обычным текстом.
        //
        // Составное имя обязательно: в свёртке лежат части ресурсов, а имя без
        // косой черты — это игровой файл, и свёртка у него быть не может.
        if (!entry.bundle.empty() && slash != std::string::npos) {
            const std::string resource = entry.name.substr(0, slash);

            // Закачкой считается только та, которой ещё не было: свёрток уже
            // лежащий здесь берётся мгновенно, и говорить о нём «качаем» значило
            // бы показывать человеку закачку там, где её нет.
            if (report) {
                report(done, wanted.size(), !bundles_->has(entry.bundle));
            }
            ++done;

            if (!ensureBundle(serverAddress, serverPort, entry.bundle, resource, attempted)) {
                continue;
            }

            shed(entry.name);
            sealed.insert(resource);

            ready.push_back(Ready{.name = entry.name, .path = {}, .bundled = true});
            continue;
        }

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
        const bool partOfResource = slash != std::string::npos;

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

    // Остатки прошлых сборок режима — до записи описи: она пишется один раз, и
    // выброшенное отсюда не должно в неё попасть.
    sweep(sealed, offered);

    // Свёртки, которыми давно не пользовались, уезжают. Здесь, а не при заводе
    // кеша: к этому мгновению известно, какие из них нужны прямо сейчас, — а до
    // разбора списка нужным выглядит всякий.
    bundles_->prune(kBundleLifetime);

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
