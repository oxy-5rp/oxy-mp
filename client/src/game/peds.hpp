#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace oxymp::client::game {

/// Прохожие сессии: куклы, которых ставит сервер.
///
/// Устроены как предметы, а не как чужие игроки, и это следует из того, чем они
/// являются. Кукла чужого игрока ведётся снимками: за ней стоит живой человек,
/// и она обязана повторять его движение кадр за кадром. Прохожий стоит там, где
/// его поставили, — у него нет ни хозяина, ни ведущего, и повторять ему нечего.
///
/// Отсюда и весь класс: список, заведение по модели, правка состояния, уборка.
/// Ни интерполяции, ни отметок времени.
///
/// Замораживаются намеренно и все — по той же причине, что и предметы:
/// отпущенная на попечение физики кукла сползёт по уклону и разъедется у каждого
/// по-своему, потому что считать её будет каждый у себя.
///
/// Вызывать можно только изнутри скриптового тика.
class Peds {
public:
    explicit Peds(const NativeTable& table) noexcept;
    ~Peds();

    Peds(const Peds&) = delete;
    Peds& operator=(const Peds&) = delete;

    [[nodiscard]] bool ready() const noexcept;

    /// Заводит прохожего или правит уже заведённого.
    ///
    /// Одним вызовом на то и другое, потому что и сообщение одно: сервер шлёт
    /// состояние целиком, а знаем мы эту куклу или нет — наша забота, не его.
    void apply(const shared::PedState& state);

    void remove(shared::PedId id);

    /// Доводит до конца то, что не удалось раньше. Вызывать раз в кадр.
    /// Свидетельство о попадании по прохожему: номер, урон, наше оружие.
    using DamageSink =
        std::function<void(shared::PedId ped, std::uint16_t amount, std::uint32_t weapon)>;

    /// Кому говорить о попаданиях. Без этого попадания просто не замечаются.
    void reportDamageTo(DamageSink sink) { onDamage_ = std::move(sink); }

    /// Довести кукол до присланного состояния.
    ///
    /// Довод — тело своего игрока: попадание засчитывается только от него.
    /// Ноль — своего тела в мире нет, и засчитывать нечего.
    void sync(int localPed);

    /// Убирает всех разом. Нужно при разрыве.
    void clear();

    /// Номер прохожего в игре. Ноль — тела здесь нет: модель ещё грузится либо
    /// о нём не объявляли вовсе.
    [[nodiscard]] int handleFor(shared::PedId id) const;

    [[nodiscard]] std::size_t shown() const noexcept { return peds_.size(); }

private:
    /// Прохожий и всё, что мы о нём помним.
    struct Entry {
        /// Последнее известное состояние — то, что уже наложено на тело.
        shared::PedState state;

        /// Номер в игре. Ноль — модель ещё не загрузилась, тела пока нет.
        int handle = 0;

        /// Здоровье и броня, какими мы их поставили в прошлый раз.
        ///
        /// Помнятся отдельно от `state`, хотя и берутся из него: попадание
        /// считается разницей между поставленным и оставшимся, а `state`
        /// обновится присланным раньше, чем мы успеем сравнить.
        int appliedHealth = 0;
        int appliedArmour = 0;
    };

    /// Заводит куклу в мире. Ноль, если модель ещё не загрузилась.
    [[nodiscard]] int spawn(const shared::PedState& state);

    /// Накладывает на тело то, что в состоянии изменилось.
    ///
    /// Именно изменившееся, а не всё подряд: здоровье и оружие задаются
    /// нативами, и звать их у трёх десятков кукол каждый раз, когда сервер
    /// прислал то же самое, — это кадр, потраченный впустую.
    void dress(int handle, const shared::PedState& fresh, const shared::PedState& previous) const;

    void destroy(int handle) const;

    /// Замечает попадание нашего игрока по этой кукле и говорит о нём.
    void noticeDamage(shared::PedId id, Entry& entry, int localPed);

    /// Запоминает здоровье и броню, только что поставленные на тело.
    static void remember(Entry& entry);

    NativeHandler requestModel_ = nullptr;
    NativeHandler hasModelLoaded_ = nullptr;
    NativeHandler modelNoLongerNeeded_ = nullptr;
    NativeHandler createPed_ = nullptr;
    NativeHandler deletePed_ = nullptr;
    NativeHandler doesExist_ = nullptr;
    NativeHandler setCoords_ = nullptr;
    NativeHandler setRotation_ = nullptr;
    NativeHandler freezePosition_ = nullptr;
    NativeHandler asMissionEntity_ = nullptr;
    NativeHandler blockEvents_ = nullptr;
    NativeHandler canRagdoll_ = nullptr;
    NativeHandler setHealth_ = nullptr;
    NativeHandler setMaxHealth_ = nullptr;
    NativeHandler setArmour_ = nullptr;
    NativeHandler giveWeapon_ = nullptr;
    NativeHandler getHealth_ = nullptr;
    NativeHandler getArmour_ = nullptr;
    NativeHandler damagedBy_ = nullptr;
    NativeHandler clearDamage_ = nullptr;
    NativeHandler selectedWeapon_ = nullptr;

    DamageSink onDamage_;

    std::unordered_map<shared::PedId, Entry> peds_;
};

} // namespace oxymp::client::game
