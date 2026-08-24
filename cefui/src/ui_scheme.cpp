#include "ui_scheme.hpp"

#include <include/cef_parser.h>
#include <include/cef_scheme.h>
#include <include/wrapper/cef_byte_read_handler.h>
#include <include/wrapper/cef_stream_resource_handler.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oxymp::cefui {
namespace {

/// Что отдавать, когда в ссылке не указано ничего.
constexpr const char* kIndex = "index.html";

/// Выдаёт страницу из памяти, а её соседей — из одного каталога и только из него.
class UiSchemeFactory : public CefSchemeHandlerFactory {
public:
    UiSchemeFactory(std::string page, std::filesystem::path directory)
        : page_{std::move(page)}, directory_{std::move(directory)} {}

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString&,
                                         CefRefPtr<CefRequest> request) override {
        const std::filesystem::path file = resolve(request->GetURL().ToString());
        if (file.empty()) {
            return nullptr;
        }

        // Сама страница отдаётся из памяти: она лежит ресурсом внутри модуля, а
        // не файлом рядом с ним. Копия на каждый показ здесь не страшна —
        // показов ровно один за запуск.
        if (!page_.empty() && file.filename() == kIndex) {
            const CefRefPtr<CefStreamReader> reader = CefStreamReader::CreateForData(
                const_cast<char*>(page_.data()), page_.size());

            return new CefStreamResourceHandler{CefString{"text/html"}, reader};
        }

        const CefRefPtr<CefStreamReader> reader =
            CefStreamReader::CreateForFile(file.string());

        if (reader == nullptr) {
            // Отладочным уровнем, а не предупреждением: браузер сам просит
            // favicon.ico у всякой страницы, и своего у нас нет. Предупреждение
            // об этом повторялось на каждый переход и забивало журнал ровно там,
            // где в него смотрят.
            spdlog::debug("страница просит {}, а его нет", file.string());
            return nullptr;
        }

        return new CefStreamResourceHandler{mimeType(file), reader};
    }

private:
    /// Превращает ссылку в путь внутри каталога. Пустой путь означает отказ.
    [[nodiscard]] std::filesystem::path resolve(const std::string& url) const {
        CefURLParts parts;
        if (!CefParseURL(url, parts)) {
            return {};
        }

        std::string path = CefString{&parts.path}.ToString();

        // Доводы и якорь до файла не относятся: `index.html#/servers` — это всё
        // тот же index.html, и открывать надо его.
        for (const char separator : {'?', '#'}) {
            if (const std::size_t at = path.find(separator); at != std::string::npos) {
                path.resize(at);
            }
        }

        while (!path.empty() && path.front() == '/') {
            path.erase(0, 1);
        }

        if (path.empty()) {
            path = kIndex;
        }

        // Ссылка ведёт в каталог страницы и никуда больше. Проверка счётом, а не
        // просмотром пути на точки: `..` можно записать по-разному, а
        // приведённый к каноническому виду путь либо лежит внутри, либо нет.
        std::error_code ec;

        const std::filesystem::path inside =
            std::filesystem::weakly_canonical(directory_ / path, ec);
        if (ec) {
            return {};
        }

        const std::filesystem::path root = std::filesystem::weakly_canonical(directory_, ec);
        if (ec) {
            return {};
        }

        const auto shared = std::mismatch(root.begin(), root.end(), inside.begin(), inside.end());
        if (shared.first != root.end()) {
            spdlog::warn("the page asks for {}: that is outside its directory", path);
            return {};
        }

        return inside;
    }

    [[nodiscard]] static CefString mimeType(const std::filesystem::path& file) {
        std::string extension = file.extension().string();
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(0, 1);
        }

        const std::string known = CefGetMimeType(extension).ToString();

        // Неизвестное отдаётся потоком байтов. Не «текстом»: угадав неверно,
        // браузер показал бы картинку как набор знаков.
        return known.empty() ? CefString{"application/octet-stream"} : CefString{known};
    }

    /// Сама страница. Пусто — значит её не встроили, и берётся она с диска.
    std::string page_;

    std::filesystem::path directory_;

    IMPLEMENT_REFCOUNTING(UiSchemeFactory);
};

/// Способ читать файлы ресурсов и замок к нему.
///
/// Ставится из сетевого потока, читается из потока ввода-вывода Chromium.
/// Копия под замком, а не ссылка: обработчик живёт долго, а поставить новый
/// могут посреди его работы.
std::mutex& readerLock() {
    static std::mutex lock;
    return lock;
}

ResourceReader& readerSlot() {
    static ResourceReader reader;
    return reader;
}

[[nodiscard]] ResourceReader currentReader() {
    const std::lock_guard guard{readerLock()};
    return readerSlot();
}

/// Держатель байтов, которые отданы Chromium.
///
/// Нужен потому, что `CefByteReadHandler` память не присваивает — он берёт
/// указатель и того, кто её держит. Держим здесь: страница читает свой файл не
/// мгновенно, а кусками, и временный буфер к третьему куску был бы уже мёртв.
class BytesHolder : public CefBaseRefCounted {
public:
    explicit BytesHolder(std::vector<std::uint8_t> data) : data_{std::move(data)} {}

    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }

private:
    std::vector<std::uint8_t> data_;

    IMPLEMENT_REFCOUNTING(BytesHolder);
};

