#include "ui_scheme.hpp"

#include <include/cef_parser.h>
#include <include/cef_scheme.h>
#include <include/wrapper/cef_stream_resource_handler.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

namespace oxymp::cefui {
namespace {

/// Что отдавать, когда в ссылке не указано ничего.
constexpr const char* kIndex = "index.html";

/// Выдаёт файлы из одного каталога и только из него.
class UiSchemeFactory : public CefSchemeHandlerFactory {
public:
    explicit UiSchemeFactory(std::filesystem::path directory)
        : directory_{std::move(directory)} {}

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString&,
                                         CefRefPtr<CefRequest> request) override {
        const std::filesystem::path file = resolve(request->GetURL().ToString());
        if (file.empty()) {
            return nullptr;
        }

        const CefRefPtr<CefStreamReader> reader =
            CefStreamReader::CreateForFile(file.string());

        if (reader == nullptr) {
            spdlog::warn("страница просит {}, а его нет", file.string());
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
            spdlog::warn("страница просит {} — это вне её каталога", path);
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

    std::filesystem::path directory_;

    IMPLEMENT_REFCOUNTING(UiSchemeFactory);
};

} // namespace

void registerUiScheme(const std::filesystem::path& directory) {
    // `http` с именем узла `ui`, а не своя схема, и это важнее, чем кажется.
    // Своей схеме браузер не даёт ни локального хранилища, ни разбора
    // происхождения: страница считалась бы пришедшей ниоткуда, и половина её
    // настроек не сохранилась бы между запусками.
    if (!CefRegisterSchemeHandlerFactory("http", "ui", new UiSchemeFactory{directory})) {
        spdlog::error("не удалось завести схему http://ui — меню не откроется");
        return;
    }

    spdlog::debug("http://ui отдаётся из {}", directory.string());
}

} // namespace oxymp::cefui
