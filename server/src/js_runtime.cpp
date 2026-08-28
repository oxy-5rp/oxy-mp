#include "js_runtime.hpp"

namespace oxymp::server {

JsRuntime::JsRuntime(std::unique_ptr<script::js::Engine> engine) noexcept
    : engine_(std::move(engine)) {}

std::unique_ptr<JsRuntime> JsRuntime::create(script::Core& core, script::Events& events,
                                             std::string& error) {
    std::unique_ptr<script::js::Engine> engine = script::js::Engine::create(core, events, error);
    if (engine == nullptr) {
        return nullptr;
    }

    // Конструктор закрыт, и make_unique до него не дотянется: заводить движок
    // мимо create нельзя — он обязан получить уже поднятый Node.
    return std::unique_ptr<JsRuntime>{new JsRuntime{std::move(engine)}};
}

bool JsRuntime::start(const ScriptResource& resource, std::string& error) {
    if (resource.main.empty()) {
        error = "не задан main — с какого файла начинать";
        return false;
    }

    return engine_->start(resource.name, resource.root, resource.main, error);
}

bool JsRuntime::stop(const ScriptResource& resource) {
    return engine_->stop(resource.name);
}

} // namespace oxymp::server
