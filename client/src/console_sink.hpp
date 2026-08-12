#pragma once

#include "ui_feed.hpp"

#include <spdlog/sinks/base_sink.h>

#include <mutex>
#include <string>

namespace oxymp::client {

/// Ещё один получатель журнала: тот же поток сообщений, но в игровую консоль.
///
/// Заводится ради F8. Всё, что клиент пишет в файл, — опознание движка, ошибки
/// нативов, вход и выход игроков — интересно не только после игры, но и во время
/// неё, а лезть в файл, не выходя из игры, невозможно.
///
/// Отдельным получателем, а не вызовами из кода: иначе каждое место, которому
/// есть что сказать, пришлось бы учить говорить дважды — и рано или поздно
/// половина сообщений оказалась бы только в файле.
class ConsoleSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ConsoleSink(UiFeed& feed) noexcept : feed_(feed) {}

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        // Берётся уже собранный текст без оформления: рамку, время и цвет
        // рисует страница, и присланное ею же оформление она бы не разобрала.
        feed_.pushConsole(static_cast<unsigned int>(message.level),
                          std::string{message.payload.data(), message.payload.size()});
    }

    void flush_() override {}

private:
    UiFeed& feed_;
};

} // namespace oxymp::client
