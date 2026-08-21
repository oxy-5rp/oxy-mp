#pragma once

#include <oxymp/client/js/abi.h>

#include <node.h>
#include <v8.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oxymp::client::js {

/// Один поднятый клиентский ресурс.
///
/// Устроен так же, как серверный (`script-js/src/resource.hpp`): свой изолят,
/// своя куча, свой цикл событий. Причина та же — ресурсы пишут разные люди, и
/// уронивший свою кучу не должен уносить чужие, — но здесь у неё есть довесок:
/// куча живёт внутри процесса игры, и её падение уносит не сессию, а игру.
///
/// Ни копировать, ни переносить нельзя: внутри указатели, которые V8 раздал
/// наружу.
class Resource final {
public:
    Resource(std::string name, std::filesystem::path root, const OxympJsHost& host,
             node::MultiIsolatePlatform& platform);
    ~Resource();

    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    /// Заводит изолят, ставит привязки и исполняет точку входа. false — отказ,
    /// объяснение уже ушло в журнал клиента.
    [[nodiscard]] bool start(const std::filesystem::path& entry);

    /// Даёт движку поработать: таймеры, обещания, ввод-вывод.
    void pump();

    /// Зовёт обработчики события с уже готовыми доводами-строками.
    ///
    /// Нагрузка передаётся строкой, а не разобранным значением, и разбирает её
    /// сам слой alt на JavaScript. Так сделано потому, что сервер её строкой и
    /// шлёт: `alt_server.js` укладывает доводы в JSON, и разбирать их дважды —
    /// здесь в C++ и там в JS — значило бы держать два разбора одного формата.
    void dispatch(std::string_view name, std::string_view payload);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const OxympJsHost& host() const noexcept { return *host_; }

    /// Ресурс, которому принадлежит изолят.
    [[nodiscard]] static Resource* of(v8::Isolate* isolate) noexcept;

    /// Запоминает обработчик события. Зовётся из привязки.
    void subscribe(std::string name, v8::Local<v8::Function> handler);

    /// Пишет строку в журнал клиента через границу.
    void log(OxympJsLogLevel level, std::string_view line) const;

    /// Запоминает окно, заведённое этим ресурсом, и забывает убранное.
    ///
    /// Помнить обязательно: окно живёт не в изоляте, а в слое интерфейса
    /// клиента, и разбор изолята его не трогает. Ресурс, остановленный без
    /// уборки, оставил бы свою страницу висеть в кадре игры до конца запуска —
    /// и убрать её было бы уже нечем: того, кто её завёл, больше нет.
    void rememberView(std::uint32_t view);
    void forgetView(std::uint32_t view);

private:
    /// Окна, которые ресурс завёл и ещё не убрал.
    std::vector<std::uint32_t> views_;

    /// Слот в изоляте, в котором лежит указатель на ресурс.
    ///
    /// Первый, а не нулевой, по той же причине, что и на сервере: нулевой —
    /// первый кандидат для всякого, кто однажды решит слотами воспользоваться.
    static constexpr std::uint32_t kIsolateSlot = 1;

    /// Пишет в журнал то, что скрипт не поймал сам.
    void report(v8::Local<v8::Context> context, const v8::TryCatch& caught) const;

    /// Дожидается, пока исполнится точка входа. false — она отказала.
    ///
    /// Точка входа грузится обещанием (см. alt_client_bootstrap.js), и к концу
    /// LoadEnvironment ещё не исполнена. Ждать приходится прокруткой цикла
    /// событий — другого способа сдвинуть обещание с места нет.
    [[nodiscard]] bool awaitStart(v8::Local<v8::Context> context, v8::Isolate* isolate);

    std::string name_;
    std::filesystem::path root_;
    const OxympJsHost* host_ = nullptr;
    node::MultiIsolatePlatform* platform_ = nullptr;

    std::unique_ptr<node::CommonEnvironmentSetup> setup_;

    /// Обработчики по имени события. Порядок подписки сохраняется.
    std::unordered_map<std::string, std::vector<v8::Global<v8::Function>>> handlers_;
};

} // namespace oxymp::client::js
