#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>

namespace oxymp::client::game {

/// Чем занят персонаж, каким это видно самой игре.
///
/// Отсюда берётся всё, что нельзя вывести из положения и скорости. Правило
/// отбора именно такое, и оно важнее полноты: бег получатель выведет из скорости
/// сам, а плавание — нет. Плывущий и идущий по грудь в воде движутся одинаково,
/// а выглядят по-разному, и разницу приходится передавать.
///
/// Читает и только читает: у этого класса есть зеркальная половина,
/// PedAnimation, которая тем же признакам отвечает движениями на чужой стороне.
/// Разделены они не ради симметрии, а потому что живут в разных местах: одна
/// работает с настоящим игроком, другая — с его изображением у соседей.
///
/// Работает нативами, поэтому вызывать можно только изнутри скриптового тика.
class PedActivity {
public:
    explicit PedActivity(const NativeTable& table) noexcept;

    /// Куда персонаж лезет, если лезет.
    struct Entering {
        /// Номер машины в игре. Ноль — никуда не лезет.
        int vehicle = 0;

        /// На какое место лезет.
        std::int8_t seat = shared::kNoSeat;
    };

    /// Набор PlayerFlag, описывающий занятие персонажа.
    ///
    /// Смерть приходит доводом, а не читается здесь: шкала здоровья доходит до
    /// нуля не в тот же миг, когда игра объявляет игрока мёртвым, и решает это
    /// разбор смерти, а не мы.
    [[nodiscard]] std::uint32_t flags(int player, int ped, bool dead) const;

    /// Броня персонажа.
    [[nodiscard]] std::uint16_t armour(int ped) const;

    /// В какую машину персонаж лезет прямо сейчас.
    [[nodiscard]] Entering entering(int ped) const;

    /// Удар, начавшийся в этом кадре. None — не начинался.
    ///
    /// Именно начавшийся, а не идущий: удар длится доли секунды, и снимок
    /// застаёт его в лучшем случае один раз. Получателю нужно знать, что удар
    /// был, а не что он продолжается.
    ///
    /// Отвечает на нажатие, а не на вопрос игре «бьёт ли он»: такого вопроса у
    /// неё нет. Нажатие при этом учитывается только тогда, когда бить и правда
    /// нечем, — иначе выстрел из пистолета обернулся бы у соседей ударом кулака.
    [[nodiscard]] shared::PedAction strike(int ped, std::uint32_t weapon) const;

private:
    /// Нажат ли только что этот орган управления.
    [[nodiscard]] bool justPressed(int control) const;

    /// Отвечает ли персонаж на «бьёт ли он вообще».
    [[nodiscard]] bool fightsBarehanded(int ped, std::uint32_t weapon) const;

    NativeHandler isShooting_ = nullptr;
    NativeHandler isAiming_ = nullptr;
    NativeHandler isRagdoll_ = nullptr;
    NativeHandler isJumping_ = nullptr;
    NativeHandler stealthMovement_ = nullptr;
    NativeHandler isClimbing_ = nullptr;
    NativeHandler isVaulting_ = nullptr;
    NativeHandler isSwimming_ = nullptr;
    NativeHandler isUnderwater_ = nullptr;
    NativeHandler isDiving_ = nullptr;
    NativeHandler isFalling_ = nullptr;
    NativeHandler parachuteState_ = nullptr;
    NativeHandler isReloading_ = nullptr;
    NativeHandler isInCover_ = nullptr;
    NativeHandler isGettingUp_ = nullptr;
    NativeHandler isDrivingBy_ = nullptr;
    NativeHandler isInMelee_ = nullptr;
    NativeHandler gettingIntoVehicle_ = nullptr;
    NativeHandler vehicleEntering_ = nullptr;
    NativeHandler seatEntering_ = nullptr;
    NativeHandler getArmour_ = nullptr;
    NativeHandler justPressed_ = nullptr;
};

} // namespace oxymp::client::game
