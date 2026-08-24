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

    return reported.empty() ? "the patch gave no reason" : reported;
}

} // namespace

std::unique_ptr<LauncherPatch> LauncherPatch::install(const std::filesystem::path& module,
                                                      const Order& order,
                                                      std::chrono::seconds timeout,
                                                      std::string& error) {
    const std::uint32_t launcher = findProcessByName(kRockstarLauncherName);
    if (launcher == 0) {
        error = "Rockstar Games Launcher is not running - nowhere to replace the BattlEye link";
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
        error = std::format("could not create the shared block: Windows error {}",
                            ::GetLastError());
        return nullptr;
    }

    patch->handoff_ = ::MapViewOfFile(patch->mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                      sizeof(shared::LaunchHandoff));
    if (patch->handoff_ == nullptr) {
        error = std::format("could not map the shared block: Windows error {}",
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
    handoff.verbose.store(order.verbose ? 1U : 0U);

    // Взводим последним: с этого мгновения ближайший запуск игры будет наш.
    handoff.armed.store(1);
    handoff.error[0] = '\0';

    const HANDLE process = ::OpenProcess(kInjectAccess, FALSE, launcher);
    if (process == nullptr) {
        error = std::format("Rockstar Games Launcher was found, but access to it is denied: "
                            "Windows error {}",
                            ::GetLastError());
        return nullptr;
    }

    // Повторное внедрение безвредно: LoadLibraryW вернёт описатель уже
    // загруженного модуля, второй подмены не появится.
    const bool injected = injectModule(process, module, error);
    ::CloseHandle(process);

    if (!injected) {
        error = "could not inject the patch into Rockstar Games Launcher: " + error;
        return nullptr;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        const auto state = static_cast<shared::LaunchState>(handoff.state.load());

        if (state == shared::LaunchState::Hooked || state == shared::LaunchState::GameStarted) {
            return patch;
        }

        if (state == shared::LaunchState::Failed) {
            error = "the BattlEye link patch did not take: " + failureText(handoff);
            return nullptr;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            error = "the BattlEye link patch never answered";
            return nullptr;
        }

        ::Sleep(static_cast<DWORD>(kPollInterval.count()));
    }
}

std::uint32_t LauncherPatch::startedGame() const {
    return handoffOf(handoff_).gameProcessId.load();
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
