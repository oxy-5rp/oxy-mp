#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace oxymp::client::game {

/// Движения, которыми чужой игрок отыгрывает то, чем он занят.
///
/// Зеркальная половина PedActivity: та читает признаки у настоящего игрока, эта
/// отвечает на них движениями у его изображения.
///
/// Признаки делятся надвое, и обходятся эти половины по-разному.
///
/// Одни игра отыгрывает сама, стоит ей сказать, в каком состоянии персонаж:
/// присед — это положение тела, а не движение, и заказывается он один раз.
/// Другим движения нет вовсе: лазание, перемахивание и падение персонаж
/// изобразит сам, если его к этому вынудить, а вынуждает его положение, которое
/// мы и так задаём каждый кадр.
///
/// Третьи — короткие: удар длится доли секунды и не попадает ни в одно
/// состояние. Их приходится проигрывать движением из набора игры, и это
/// единственное место в клиенте, где называются движения по имени.
///
/// Вызывать можно только изнутри скриптового тика.
class PedAnimation {
public:
    explicit PedAnimation(const NativeTable& table) noexcept;

    /// Приводит позу персонажа в соответствие с признаками.
    ///
    /// previous нужен, чтобы отличить переход от состояния: заказанное заново
    /// каждый кадр, положение тела сбрасывается до того, как успевает начаться.
    void applyPosture(int ped, std::uint32_t flags, std::uint32_t previous) const;

    /// Проигрывает короткое движение.
    ///
    /// Возвращает false, если движение ещё не готово — набор движений
    /// подгружается, и первый удар может прийтись на то мгновение, когда его
    /// ещё нет. Вызывающему это знать полезно: неудавшийся удар стоит попробовать
    /// в следующем кадре, а не считать показанным.
    [[nodiscard]] bool play(int ped, shared::PedAction action);

private:
    /// Одно движение из набора игры.
    struct Clip {
        std::string_view dictionary;
        std::string_view name;
    };

    /// Какое движение отвечает какому действию.
    [[nodiscard]] static Clip clipFor(shared::PedAction action);

    /// Заказывает набор движений и говорит, готов ли он.
    ///
    /// О ненайденном наборе жалуется один раз: имя набора либо верное, либо нет,
    /// и повторять это тридцать раз в секунду незачем.
    [[nodiscard]] bool ready(std::string_view dictionary);

    NativeHandler requestDict_ = nullptr;
    NativeHandler hasDict_ = nullptr;
    NativeHandler playAnim_ = nullptr;
    NativeHandler stealthMovement_ = nullptr;
    NativeHandler gameTimer_ = nullptr;

    /// Наборы, о которых уже сказано в журнал, что они не загружаются.
    std::unordered_set<std::string_view> complained_;

    /// Когда набор заказан впервые. По этому времени видно, что он не грузится.
    std::unordered_map<std::string_view, std::int32_t> requestedAt_;
};

} // namespace oxymp::client::game
