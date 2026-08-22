#include "intro.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <format>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Инструкция возврата из функции.
constexpr std::uint8_t kReturn = 0xC3;

/// Первый байт пролога функции музыки: `mov [rsp+8], rbx`.
///
/// Сверяется перед записью — по той же причине, по которой сверяется всё
/// остальное: сигнатура могла совпасть не там, где нужно.
constexpr std::uint8_t kFunctionStart = 0x48;

/// Пишет байт в код игры, открыв его на запись и закрыв обратно.
[[nodiscard]] bool writeCodeByte(std::uint8_t* where, std::uint8_t what, std::string& error) {
    DWORD previous = 0;
    if (::VirtualProtect(where, sizeof(what), PAGE_EXECUTE_READWRITE, &previous) == 0) {
        error = std::format("не удалось открыть код игры для записи: код ошибки Windows {}",
                            ::GetLastError());
        return false;
    }

    *where = what;

    DWORD restored = 0;
    ::VirtualProtect(where, sizeof(what), previous, &restored);

    ::FlushInstructionCache(::GetCurrentProcess(), where, sizeof(what));

    return true;
}

} // namespace

void skipLegalScreens(const EngineAddresses& addresses) {
    auto* duration = addresses.pointerTo<std::uint32_t*>("legal_screen_duration");
    if (duration == nullptr) {
        return;
    }

    const std::uint32_t before = *duration;

    *duration = 0;

    // Значение перечитывается обратно, а не считается записанным. Запись в чужой
    // процесс может не лечь молча — например если по адресу лежит копия, а
    // читает игра другую, — и тогда «обнуляем» в журнале означало бы ровно
    // ничего.
    spdlog::debug("юридическая заставка держалась {} мс, после записи {} мс", before, *duration);
}

bool muteLoadingMusic(const EngineAddresses& addresses, std::string& error) {
    auto* const function = addresses.pointerTo<std::uint8_t*>("loading_screen_music");
    if (function == nullptr) {
        error = "адрес музыки загрузки не разрешён";
        return false;
    }

    if (*function != kFunctionStart) {
        error = std::format("по адресу музыки лежит {:#04x}, а ожидалось начало функции "
                            "{:#04x} — сигнатура указывает не туда, правки не будет",
                            *function, kFunctionStart);
        return false;
    }

    if (!writeCodeByte(function, kReturn, error)) {
        return false;
    }

    spdlog::debug("музыка загрузки заглушена: {:#x} теперь возврат",
                 reinterpret_cast<std::uintptr_t>(function));

    return true;
}

} // namespace oxymp::client::game
