#include "resource_cache.hpp"

#include <oxymp/shared/resource/vault.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>

#include <windows.h>

#include <wininet.h>

namespace oxymp::client {
namespace {

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
        spdlog::warn("каталог кеша {} не создан: {}", directory_.string(), ec.message());
    }
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

std::vector<ResourceCache::Ready> ResourceCache::sync(
    const std::string& serverAddress, std::uint16_t serverPort,
    const std::vector<shared::ResourceEntry>& wanted) {
    std::vector<Ready> ready;

    if (wanted.empty()) {
        return ready;
    }

    const std::vector<std::uint8_t> key = shared::Vault::builtInKey();

    spdlog::info("сервер предлагает ресурсов: {}", wanted.size());

    for (const shared::ResourceEntry& entry : wanted) {
        // Расшифрованное лежит под отпечатком, а не под именем: имя у разных
        // серверов может совпасть при разном содержимом, отпечаток — нет.
        const std::filesystem::path unpacked = directory_ / (entry.hash + "-" + entry.name);

        std::error_code ec;
        if (std::filesystem::exists(unpacked, ec)) {
            spdlog::info("ресурс {} уже есть", entry.name);
            ready.push_back(Ready{.name = entry.name, .path = unpacked});
            continue;
        }

        const std::string url = std::format("http://{}:{}/resources/dlcpacks/{}.resource",
                                            serverAddress, serverPort, entry.hash);

        spdlog::info("качаем {} ({} КБ)", entry.name, entry.size / 1024);

        std::string error;
        const std::vector<std::uint8_t> packed = download(url, entry.size, error);

        if (packed.empty()) {
            spdlog::error("ресурс {} не скачался: {}", entry.name, error);
            continue;
        }

        // Проверка до расшифровки, а не после: отпечаток объявлен для
        // зашифрованного, и сверять его надо с тем, что пришло по проводу.
        if (const std::string actual = shared::fingerprint(packed); actual != entry.hash) {
            spdlog::error("ресурс {} дошёл испорченным: отпечаток не сошёлся", entry.name);
            continue;
        }

        const std::vector<std::uint8_t> plain = shared::Vault::unpack(packed, key, error);

        if (plain.empty()) {
            spdlog::error("ресурс {} не открылся: {}", entry.name, error);
            continue;
        }

        if (!writeFile(unpacked, plain)) {
            spdlog::error("ресурс {} не удалось сохранить в кеш", entry.name);
            continue;
        }

        spdlog::info("ресурс {} готов", entry.name);
        ready.push_back(Ready{.name = entry.name, .path = unpacked});
    }

    return ready;
}

} // namespace oxymp::client
