#include "hook.hpp"

#include <MinHook.h>

#include <format>

namespace oxymp::client::game {
namespace {

std::string describe(MH_STATUS status) {
    const char* text = ::MH_StatusToString(status);
    return text != nullptr ? text : "неизвестная ошибка";
}

} // namespace

bool HookEngine::initialise(std::string& error) {
    const MH_STATUS status = ::MH_Initialize();

    // Повторная подготовка не ошибка: механизм один на процесс, и владельцев
    // у него может быть несколько.
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
        return true;
    }

    error = std::format("не удалось подготовить перехват: {}", describe(status));
    return false;
}

void HookEngine::shutdown() noexcept {
    ::MH_DisableHook(MH_ALL_HOOKS);
    ::MH_Uninitialize();
}

Hook::~Hook() {
    remove();
}

void Hook::remove() noexcept {
    if (target_ == nullptr) {
        return;
    }

    ::MH_DisableHook(target_);
    ::MH_RemoveHook(target_);

    target_ = nullptr;
    original_ = nullptr;
}

bool Hook::install(void* target, void* detour, std::string& error) {
    if (target == nullptr || detour == nullptr) {
        error = "перехват без адреса";
        return false;
    }

    if (const MH_STATUS status = ::MH_CreateHook(target, detour, &original_); status != MH_OK) {
        error = std::format("не удалось создать перехват: {}", describe(status));
        return false;
    }

    if (const MH_STATUS status = ::MH_EnableHook(target); status != MH_OK) {
        ::MH_RemoveHook(target);
        original_ = nullptr;

        error = std::format("не удалось включить перехват: {}", describe(status));
        return false;
    }

    target_ = target;
    return true;
}

} // namespace oxymp::client::game
