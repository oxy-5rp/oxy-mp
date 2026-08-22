#include "landing_page.hpp"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <format>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Опкод короткого перехода «если равно» и опкод безусловного перехода.
///
/// Обе инструкции двухбайтные и отличаются только первым байтом, поэтому замена
/// одного на другой не сдвигает ничего вокруг.
constexpr std::uint8_t kJumpIfEqual = 0x74;
constexpr std::uint8_t kJumpAlways = 0xEB;

} // namespace

bool skipLandingPage(const EngineAddresses& addresses, std::string& error) {
    auto* const branch = addresses.pointerTo<std::uint8_t*>("landing_page_branch");
    if (branch == nullptr) {
        error = "адрес развилки страницы выбора не разрешён";
        return false;
    }

    // Сверка перед записью обязательна. Сигнатура могла совпасть не там, где
    // нужно, оставаясь при этом формально однозначной, — а запись чужого байта
    // в середину чужой инструкции уронит игру без всяких объяснений.
    if (*branch != kJumpIfEqual) {
        error = std::format("по адресу развилки лежит {:#04x}, а ожидался {:#04x} — "
                            "сигнатура указывает не туда, правки не будет",
                            *branch, kJumpIfEqual);
        return false;
    }

    DWORD previous = 0;
    if (::VirtualProtect(branch, sizeof(*branch), PAGE_EXECUTE_READWRITE, &previous) == 0) {
        error = std::format("не удалось открыть код игры для записи: код ошибки Windows {}",
                            ::GetLastError());
        return false;
    }

    *branch = kJumpAlways;

    DWORD restored = 0;
    ::VirtualProtect(branch, sizeof(*branch), previous, &restored);

    spdlog::debug("страница выбора режима отключена: {:#x} теперь безусловный переход",
                 reinterpret_cast<std::uintptr_t>(branch));

    return true;
}

} // namespace oxymp::client::game
