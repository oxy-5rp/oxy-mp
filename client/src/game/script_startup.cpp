#include "script_startup.hpp"

#include <spdlog/spdlog.h>

#include <atomic>

namespace oxymp::client::game {
namespace {

constexpr std::string_view kStartupId = "script_startup_init";

/// Тип перехватываемой функции.
///
/// Аргументов она не принимает: разбор её начала показывает, что входящие
/// rcx/rdx/r8 не читаются — функция сразу заполняет их сама. Возврат объявлен
/// числом, а не void, чтобы значение в rax дошло до вызывающего в целости,
/// какой бы смысл в него ни вкладывала игра.
using StartupFunction = std::uint64_t (*)();

/// Установленный перехват. Он один на процесс, поэтому и указатель один:
/// обработчику иначе неоткуда узнать, что именно он подменил.
StartupFunction g_original = nullptr;

GameScripts g_scripts = GameScripts::Run;

std::atomic<unsigned int> g_invocations{0};

std::uint64_t detour() {
    const unsigned int count = g_invocations.fetch_add(1) + 1;

    // Работы здесь намеренно нет никакой, кроме записи в журнал: обработчик
    // выполняется в главном потоке игры, и всё тяжёлое отсюда отзовётся
    // подвисанием картинки.
    if (g_scripts == GameScripts::Block) {
        spdlog::debug("стартовые скрипты игры не запущены (раз {}) — сюжета не будет", count);

        // Возврат нуля безопасен: разбор функции показывает, что на одной из
        // веток она уходит на эпилог, не выставив rax вовсе. Значение её
        // вызывающему не нужно.
        return 0;
    }

    spdlog::debug("игра запускает стартовые скрипты (раз {})", count);

    if (g_original == nullptr) {
        return 0;
    }

    return g_original();
}

} // namespace

std::unique_ptr<ScriptStartup> ScriptStartup::install(const EngineAddresses& addresses,
                                                      GameScripts scripts, std::string& error) {
    auto* target = addresses.pointerTo<void*>(kStartupId);
    if (target == nullptr) {
        error = "адрес запуска стартовых скриптов не разрешён";
        return nullptr;
    }

    std::unique_ptr<ScriptStartup> startup{new ScriptStartup};

    // Решение выставляется до постановки перехвата: после неё обработчик может
    // быть вызван в любое мгновение.
    g_scripts = scripts;
    g_invocations.store(0);

    if (!startup->hook_.install(target, reinterpret_cast<void*>(&detour), error)) {
        return nullptr;
    }

    g_original = startup->hook_.original<StartupFunction>();

    spdlog::debug("перехват запуска скриптов поставлен на {:#x}, режим: {}",
                 reinterpret_cast<std::uintptr_t>(target),
                 scripts == GameScripts::Block ? "не пускать" : "пускать");

    return startup;
}

ScriptStartup::~ScriptStartup() {
    // Порядок важен: пока перехват стоит, обработчик может быть вызван в любой
    // момент и обязан знать, что вызывать. Поэтому сперва снятие, и только
    // после него — забывание оригинала.
    hook_.remove();
    g_original = nullptr;
    g_scripts = GameScripts::Run;
}

unsigned int ScriptStartup::invocations() const noexcept {
    return g_invocations.load();
}

} // namespace oxymp::client::game