/// Отдаёт файлы ресурсов: сперва из свёртка, потом с диска.
///
/// Порядок именно такой. В свёртке лежит то, что читает наш же клиент, —
/// исходники и страницы; на диске остаётся то, что читает сама игра, — модели и
/// звуки. Страница вправе попросить и то и другое.
class ResourceSchemeFactory : public CefSchemeHandlerFactory {
public:
    explicit ResourceSchemeFactory(std::filesystem::path directory)
        : directory_{std::move(directory)} {}

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString&,
                                         CefRefPtr<CefRequest> request) override {
        const std::string path = pathOf(request->GetURL().ToString());
        if (path.empty()) {
            return nullptr;
        }

        // Первый кусок пути — имя ресурса, остальное — путь внутри него. Так
        // страницы режимов и написаны под alt:V, и делить иначе нельзя: имена
        // файлов у разных ресурсов совпадают сплошь и рядом.
        const std::size_t slash = path.find('/');
        if (slash == std::string::npos) {
            return nullptr;
        }

        const std::string_view resource{path.data(), slash};
        const std::string_view file{path.data() + slash + 1, path.size() - slash - 1};

        if (file.empty()) {
            return nullptr;
        }

        if (const ResourceReader reader = currentReader(); reader) {
            std::vector<std::uint8_t> contents;

            if (reader(resource, file, contents)) {
                const CefRefPtr<BytesHolder> holder = new BytesHolder{std::move(contents)};

                const CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForHandler(
                    new CefByteReadHandler{holder->data().data(), holder->data().size(), holder});

                return new CefStreamResourceHandler{mimeType(std::filesystem::path{file}), stream};
            }
        }

        const std::filesystem::path onDisk = resolve(path);
        if (onDisk.empty()) {
            return nullptr;
        }

        const CefRefPtr<CefStreamReader> reader = CefStreamReader::CreateForFile(onDisk.string());

        if (reader == nullptr) {
            // Отладочным уровнем: браузер сам просит favicon.ico у всякой
            // страницы, и своего у ресурса обычно нет.
            spdlog::debug("страница ресурса просит {}, а его нет нигде", path);
            return nullptr;
        }

        return new CefStreamResourceHandler{mimeType(onDisk), reader};
    }

private:
    /// Путь из ссылки, без доводов и якоря. Пусто — ссылка не разобралась.
    [[nodiscard]] static std::string pathOf(const std::string& url) {
        CefURLParts parts;
        if (!CefParseURL(url, parts)) {
            return {};
        }

        std::string path = CefString{&parts.path}.ToString();

        for (const char separator : {'?', '#'}) {
            if (const std::size_t at = path.find(separator); at != std::string::npos) {
                path.resize(at);
            }
        }

        while (!path.empty() && path.front() == '/') {
            path.erase(0, 1);
        }

        // Ссылка приходит в виде, пригодном для адреса: пробелы и кириллица в
        // ней записаны процентами. В свёртке же имя лежит таким, каким его
        // написал автор ресурса.
        return CefURIDecode(path, true, static_cast<cef_uri_unescape_rule_t>(
                                            UU_SPACES | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS))
            .ToString();
    }

    /// Путь на диске, если он лежит внутри каталога. Пусто — отказ.
    [[nodiscard]] std::filesystem::path resolve(const std::string& path) const {
        std::error_code ec;

        const std::filesystem::path inside =
            std::filesystem::weakly_canonical(directory_ / path, ec);
        if (ec) {
            return {};
        }

        const std::filesystem::path root = std::filesystem::weakly_canonical(directory_, ec);
        if (ec) {
            return {};
        }

        // Проверка счётом, а не просмотром пути на точки: `..` можно записать
        // по-разному, а приведённый к каноническому виду путь либо лежит внутри,
        // либо нет.
        const auto shared = std::mismatch(root.begin(), root.end(), inside.begin(), inside.end());
        if (shared.first != root.end()) {
            spdlog::warn("the page asks for {}: that is outside its directory", path);
            return {};
        }

        return inside;
    }

    [[nodiscard]] static CefString mimeType(const std::filesystem::path& file) {
        std::string extension = file.extension().string();
        if (!extension.empty() && extension.front() == '.') {
            extension.erase(0, 1);
        }

        const std::string known = CefGetMimeType(extension).ToString();

        return known.empty() ? CefString{"application/octet-stream"} : CefString{known};
    }

    std::filesystem::path directory_;

    IMPLEMENT_REFCOUNTING(ResourceSchemeFactory);
};

} // namespace

void registerUiScheme(std::string page, const std::filesystem::path& directory) {
    const std::size_t size = page.size();

    // `http` с именем узла `ui`, а не своя схема, и это важнее, чем кажется.
    // Своей схеме браузер не даёт ни локального хранилища, ни разбора
    // происхождения: страница считалась бы пришедшей ниоткуда, и половина её
    // настроек не сохранилась бы между запусками.
    if (!CefRegisterSchemeHandlerFactory("http", "ui",
                                         new UiSchemeFactory{std::move(page), directory})) {
        spdlog::error("the http://ui scheme was not registered: the menu will not open");
        return;
    }

    if (size != 0) {
        spdlog::debug("страница меню взята из модуля: {} КБ", size / 1024);
    } else {
        spdlog::warn("the menu page is not in the module: taking it from {}", directory.string());
    }
}

void setResourceReader(ResourceReader reader) {
    const std::lock_guard guard{readerLock()};
    readerSlot() = std::move(reader);
}

void registerResourceScheme(const std::filesystem::path& directory) {
    if (!CefRegisterSchemeHandlerFactory("http", "resource",
                                         new ResourceSchemeFactory{directory})) {
        spdlog::error("the http://resource scheme was not registered: mode pages will not open");
        return;
    }

    spdlog::debug("страницы ресурсов берутся из свёртков, а недостающее — из {}",
                  directory.string());
}

} // namespace oxymp::cefui
