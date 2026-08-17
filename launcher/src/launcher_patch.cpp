#include "launcher_patch.hpp"

#include "process_inject.hpp"
#include "process_lookup.hpp"

#include <oxymp/shared/launch/handoff.hpp>

#include <algorithm>
#include <cwchar>
#include <format>
#include <iterator>

#include <windows.h>

namespace oxymp::launcher {
namespace {

constexpr const wchar_t* kRockstarLauncherName = L"Launcher.exe";

/// Как часто перечитывать состояние подмены.
constexpr auto kPollInterval = std::chrono::milliseconds{100};

shared::LaunchHandoff& handoffOf(void* pointer) {
    return *static_cast<shared::LaunchHandoff*>(pointer);
}

/// Что подмена сообщила о своей неудаче.
std::string failureText(const shared::LaunchHandoff& handoff) {
    const std::string reported = handoff.error;

    return reported.empty() ? "подмена не сообщила причины" : reported;
}

} // namespace

std::unique_ptr<LauncherPatch> LauncherPatch::install(const std::filesystem::path& module,
                                                      const Order& order,
                                                      std::chrono::seconds timeout,
                                                      std::string& error) {
    const std::uint32_t launcher = findProcessByName(kRockstarLauncherName);
    if (launcher == 0) {
        error = "Rockstar Games Launcher не запущен — подменять звено BattlEye негде";
        return nullptr;
    }

    std::unique_ptr<LauncherPatch> patch{new LauncherPatch};

    // Блок заводим мы, а не внедрённый модуль: путь к журналу нужно передать
    // ему до того, как он возьмётся за дело, а изнутри чужого процесса взять
    // этот путь неоткуда.
    patch->mapping_ =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                             sizeof(shared::LaunchHandoff), shared::kLaunchHandoffName);
    if (patch->mapping_ == nullptr) {
        error = std::format("не удалось завести общий блок: код ошибки Windows {}",
                            ::GetLastError());
        return nullptr;
    }

    patch->handoff_ = ::MapViewOfFile(patch->mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                      sizeof(shared::LaunchHandoff));
    if (patch->handoff_ == nullptr) {
        error = std::format("не удалось отобразить общий блок: код ошибки Windows {}",
                            ::GetLastError());
        return nullptr;
    }

    shared::LaunchHandoff& handoff = handoffOf(patch->handoff_);

    const std::wstring logs = order.logDirectory.wstring();
    const std::size_t length = std::min(logs.size(), std::size(handoff.logDirectory) - 1);
    std::wmemcpy(handoff.logDirectory, logs.c_str(), length);
    handoff.logDirectory[length] = L'\0';

    const std::size_t languageLength =
        std::min(order.gameLanguage.size(), std::size(handoff.gameLanguage) - 1);
    std::wmemcpy(handoff.gameLanguage, order.gameLanguage.c_str(), languageLength);
    handoff.gameLanguage[languageLength] = L'\0';

    // Прошлый запуск мог оставить в блоке свою игру. Состояние при этом не
    // сбрасывается намеренно: модуль мог остаться в лаунчере с прошлого раза,
    // и второй раз его DllMain не выполнится — обнулив состояние, мы ждали бы
    // готовности, которую уже некому объявить.
    handoff.gameProcessId.store(0);
    handoff.straightIntoFreemode.store(order.straightIntoFreemode ? 1U : 0U);

    // Взводим последним: с этого мгновения ближайший запуск игры будет наш.
    handoff.armed.store(1);
    handoff.error[0] = '\0';

    const HANDLE process = ::OpenProcess(kInjectAccess, FALSE, launcher);
    if (process == nullptr) {
        error = std::format("Rockstar Games Launcher найден, но доступ к нему закрыт: "
                            "код ошибки Windows {}",
                            ::GetLastError());
        return nullptr;
    }

    // Повторное внедрение безвредно: LoadLibraryW вернёт описатель уже
    // загруженного модуля, второй подмены не появится.
    const bool injected = injectModule(process, module, error);
    ::CloseHandle(process);

    if (!injected) {
        error = "не удалось внедрить подмену в Rockstar Games Launcher: " + error;
        return nullptr;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        const auto state = static_cast<shared::LaunchState>(handoff.state.load());

        if (state == shared::LaunchState::Hooked || state == shared::LaunchState::GameStarted) {
            return patch;
        }

        if (state == shared::LaunchState::Failed) {
            error = "подмена звена BattlEye не встала: " + failureText(handoff);
            return nullptr;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            error = "подмена звена BattlEye не отозвалась";
            return nullptr;
        }

        ::Sleep(static_cast<DWORD>(kPollInterval.count()));
    }
}

std::uint32_t LauncherPatch::waitForGame(std::chrono::seconds timeout, std::string& error) {
    shared::LaunchHandoff& handoff = handoffOf(handoff_);

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        if (const std::uint32_t game = handoff.gameProcessId.load(); game != 0) {
            return game;
        }

        if (static_cast<shared::LaunchState>(handoff.state.load()) ==
            shared::LaunchState::Failed) {
            error = "запуск игры сорвался: " + failureText(handoff);
            return 0;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            error = "лаунчер Rockstar так и не запустил игру — проверьте, что в нём выполнен вход";
            return 0;
        }

        ::Sleep(static_cast<DWORD>(kPollInterval.count()));
    }
}

LauncherPatch::~LauncherPatch() {
    // Сам модуль остаётся в лаунчере Rockstar до конца его жизни: выгрузить его
    // отсюда нельзя, а снимать перехват на живом лаунчере опаснее, чем оставить.
    // Безвредно это только благодаря заказу, который снимается после первого же
    // срабатывания: дальше лаунчер запускает игру как обычно, со всеми её
    // звеньями, и GTA Online игрока пускает.
    //
    // Здесь освобождается только наша половина общего блока.
    if (handoff_ != nullptr) {
        ::UnmapViewOfFile(handoff_);
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(mapping_);
    }
}

} // namespace oxymp::launcher
