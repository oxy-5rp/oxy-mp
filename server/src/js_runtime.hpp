#pragma once

#include "runtime.hpp"

#include <oxymp/script/js/engine.hpp>

#include <memory>
#include <string>

namespace oxymp::server {

/// Переходник между сервером и движком JavaScript.
///
/// Всё, что он делает, — переводит `ScriptResource` в то, что понимает движок:
/// имя, каталог, точку входа. Больше в нём ничего и быть не должно.
///
/// Живёт он здесь, а не в самом движке, и это граница знания, а не мелочь.
/// `ScriptResource` — понятие сервера: в нём путь на диске, разобранное
/// описание и список раздаваемых клиенту файлов. Движку из всего этого нужны три
/// поля, и знать про остальное ему незачем — иначе он потянул бы за собой
/// настройки сервера, каталог ресурсов и правила их проверки.
class JsRuntime final : public Runtime {
public:
    /// Поднимает движок. nullptr — с объяснением в error.
    ///
    /// Отказ здесь не должен останавливать сервер: без движка он лишится
    /// скриптов, но не сессии.
    [[nodiscard]] static std::unique_ptr<JsRuntime> create(script::Core& core,
                                                           script::Events& events,
                                                           std::string& error);

    [[nodiscard]] std::string_view type() const noexcept override { return "js"; }

    bool start(const ScriptResource& resource, std::string& error) override;
    void stop(const ScriptResource& resource) override;

private:
    explicit JsRuntime(std::unique_ptr<script::js::Engine> engine) noexcept;

    std::unique_ptr<script::js::Engine> engine_;
};

} // namespace oxymp::server
