#pragma once

#include "native_table.hpp"

#include <oxymp/shared/math/vec3.hpp>

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
    void clearArea(shared::Vec3 centre, float radius) const;

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

private:
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
};

} // namespace oxymp::client::game
