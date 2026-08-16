#include "code_patch.hpp"

#include <cstring>
#include <format>

#include <windows.h>

namespace oxymp::client::game::code {
namespace {

/// Переходник: `mov rax, адрес; jmp rax`.
constexpr std::uint8_t kThunkTemplate[] = {
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, imm64
    0xFF, 0xE0,                         // jmp rax
};

/// Смещение адреса внутри переходника.
constexpr std::size_t kThunkAddressOffset = 2;

/// Насколько далеко от игры соглашаемся выделить переходник.
///
/// Меньше двух гигабайт, и с запасом: смещение в `call rel32` знаковое, и
/// упираться в самую его границу незачем.
constexpr std::uintptr_t kReach = 0x7000'0000;

/// Байт вызова и его длина вместе со смещением.
constexpr std::uint8_t kCallOpcode = 0xE8;
constexpr std::size_t kCallLength = 5;

} // namespace

bool write(void* where, const void* what, std::size_t size, std::string& error) {
    DWORD previous = 0;
    if (::VirtualProtect(where, size, PAGE_EXECUTE_READWRITE, &previous) == 0) {
        error = std::format("не удалось открыть код игры для записи: код ошибки Windows {}",
                            ::GetLastError());
        return false;
    }

    std::memcpy(where, what, size);

    DWORD restored = 0;
    ::VirtualProtect(where, size, previous, &restored);

    ::FlushInstructionCache(::GetCurrentProcess(), where, size);

    return true;
}

void* allocateNear(std::uintptr_t anchor, std::size_t size) {
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);

    const std::uintptr_t step = info.dwAllocationGranularity;
    if (step == 0) {
        return nullptr;
    }

    // Шагаем от места правки в обе стороны: ближе — лучше, а какая из сторон
    // окажется свободной, заранее не известно.
    for (std::uintptr_t distance = step; distance < kReach; distance += step) {
        const std::uintptr_t candidates[] = {
            anchor > distance ? (anchor - distance) & ~(step - 1) : 0,
            (anchor + distance) & ~(step - 1),
        };

        for (const std::uintptr_t candidate : candidates) {
            if (candidate == 0) {
                continue;
            }

            void* const memory =
                ::VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
            if (memory != nullptr) {
                return memory;
            }
        }
    }

    return nullptr;
}

void release(void* memory) noexcept {
    if (memory != nullptr) {
        ::VirtualFree(memory, 0, MEM_RELEASE);
    }
}

void* makeThunk(std::uintptr_t anchor, const void* target, std::string& error) {
    void* const memory = allocateNear(anchor, sizeof(kThunkTemplate));
    if (memory == nullptr) {
        error = "рядом с игрой не нашлось памяти под переходник";
        return nullptr;
    }

    std::uint8_t thunk[sizeof(kThunkTemplate)];
    std::memcpy(thunk, kThunkTemplate, sizeof(thunk));
    std::memcpy(thunk + kThunkAddressOffset, &target, sizeof(target));

    std::memcpy(memory, thunk, sizeof(thunk));
    ::FlushInstructionCache(::GetCurrentProcess(), memory, sizeof(thunk));

    return memory;
}

bool redirectCall(std::uint8_t* site, const void* target, void** original, std::string& error) {
    // Сверка перед записью обязательна: сигнатура могла совпасть не там, где
    // нужно, оставаясь при этом формально однозначной, — а смещение, записанное
    // в середину чужой инструкции, уронит игру без объяснений.
    if (*site != kCallOpcode) {
        error = std::format("по адресу вызова лежит {:#04x}, а ожидался {:#04x} — сигнатура "
                            "указывает не туда, правки не будет",
                            *site, kCallOpcode);
        return false;
    }

    if (original != nullptr) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, site + 1, sizeof(displacement));

        *original = site + kCallLength + displacement;
    }

    void* const thunk = makeThunk(reinterpret_cast<std::uintptr_t>(site), target, error);
    if (thunk == nullptr) {
        return false;
    }

    const auto reach = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(thunk)) -
                       static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(site)) -
                       static_cast<std::int64_t>(kCallLength);

    const auto displacement = static_cast<std::int32_t>(reach);

    if (!write(site + 1, &displacement, sizeof(displacement), error)) {
        release(thunk);
        return false;
    }

    return true;
}

} // namespace oxymp::client::game::code
