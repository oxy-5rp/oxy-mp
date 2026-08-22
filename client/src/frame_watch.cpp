#include "frame_watch.hpp"

#include <spdlog/spdlog.h>

#include <thread>

namespace oxymp::client {
namespace {

/// Как часто спрашивать сторожа. Зависание меряется секундами, и частить
/// незачем — поток должен спать, а не жечь ядро.
constexpr auto kLookGap = std::chrono::milliseconds{1000};

/// Каким шагом спать между проверками.
///
/// Мельче самой проверки, и только затем, чтобы уход был скорым: сон нельзя
/// прервать, и целая секунда задержала бы выгрузку модуля на эту секунду.
constexpr auto kNap = std::chrono::milliseconds{100};

[[nodiscard]] std::int64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void FrameWatch::mark(const char* phase) noexcept {
    phase_.store(phase, std::memory_order_relaxed);
    markedAt_.store(nowMs(), std::memory_order_relaxed);
}

FrameWatch::Look FrameWatch::look() const noexcept {
    const std::int64_t marked = markedAt_.load(std::memory_order_relaxed);
    const char* const phase = phase_.load(std::memory_order_relaxed);

    if (marked == 0) {
        return Look{.silence = std::chrono::milliseconds{0}, .phase = phase};
    }

    return Look{.silence = std::chrono::milliseconds{nowMs() - marked}, .phase = phase};
}

void watchFrames(const FrameWatch& watch, std::stop_token stop) {
    bool complained = false;

    while (!stop.stop_requested()) {
        for (auto slept = std::chrono::milliseconds{0};
             slept < kLookGap && !stop.stop_requested(); slept += kNap) {
            std::this_thread::sleep_for(kNap);
        }

        if (stop.stop_requested()) {
            return;
        }

        const FrameWatch::Look seen = watch.look();

        // До первого кадра сторожить нечего: игра ещё грузится, и кадров с
        // обработчиком скрипта у неё попросту нет.
        if (seen.phase == nullptr || seen.silence == std::chrono::milliseconds{0}) {
            continue;
        }

        if (seen.silence >= FrameWatch::kFreeze) {
            if (!complained) {
                complained = true;

                // Уровень предупреждения, а не отладки, и это тот редкий
                // случай, когда внутренность игры показывают человеку. Он
                // видит застывшее окно и всё равно придёт с вопросом — пусть
                // придёт со строкой, по которой видно, где встало.
                spdlog::warn("the game frame has been stuck for {} s at \"{}\"",
                             seen.silence.count() / 1000, seen.phase);
            }

            continue;
        }

        if (complained) {
            complained = false;
            spdlog::info("The game is running again");
        }
    }
}

} // namespace oxymp::client
