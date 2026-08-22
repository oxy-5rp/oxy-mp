// oxymp-rglpatch — модуль, который лаунчер oxyMP внедряет в Rockstar Games
// Launcher.
//
// Делает ровно одно: подменяет звено BattlEye в цепочке запуска игры и говорит
// своим, какой процесс получился. Всё остальное — дело лаунчера oxyMP: здесь мы
// в чужом процессе, и чем меньше мы в нём делаем, тем меньше вероятность
// уронить его вместе с игрой.

#include "battleye_link.hpp"

#include <oxymp/shared/launch/handoff.hpp>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <windows.h>

namespace {

HANDLE g_mapping = nullptr;
oxymp::shared::LaunchHandoff* g_handoff = nullptr;
std::unique_ptr<oxymp::rglpatch::BattlEyeLink> g_link;

/// Открывает общий с лаунчером блок.
///
/// Открывает, а не создаёт: блок заводит лаунчер до внедрения, потому что путь
/// к журналу он выставляет туда же. Не найдя блока, работать незачем — сообщить
/// о сделанном будет некому.
bool openHandoff() {
    g_mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, oxymp::shared::kLaunchHandoffName);
    if (g_mapping == nullptr) {
        return false;
    }

    g_handoff = static_cast<oxymp::shared::LaunchHandoff*>(::MapViewOfFile(
        g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(oxymp::shared::LaunchHandoff)));

    return g_handoff != nullptr;
}

void publish(oxymp::shared::LaunchState state) {
    if (g_handoff == nullptr) {
        return;
    }

    g_handoff->state.store(static_cast<std::uint32_t>(state));
    g_handoff->generation.fetch_add(1);
}

void publishFailure(const std::string& error) {
    if (g_handoff == nullptr) {
        return;
    }

    const std::size_t length = std::min(error.size(), sizeof(g_handoff->error) - 1);
    std::memcpy(g_handoff->error, error.data(), length);
    g_handoff->error[length] = '\0';

    publish(oxymp::shared::LaunchState::Failed);
}

/// Направляет журнал в файл рядом с журналом лаунчера.
///
/// Консоли у Rockstar Games Launcher нет, и своей завести нельзя: это чужое
/// окно. Файл здесь единственный способ узнать, почему подмена не сработала.
void setUpLogging() {
    if (g_handoff == nullptr || g_handoff->logDirectory[0] == L'\0') {
        return;
    }

    try {
        const std::filesystem::path path =
            std::filesystem::path{g_handoff->logDirectory} / "rglpatch.log";

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        auto logger = spdlog::basic_logger_mt("rglpatch", path.string(), true);
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        logger->flush_on(spdlog::level::debug);
        spdlog::set_default_logger(std::move(logger));
    } catch (const spdlog::spdlog_ex&) {
        // Без журнала подмена работает, отказываться от неё из-за журнала — нет.
    }
}

/// Отдельный поток, а не тело DllMain.
///
/// В DllMain держится замок загрузчика, и всё, что трогает другие модули, —
/// чтение таблицы импорта, открытие файлов, spdlog — способно там встать
/// намертво. Лаунчер oxyMP всё равно дожидается готовности через общий блок,
/// поэтому задержка на создание потока ничего не стоит.
DWORD WINAPI worker(LPVOID) {
    if (!openHandoff()) {
        return 1;
    }

    setUpLogging();

    spdlog::debug("модуль в Rockstar Games Launcher, ставим подмену");

    std::string error;

    g_link = oxymp::rglpatch::BattlEyeLink::install(
        [](std::uint32_t processId) {
            if (g_handoff == nullptr) {
                return;
            }

            g_handoff->gameProcessId.store(processId);
            publish(oxymp::shared::LaunchState::GameStarted);
        },
        [] {
            oxymp::rglpatch::BattlEyeLink::Order order;
            if (g_handoff == nullptr) {
                return order;
            }

            // exchange, а не load: заказ снимается тем же действием, каким
            // читается, и второй запуск его уже не застанет.
            order.substitute = g_handoff->armed.exchange(0) != 0;
            order.straightIntoFreemode = g_handoff->straightIntoFreemode.load() != 0;
            order.language = g_handoff->gameLanguage;

            return order;
        },
        error);

    if (g_link == nullptr) {
        spdlog::error("the patch could not be installed: {}", error);
        publishFailure(error);
        return 1;
    }

    spdlog::debug("подмена стоит: запуск игры пойдёт мимо BattlEye");
    publish(oxymp::shared::LaunchState::Hooked);

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        // Уведомления о потоках нам не нужны, а лаунчер их создаёт много.
        ::DisableThreadLibraryCalls(module);

        const HANDLE thread = ::CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr);
        if (thread != nullptr) {
            ::CloseHandle(thread);
        }
        return TRUE;
    }

    case DLL_PROCESS_DETACH:
        // reserved не ноль означает, что процесс заканчивается целиком: снимать
        // перехват уже некому и незачем, а трогать что-либо в этот момент —
        // верный способ уронить чужое завершение.
        if (reserved == nullptr) {
            g_link.reset();

            if (g_handoff != nullptr) {
                ::UnmapViewOfFile(g_handoff);
                g_handoff = nullptr;
            }
            if (g_mapping != nullptr) {
                ::CloseHandle(g_mapping);
                g_mapping = nullptr;
            }
        }
        return TRUE;

    default:
        return TRUE;
    }
}
