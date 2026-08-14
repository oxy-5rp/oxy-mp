#pragma once

#include "native_table.hpp"

#include <oxymp/shared/math/vec3.hpp>
#include <oxymp/shared/protocol/messages.hpp>

#include <string>

namespace oxymp::client::game {

/// Мир вокруг игрока: население и полиция.
///
/// Одиночная игра населяет улицы прохожими, машинами и патрулями. Мультиплееру
/// они не нужны совсем: чужие игроки приходят с сервера, а вся местная толпа
/// существует только на этом компьютере, у каждого своя, — договориться о ней с
/// другими игроками невозможно. Поэтому население выключается целиком.
///
/// Вызывать можно только изнутри скриптового тика.
class World {
public:
    explicit World(const NativeTable& table) noexcept;

    [[nodiscard]] bool ready() const noexcept;

    /// Держит население выключенным. Вызывать каждый кадр.
    ///
    /// Иначе нельзя: почти все эти нативы действуют ровно один кадр — так их
    /// задумала игра, чтобы миссии могли разово опустошить улицу. Постоянного
    /// выключателя у населения нет.
    void suppressPopulation() const;

    /// Держит розыск выключенным. Вызывать каждый кадр.
    void suppressWanted(int player) const;

    /// Убирает уже созданных прохожих, машины и патрули вокруг точки.
    ///
    /// Нужно вдобавок к выключенному населению: запрет касается только новых, а
    /// созданные до него остаются на местах.
    ///
    /// Не щадит ничего, включая наши сетевые сущности, — поэтому зовётся только
    /// там, где их заведомо нет: в точке появления до входа в сессию.
    void clearArea(shared::Vec3 centre, float radius) const;

    /// Убирает случайную машину, забредшую в мир помимо сессии.
    ///
    /// Нужна оттого, что заглушить население полностью не выходит: множители
    /// плотности и выключенные генераторы останавливают почти всё, но машины с
    /// водителями всё равно изредка появляются — их заводят сюжетные скрипты
    /// игры, которые продолжают работать, и точки появления, до которых запрет
    /// не дотянулся.
    ///
    /// Отличить чужую от нашей можно надёжно: наши помечены как принадлежащие
    /// скрипту (мы сами так их пометили при создании), а случайные принадлежат
    /// миру. Сравнивать по спискам не нужно и не годится — список машин сессии
    /// живёт этажом выше, а сюда ему ходу нет.
    ///
    /// По одной за вызов: перебирать весь мир каждый кадр незачем, а лишняя
    /// машина, прожившая лишние полсекунды, никому не мешает.
    void sweepStrayVehicle(shared::Vec3 centre, float radius) const;

    /// Возвращает времени обычный ход. Вызывать каждый кадр.
    ///
    /// Игра замедляет время сама и в нескольких местах: на колесе выбора
    /// персонажа и оружия, при смерти, при аресте, при съезде с дороги на
    /// скорости. В одиночной игре это украшение. В мультиплеере — расхождение:
    /// замедлился один, а остальные продолжают жить в обычном темпе, и его
    /// снимки приходят к ним вдвое реже, чем должны.
    ///
    /// Каждый кадр, потому что множитель ставит не игрок и не мы, а те самые
    /// места, и снять его насовсем невозможно — можно только каждый кадр
    /// возвращать своё.
    void keepTimeFlowing() const;

    /// Ставит погоду и время, присланные сервером.
    ///
    /// Погода и часы принадлежат серверу целиком, и это не прихоть: у себя их
    /// ставит каждый клиент сам, и предоставленные себе они разойдутся за
    /// минуты — у одного полдень и ясно, у другого ночь и гроза. Расходятся при
    /// этом не одни виды: ночью дальше вытянутой руки не видно, а в дождь машину
    /// несёт юзом, и один игрок объясняет другому происходящее у себя как чужую
    /// неисправность.
    ///
    /// Погода ставится только на изменение: натив её меняет мгновенно и рвёт
    /// плавный переход, а сервер повторяет одно и то же раз в две секунды.
    void applyWorldState(const shared::WorldState& state);

private:
    /// Погода, поставленная в прошлый раз. Пусто — ещё не ставили.
    std::string weather_;
    NativeHandler pedDensity_ = nullptr;
    NativeHandler scenarioPedDensity_ = nullptr;
    NativeHandler vehicleDensity_ = nullptr;
    NativeHandler randomVehicleDensity_ = nullptr;
    NativeHandler parkedVehicleDensity_ = nullptr;
    NativeHandler randomCops_ = nullptr;
    NativeHandler randomCopsNotOnScenarios_ = nullptr;
    NativeHandler garbageTrucks_ = nullptr;
    NativeHandler randomBoats_ = nullptr;
    NativeHandler randomTrains_ = nullptr;
    NativeHandler deleteAllTrains_ = nullptr;
    NativeHandler clearPeds_ = nullptr;
    NativeHandler clearVehicles_ = nullptr;
    NativeHandler clearCops_ = nullptr;
    NativeHandler maxWanted_ = nullptr;
    NativeHandler clearWanted_ = nullptr;
    NativeHandler policeIgnore_ = nullptr;
    NativeHandler dispatchService_ = nullptr;
    NativeHandler timeScale_ = nullptr;
    NativeHandler setWeather_ = nullptr;
    NativeHandler setClock_ = nullptr;
    NativeHandler vehicleBudget_ = nullptr;
    NativeHandler pedBudget_ = nullptr;
    NativeHandler parkedVehicles_ = nullptr;
    NativeHandler lowPriorityGenerators_ = nullptr;
    NativeHandler clearGenerators_ = nullptr;
    NativeHandler closestVehicle_ = nullptr;
    NativeHandler isMissionEntity_ = nullptr;
    NativeHandler deleteVehicle_ = nullptr;
};

} // namespace oxymp::client::game
