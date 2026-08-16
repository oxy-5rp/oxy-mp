#include "discord_block.hpp"

#include "hook.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cwctype>
#include <string>
#include <string_view>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Начало имени канала, которым Discord разговаривает со всеми.
///
/// Номер в конце — от нуля до девяти: столько каналов Discord открывает при
/// нескольких запущенных копиях. Сравнивается только начало, номер не важен.
constexpr std::wstring_view kDiscordPipe = L"\\\\.\\pipe\\discord-ipc-";

using CreateFileWFunction = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                                            DWORD, HANDLE);
using CreateFileAFunction = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD,
                                            DWORD, HANDLE);
using LoadLibraryFunction = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

/// Библиотека, которой Social Club показывает игру в Discord.
///
/// Ничем другим она не занята: имя говорит само за себя, и загружается она
/// только ради показа.
constexpr std::wstring_view kDiscordHelper = L"RockstarDiscordHelper.dll";

/// Наш ли это вызов. Потоковый: показ живёт в своём потоке, игра — в своих.
thread_local bool t_ours = false;

/// Сравнивает начало имени без учёта регистра.
///
/// Регистр важен: имя канала собирает не Windows, а тот, кто его открывает, и
/// писать его он вправе как угодно.
/// Кончается ли путь названным именем файла, без учёта регистра.
[[nodiscard]] bool named(LPCWSTR path, std::wstring_view name) {
    if (path == nullptr) {
        return false;
    }

    const std::wstring_view whole{path};
    if (whole.size() < name.size()) {
        return false;
    }

    const std::wstring_view tail = whole.substr(whole.size() - name.size());

    // Начало хвоста обязано быть началом имени файла, а не серединой другого:
    // «NotRockstarDiscordHelper.dll» — не то, что мы ищем.
    if (whole.size() > name.size()) {
        const wchar_t before = whole[whole.size() - name.size() - 1];
        if (before != L'\\' && before != L'/') {
            return false;
        }
    }

    for (std::size_t i = 0; i < name.size(); ++i) {
        if (std::towlower(static_cast<std::wint_t>(tail[i])) !=
            std::towlower(static_cast<std::wint_t>(name[i]))) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool startsWithDiscordPipe(LPCWSTR name) {
    if (name == nullptr) {
        return false;
    }

    for (std::size_t i = 0; i < kDiscordPipe.size(); ++i) {
        if (name[i] == L'\0') {
            return false;
        }

        if (std::towlower(static_cast<std::wint_t>(name[i])) !=
            std::towlower(static_cast<std::wint_t>(kDiscordPipe[i]))) {
            return false;
        }
    }

    return true;
}

/// То же для однобайтового имени: канал открывают и так, и так.
[[nodiscard]] bool startsWithDiscordPipe(LPCSTR name) {
    if (name == nullptr) {
        return false;
    }

    for (std::size_t i = 0; i < kDiscordPipe.size(); ++i) {
        if (name[i] == '\0') {
            return false;
        }

        if (std::towlower(static_cast<std::wint_t>(static_cast<unsigned char>(name[i]))) !=
            std::towlower(static_cast<std::wint_t>(kDiscordPipe[i]))) {
            return false;
        }
    }

    return true;
}

} // namespace

struct DiscordBlock::State {
    Hook wide;
    Hook narrow;
    Hook library;
    std::atomic<unsigned int> refused{0};

    /// Одна запись на путь, а не тысяча: Social Club стучится повторно, а
    /// сказать нужно ровно одно — что дверь закрыта и закрыта именно нами.
    void tellRefused(const char* how) {
        if (refused.fetch_add(1) == 0) {
            spdlog::info("показ игры в Discord отклонён: {}", how);
        }
    }

    static HANDLE WINAPI openWide(LPCWSTR name, DWORD access, DWORD share,
                                  LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                                  HANDLE templateFile) {
        State* const state = active_;

        if (state != nullptr && !t_ours && startsWithDiscordPipe(name)) {
            state->tellRefused("канал (широкое имя) занят нашим");

            // Тот же отказ, что приходит всякому, у кого Discord не запущен.
            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }

        auto original = state != nullptr ? state->wide.original<CreateFileWFunction>() : nullptr;
        if (original == nullptr) {
            ::SetLastError(ERROR_INVALID_FUNCTION);
            return INVALID_HANDLE_VALUE;
        }

        return original(name, access, share, security, disposition, flags, templateFile);
    }

    static HANDLE WINAPI openNarrow(LPCSTR name, DWORD access, DWORD share,
                                    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                                    HANDLE templateFile) {
        State* const state = active_;

        if (state != nullptr && !t_ours && startsWithDiscordPipe(name)) {
            state->tellRefused("канал (однобайтовое имя) занят нашим");

            ::SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }

        auto original = state != nullptr ? state->narrow.original<CreateFileAFunction>() : nullptr;
        if (original == nullptr) {
            ::SetLastError(ERROR_INVALID_FUNCTION);
            return INVALID_HANDLE_VALUE;
        }

        return original(name, access, share, security, disposition, flags, templateFile);
    }

    static HMODULE WINAPI load(LPCWSTR path, HANDLE reserved, DWORD flags) {
        State* const state = active_;

        if (state != nullptr && named(path, kDiscordHelper)) {
            state->tellRefused("библиотека показа не загружена");

            ::SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
        }

        auto original = state != nullptr ? state->library.original<LoadLibraryFunction>() : nullptr;
        if (original == nullptr) {
            ::SetLastError(ERROR_INVALID_FUNCTION);
            return nullptr;
        }

        return original(path, reserved, flags);
    }
};

DiscordBlock::State* DiscordBlock::active_ = nullptr;

DiscordBlock::Ours::Ours() noexcept { t_ours = true; }
DiscordBlock::Ours::~Ours() { t_ours = false; }

std::unique_ptr<DiscordBlock> DiscordBlock::install(std::string& error) {
    if (active_ != nullptr) {
        error = "показ игры в Discord уже заткнут";
        return nullptr;
    }

    auto state = std::make_unique<State>();

    // Указатель ставится раньше перехвата: подменённая функция вправе быть
    // вызвана в тот же миг.
    active_ = state.get();

    const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) {
        active_ = nullptr;
        error = "kernel32 не найдена — перехватывать нечего";
        return nullptr;
    }

    const auto put = [kernel, &error](Hook& hook, const char* name, void* detour) {
        void* const target = reinterpret_cast<void*>(::GetProcAddress(kernel, name));
        if (target == nullptr) {
            error = std::string{name} + " не найдена";
            return false;
        }

        return hook.install(target, detour, error);
    };

    // Широкое имя обязательно: им открывает канал и Discord, и мы сами.
    // Остальные две двери необязательны: не закрылась одна — остаются другие.
    if (!put(state->wide, "CreateFileW", reinterpret_cast<void*>(&State::openWide))) {
        active_ = nullptr;
        return nullptr;
    }

    if (!put(state->narrow, "CreateFileA", reinterpret_cast<void*>(&State::openNarrow))) {
        spdlog::warn("однобайтовое открытие канала не перехвачено: {}", error);
    }

    if (!put(state->library, "LoadLibraryExW", reinterpret_cast<void*>(&State::load))) {
        spdlog::warn("загрузка библиотеки показа не перехвачена: {}", error);
    }

    spdlog::info("показ игры в Discord заткнут: канал открываем только мы");

    auto block = std::unique_ptr<DiscordBlock>{new DiscordBlock};
    block->state_ = std::move(state);
    return block;
}

DiscordBlock::~DiscordBlock() {
    if (state_ != nullptr) {
        spdlog::info("чужих попыток открыть канал Discord отклонено: {}",
                     state_->refused.load());

        state_->wide.remove();
        state_->narrow.remove();
        state_->library.remove();
    }

    active_ = nullptr;
}

unsigned int DiscordBlock::refused() const noexcept {
    return state_ != nullptr ? state_->refused.load() : 0;
}

} // namespace oxymp::client::game
