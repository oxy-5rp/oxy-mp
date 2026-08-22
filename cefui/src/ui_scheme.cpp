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

void registerResourceScheme(const std::filesystem::path& directory) {
    // Тот же обработчик, что и у меню, но без встроенной страницы: здесь всё
    // приходит с диска, из того, что клиент скачал у сервера.
    if (!CefRegisterSchemeHandlerFactory("http", "resource",
                                         new UiSchemeFactory{std::string{}, directory})) {
        spdlog::error("the http://resource scheme was not registered: mode pages will not open");
        return;
    }

    spdlog::debug("страницы ресурсов берутся из {}", directory.string());
}

} // namespace oxymp::cefui
