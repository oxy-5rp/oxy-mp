#pragma once

#include "native_table.hpp"

#include <oxymp/shared/math/vec3.hpp>

#include <cstdint>

namespace oxymp::client::game {

/// Смерть игрока и возвращение его в игру.
///
/// Разбираться со смертью самой игре нельзя. Её порядок рассчитан на одиночный
/// сюжет: экран гаснет, появляется надпись о проваленной миссии, и текущая
/// миссия запускается заново. Мультиплееру нужно ровно противоположное —
/// игрок просто встаёт в условленной точке и играет дальше.
///
/// Вызывать можно только изнутри скриптового тика.
class Respawn {
public:
    explicit Respawn(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Запрещает игре разбираться со смертью самой. Вызывать каждый кадр.
    ///
    /// Каждый кадр, а не однажды, потому что запрет снимают сами сюжетные
    /// скрипты: миссия, начавшись, выставляет своё поведение при смерти.
    void suppressGameHandling() const;

    [[nodiscard]] bool dead(int player) const;

    /// Поднимает игрока в указанной точке.
    void resurrect(shared::Vec3 point, float heading, int player);

    /// Пора ли поднимать игрока.
    ///
    /// Между смертью и подъёмом выдерживается пауза: мгновенный подъём выглядит
    /// так, будто ничего не произошло, и игрок не понимает, что его убили.
    /// Отсчёт ведётся по игровому времени, а не по кадрам: при просевшей частоте
    /// кадров пауза иначе растянулась бы на минуты.
    [[nodiscard]] bool due(bool isDead);

private:
    NativeHandler pauseDeathRestart_ = nullptr;
    NativeHandler ignoreNextRestart_ = nullptr;
    NativeHandler fadeOutAfterDeath_ = nullptr;
    NativeHandler isPlayerDead_ = nullptr;
    NativeHandler resurrect_ = nullptr;
    NativeHandler resetArrest_ = nullptr;
    NativeHandler gameTimer_ = nullptr;

    /// Момент смерти по игровому времени. Ноль означает, что игрок жив.
    std::int32_t diedAt_ = 0;
};

} // namespace oxymp::client::game
