#include "focus_pause.hpp"

#include "code_patch.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <format>

namespace oxymp::client::game {
namespace {

/// Опкод инструкции, кладущей признак: `setne byte ptr [rip+X]`.
///
/// Три байта опкода и четыре — смещения до признака, итого семь. Сверяются
/// только опкодные: смещение у каждой сборки своё.
constexpr std::array<std::uint8_t, 3> kSetIfNotEqual{0x0F, 0x95, 0x05};

/// Ничего не делающая инструкция.
constexpr std::uint8_t kNoOperation = 0x90;

/// Вся инструкция целиком — её и заменяем пустотой.
constexpr std::array<std::uint8_t, 7> kPatch{
    kNoOperation, kNoOperation, kNoOperation, kNoOperation,
    kNoOperation, kNoOperation, kNoOperation,
};

/// Значение признака, при котором игра не встаёт на паузу.
constexpr std::uint8_t kDoNotPause = 0;

} // namespace

bool keepRunningUnfocused(const EngineAddresses& addresses, std::string& error) {
    auto* const write = addresses.pointerTo<std::uint8_t*>("focus_pause_write");
    auto* const flag = addresses.pointerTo<std::uint8_t*>("focus_pause_flag");

    if (write == nullptr || flag == nullptr) {
        error = "адрес паузы при потере фокуса не разрешён";
        return false;
    }

    // Сверка перед записью обязательна. Сигнатура могла совпасть не там, где
    // нужно, оставаясь при этом формально однозначной, — а семь чужих байт
    // посреди чужой функции уронят игру без всяких объяснений.
    for (std::size_t at = 0; at < kSetIfNotEqual.size(); ++at) {
        if (write[at] != kSetIfNotEqual[at]) {
            error = std::format("по адресу паузы лежит {:#04x} вместо {:#04x} — сигнатура "
                                "указывает не туда, правки не будет",
                                write[at], kSetIfNotEqual[at]);
            return false;
        }
    }

    if (!code::write(write, kPatch.data(), kPatch.size(), error)) {
        return false;
    }

    // Признак обнуляется после правки, а не до: между двумя действиями игра
    // вправе пройти по этому месту, и лучше пусть она выставит признак, который
    // мы тут же снимем, чем снимет тот, который она тут же выставит обратно.
    if (!code::write(flag, &kDoNotPause, sizeof(kDoNotPause), error)) {
        return false;
    }

    spdlog::info("пауза при потере фокуса убрана: запись {:#x}, признак {:#x}",
                 reinterpret_cast<std::uintptr_t>(write),
                 reinterpret_cast<std::uintptr_t>(flag));

    return true;
}

} // namespace oxymp::client::game
