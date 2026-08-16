#include "bink_sound.hpp"

#include "hook.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>

#include <windows.h>

namespace oxymp::client::game {
namespace {

/// Как называется библиотека роликов рядом с игрой.
constexpr const wchar_t* kBinkModule = L"bink2w64.dll";

/// Ответ «получилось» на языке Bink.
constexpr std::int32_t kBinkOk = 1;

using SetSoundSystemFunction = std::int32_t(__stdcall*)(void*, std::uintptr_t);
using SetSoundOnOffFunction = std::int32_t(__stdcall*)(void*, std::int32_t);
using SetVolumeFunction = std::int32_t(__stdcall*)(void*, std::uint32_t, std::int32_t);

} // namespace

struct BinkSound::State {
    Hook soundSystem;
    Hook soundSystem2;
    Hook soundOnOff;
    Hook volume;

    /// По разу на каждый перехват: заранее не известно, каким из трёх путей
    /// игра заводит звук, а по журналу это видно сразу.
    std::atomic<bool> toldSoundSystem{false};
    std::atomic<bool> toldSoundOnOff{false};
    std::atomic<bool> toldVolume{false};

    /// Единственный на процесс: подменённые функции — свободные, и своего
    /// состояния Windows им не передаёт.
    static State* active;

    static void tellOnce(std::atomic<bool>& told, const char* what) {
        if (!told.exchange(true)) {
            spdlog::info("звук ролика заглушен: игра звала {}", what);
        }
    }

    /// Звуковая подсистема не задаётся вовсе: без неё Bink открывает ролик без
    /// звуковой дорожки. Ответ при этом честный — «получилось»: отказ игра
    /// вправе счесть поломкой звука и полезть чинить его сама.
    static std::int32_t __stdcall setSoundSystem(void* open, std::uintptr_t parameter) {
        (void)open;
        (void)parameter;

        if (active != nullptr) {
            tellOnce(active->toldSoundSystem, "BinkSetSoundSystem");
        }

        return kBinkOk;
    }

    static std::int32_t __stdcall setSoundOnOff(void* bink, std::int32_t onOff) {
        (void)onOff;

        if (active == nullptr) {
            return kBinkOk;
        }

        tellOnce(active->toldSoundOnOff, "BinkSetSoundOnOff");

        // Ноль вместо того, что просили: звук выключен для этого ролика.
        const auto original = active->soundOnOff.original<SetSoundOnOffFunction>();
        return original != nullptr ? original(bink, 0) : kBinkOk;
    }

    static std::int32_t __stdcall setVolume(void* bink, std::uint32_t track, std::int32_t level) {
        (void)level;

        if (active == nullptr) {
            return kBinkOk;
        }

        tellOnce(active->toldVolume, "BinkSetVolume");

        const auto original = active->volume.original<SetVolumeFunction>();
        return original != nullptr ? original(bink, track, 0) : kBinkOk;
    }
};

BinkSound::State* BinkSound::State::active = nullptr;

std::unique_ptr<BinkSound> BinkSound::mute(std::string& error) {
    if (State::active != nullptr) {
        error = "звук роликов уже заглушен";
        return nullptr;
    }

    // Библиотека грузится вместе с игрой, до нас: если её нет, роликов нет тоже,
    // и глушить нечего.
    const HMODULE bink = ::GetModuleHandleW(kBinkModule);
    if (bink == nullptr) {
        error = "bink2w64.dll не загружена — роликов не будет и без нас";
        return nullptr;
    }

    auto state = std::make_unique<State>();
    State::active = state.get();

    const auto put = [bink, &error](Hook& hook, const char* name, void* detour) {
        void* const target = reinterpret_cast<void*>(::GetProcAddress(bink, name));
        if (target == nullptr) {
            spdlog::debug("в bink2w64.dll нет {} — этот путь звука не перехватывается", name);
            return;
        }

        std::string own;
        if (!hook.install(target, detour, own)) {
            spdlog::warn("{} не перехвачена: {}", name, own);
            error = std::move(own);
        }
    };

    put(state->soundSystem, "BinkSetSoundSystem",
        reinterpret_cast<void*>(&State::setSoundSystem));
    put(state->soundSystem2, "BinkSetSoundSystem2",
        reinterpret_cast<void*>(&State::setSoundSystem));
    put(state->soundOnOff, "BinkSetSoundOnOff", reinterpret_cast<void*>(&State::setSoundOnOff));
    put(state->volume, "BinkSetVolume", reinterpret_cast<void*>(&State::setVolume));

    spdlog::info("звук роликов заглушен");

    auto sound = std::unique_ptr<BinkSound>{new BinkSound};
    sound->state_ = std::move(state);
    return sound;
}

BinkSound::~BinkSound() {
    if (state_ != nullptr) {
        state_->soundSystem.remove();
        state_->soundSystem2.remove();
        state_->soundOnOff.remove();
        state_->volume.remove();
    }

    State::active = nullptr;
}

} // namespace oxymp::client::game
