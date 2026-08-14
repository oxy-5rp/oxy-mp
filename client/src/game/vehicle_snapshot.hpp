#pragma once

#include "native_table.hpp"

#include <oxymp/shared/protocol/messages.hpp>

#include <cstdint>

namespace oxymp::client::game {

/// Снимок машины: как снять его с игры и как наложить обратно.
///
/// Ничего не помнит и ничем не владеет — только переводит состояние машины из
/// того вида, в каком его держит игра, в тот, в каком оно ходит по сети, и
/// обратно. Учёт самих машин ведёт Vehicles; разделены они потому, что это
/// разные заботы: одна про то, какие машины есть, другая про то, что с ними
/// происходит.
///
/// Вызывать можно только изнутри скриптового тика.
class VehicleSnapshot {
public:
    explicit VehicleSnapshot(const NativeTable& table) noexcept;

    /// Все ли нативы, без которых снимок бессмыслен, нашлись.
    ///
    /// Проверяются только те, без которых машину не показать вовсе. Свет, двери
    /// и тюнинг сюда не входят намеренно: не нашёлся натив — не передаётся эта
    /// подробность, а не ломается синхронизация целиком.
    [[nodiscard]] bool ready() const noexcept;

    /// Снимает состояние машины.
    ///
    /// driving говорит, сидим ли мы за рулём. От этого зависит руль, газ и
    /// тормоз: их нет у машины, они есть у водителя, и читаются они из его
    /// ввода. Пассажир, взявший машину под свою руку после ухода водителя, ею
    /// не правит — и объявлять его нажатия за поворот руля было бы ложью.
    [[nodiscard]] shared::VehicleState read(int vehicle, bool driving) const;

    /// Снимает внешность машины: цвет, номер, тюнинг.
    ///
    /// Дороже обычного снимка на полсотни вызовов, и потому снимается редко —
    /// столько же раз, сколько внешность меняется, то есть почти никогда.
    [[nodiscard]] shared::VehicleAppearance readAppearance(int vehicle) const;

    /// Ставит машину туда, где ей положено быть, с той скоростью, что у неё есть.
    ///
    /// seconds — сколько длился кадр. Нужно затем, что расхождение закрывается
    /// не разом, а понемногу, и «понемногу» обязано считаться от времени, а не
    /// от кадров: иначе одна и та же машина у двух зрителей едет по-разному.
    void applyMotion(int vehicle, const shared::VehicleState& state, float seconds) const;

    /// Накладывает то, чем машина занята: руль, тормоз, свет, сирену, двигатель.
    ///
    /// Руль и тормоз задаются каждый раз — они меняются непрерывно. Остальное
    /// только на изменение: свет и сирена переключаются считанные разы за
    /// поездку, а машин вокруг бывает три десятка.
    void applyControls(int vehicle, const shared::VehicleState& state,
                       const shared::VehicleState& previous) const;

    /// Накладывает повреждения, изменившиеся с прошлого раза.
    ///
    /// Изменившиеся, а не все: разбить стекло можно только один раз, а вот
    /// приказывать разбивать его тридцать раз в секунду игра будет исправно.
    void applyDamage(int vehicle, const shared::VehicleState& state,
                     const shared::VehicleState& previous) const;

    /// Накладывает внешность целиком.
    void applyAppearance(int vehicle, const shared::VehicleAppearance& appearance) const;

private:
    /// Насколько машина должна разойтись со снимком, чтобы её переставить рывком.
    [[nodiscard]] static bool tooFar(const shared::Vec3& from, const shared::Vec3& to);

    NativeHandler getCoords_ = nullptr;
    NativeHandler setCoords_ = nullptr;
    NativeHandler getRotation_ = nullptr;
    NativeHandler setRotation_ = nullptr;
    NativeHandler getVelocity_ = nullptr;
    NativeHandler setVelocity_ = nullptr;
    NativeHandler getAngularVelocity_ = nullptr;
    NativeHandler getModel_ = nullptr;

    NativeHandler controlNormal_ = nullptr;
    NativeHandler controlPressed_ = nullptr;

    NativeHandler getBodyHealth_ = nullptr;
    NativeHandler setBodyHealth_ = nullptr;
    NativeHandler getEngineHealth_ = nullptr;
    NativeHandler setEngineHealth_ = nullptr;
    NativeHandler getTankHealth_ = nullptr;
    NativeHandler setTankHealth_ = nullptr;

    NativeHandler engineRunning_ = nullptr;
    NativeHandler setEngineOn_ = nullptr;
    NativeHandler lightsState_ = nullptr;
    NativeHandler setLights_ = nullptr;
    NativeHandler setFullBeam_ = nullptr;
    NativeHandler sirenOn_ = nullptr;
    NativeHandler setSiren_ = nullptr;
    NativeHandler setSteerBias_ = nullptr;
    NativeHandler setHandbrake_ = nullptr;
    NativeHandler setBrakeLights_ = nullptr;

    NativeHandler doorAngle_ = nullptr;
    NativeHandler openDoor_ = nullptr;
    NativeHandler shutDoor_ = nullptr;
    NativeHandler doorDamaged_ = nullptr;
    NativeHandler breakDoor_ = nullptr;
    NativeHandler windowIntact_ = nullptr;
    NativeHandler smashWindow_ = nullptr;
    NativeHandler tyreBurst_ = nullptr;
    NativeHandler burstTyre_ = nullptr;
    NativeHandler fixTyre_ = nullptr;

    NativeHandler getColours_ = nullptr;
    NativeHandler setColours_ = nullptr;
    NativeHandler getExtraColours_ = nullptr;
    NativeHandler setExtraColours_ = nullptr;
    NativeHandler getPlate_ = nullptr;
    NativeHandler setPlate_ = nullptr;
    NativeHandler getPlateStyle_ = nullptr;
    NativeHandler setPlateStyle_ = nullptr;
    NativeHandler getLivery_ = nullptr;
    NativeHandler setLivery_ = nullptr;
    NativeHandler getDirt_ = nullptr;
    NativeHandler setDirt_ = nullptr;
    NativeHandler getMod_ = nullptr;
    NativeHandler setMod_ = nullptr;
    NativeHandler setModKit_ = nullptr;
    NativeHandler toggleMod_ = nullptr;
    NativeHandler toggleModOn_ = nullptr;
    NativeHandler getWheelType_ = nullptr;
    NativeHandler setWheelType_ = nullptr;
    NativeHandler getWindowTint_ = nullptr;
    NativeHandler setWindowTint_ = nullptr;
};

} // namespace oxymp::client::game
