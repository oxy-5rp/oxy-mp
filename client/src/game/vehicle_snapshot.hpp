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
    ///
    /// Возвращает true, если машину пришлось починить целиком — то есть если она
    /// перестала числиться разбитой. Вызывающему это знать необходимо: починка
    /// возвращает машину в заводское состояние и вместе с вмятинами снимает всё,
    /// что мы накладывали когда-то, — свет, сирену, зажигание, крышу.
    /// Накладывается это только на изменение, а изменения после починки не
    /// будет: у приславшего снимок ничего не поменялось. Значит наложенное нужно
    /// объявить забытым и наложить заново.
    [[nodiscard]] bool applyControls(int vehicle, const shared::VehicleState& state,
                                     const shared::VehicleState& previous) const;

    /// Накладывает повреждения, изменившиеся с прошлого раза.
    ///
    /// Изменившиеся, а не все: разбить стекло можно только один раз, а вот
    /// приказывать разбивать его тридцать раз в секунду игра будет исправно.
    ///
    /// Ставит одну дверь под нужной степенью: ноль — закрыта, семёрка — настежь.
    ///
    /// Своим методом, а не куском наложения снимка, потому что зовут его двое:
    /// само наложение и распоряжение сервера о дверях. Два одинаковых тела
    /// разошлись бы на первой же правке.
    void applyDoor(int vehicle, int door, std::uint32_t level) const;

    /// На какой степени дверь сейчас — по ответу самой игры, а не по памяти о
    /// том, что мы ей велели. Нужен ведущему: доехавшую дверь больше не ведут.
    [[nodiscard]] std::uint32_t doorLevelOf(int vehicle, int door) const;

    /// Запирает машину так, как велел сервер.
    ///
    /// Накладывается всем, кто машину видит, а не одному ведущему: запертую
    /// дверь игра проверяет у того, кто в неё лезет, а лезут в чужую машину как
    /// раз не ведущие.
    /// Опускает и поднимает стёкла. Целиком, а не разницей: прочесть у игры
    /// нынешнее положение нечем.
    void applyWindows(int vehicle, std::uint8_t open) const;

    /// Складывает или поднимает крышу. Ноль означает «не трогать».
    void applyRoof(int vehicle, std::uint8_t roof) const;

    void applyLock(int vehicle, std::uint8_t lockState) const;

    void applyDamage(int vehicle, const shared::VehicleState& state,
                     const shared::VehicleState& previous) const;

    /// Закрывает машину от того урона, считать который не нам.
    ///
    /// remote означает «машину ведёт не мы». Такая машина принимает пули и
    /// удары и не принимает больше ничего: ни взрывов, ни огня, ни столкновений.
    /// Разделение здесь не по вкусу, а по тому, кто урон считает.
    ///
    /// Пуля и удар — наши: попал по машине игрок, стоящий здесь, и заметить
    /// убыль прочности можем только мы. Взрыв, огонь и столкновение — не наши:
    /// взрыв сервер объявляет всем сразу, а столкновение считает у себя тот, кто
    /// в машине едет. Приняв их здесь, мы отняли бы у машины прочность дважды —
    /// и вдобавок могли бы взорвать её у одного зрителя из всех.
    ///
    /// Прежде чужая машина была неуязвима целиком. Это было проще и стоило вот
    /// чего: стрельба по чужому транспорту не делала ровно ничего — ни здесь,
    /// где машина не принимала пуль, ни у ведущего, куда присланный выстрел
    /// приходит пулей с нулевым уроном.
    void protect(int vehicle, bool remote) const;

    /// Замечает попадание по чужой машине и возвращает ей прочность.
    ///
    /// Возвращает убыль, которую нанесли мы, — и только её. Прочность при этом
    /// возвращается к присланной в любом случае, кто бы её ни отнял: машину
    /// ведёт не мы, и всё, что случилось с ней здесь, — не более чем
    /// свидетельство.
    ///
    /// Здесь же лежит и наложение присланной прочности, а не в applyControls, и
    /// это не перестановка ради опрятности. Сверяться нужно с тем, что у машины
    /// есть на самом деле, а не с прошлым снимком: прочность, отнятую здесь,
    /// прошлый снимок не заметит никогда — она не менялась у того, кто его
    /// прислал.
    [[nodiscard]] shared::VehicleHarm settleHealth(int vehicle, const shared::VehicleState& state,
                                                   int localPed) const;

    /// Отнимает у машины прочность, о которой сказал сервер.
    ///
    /// Только своей: у чужой прочность принадлежит её ведущему. Отнимается, а не
    /// назначается, — сервер знает убыль, а не остаток: остаток знает та игра, в
    /// которой машина живёт, то есть эта.
    void takeHealth(int vehicle, const shared::VehicleHarm& harm) const;

    /// Накладывает внешность целиком.
    void applyAppearance(int vehicle, const shared::VehicleAppearance& appearance) const;

    /// Что сцеплено с машиной: прицеп либо машина на крюке эвакуатора.
    struct Hitched {
        /// Номер в игре, а не в сессии: о номерах сессии этот класс не знает
        /// вовсе, и переводить один в другой — дело того, кто ведёт список
        /// машин. Ноль — не сцеплено ничего.
        int vehicle = 0;

        /// Висит на крюке, а не на сцепке.
        bool onHook = false;
    };

    [[nodiscard]] Hitched hitchedTo(int vehicle) const;

    /// Сцепляет машину с прицепом или машиной на крюке — или расцепляет её.
    ///
    /// hitched.vehicle равный нулю означает «не сцеплено ничего».
    ///
    /// had говорит, было ли что-то сцеплено в прошлом наложенном снимке.
    /// Расцепляем мы только на переходе, и это существенно: игра сцепляет
    /// прицеп сама, стоит тягачу подать назад, — и расцепляй мы каждый кадр, у
    /// зрителя сцепка разваливалась бы в тот самый миг, когда у хозяина она
    /// только что сложилась.
    void applyHitch(int vehicle, const Hitched& hitched, bool had) const;

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
    NativeHandler trailerOf_ = nullptr;
    NativeHandler hookedOf_ = nullptr;
    NativeHandler attachHook_ = nullptr;
    NativeHandler detachHook_ = nullptr;
    NativeHandler hookAttached_ = nullptr;
    NativeHandler attachTrailer_ = nullptr;
    NativeHandler detachTrailer_ = nullptr;
    NativeHandler trailerAttached_ = nullptr;
    NativeHandler wrecked_ = nullptr;
    NativeHandler explode_ = nullptr;
    NativeHandler setProofs_ = nullptr;
    NativeHandler damagedBy_ = nullptr;
    NativeHandler clearDamage_ = nullptr;
    NativeHandler hornActive_ = nullptr;
    NativeHandler startHorn_ = nullptr;
    NativeHandler roofState_ = nullptr;
    NativeHandler raiseRoof_ = nullptr;
    NativeHandler lowerRoof_ = nullptr;
    NativeHandler convertible_ = nullptr;
    NativeHandler fix_ = nullptr;
    NativeHandler fixDeformation_ = nullptr;
    NativeHandler landingGear_ = nullptr;
    NativeHandler setLandingGear_ = nullptr;
    NativeHandler hasLandingGear_ = nullptr;
    NativeHandler setSteerBias_ = nullptr;
    NativeHandler setHandbrake_ = nullptr;
    NativeHandler setBrakeLights_ = nullptr;

    NativeHandler doorAngle_ = nullptr;
    NativeHandler lockDoors_ = nullptr;
    NativeHandler doorControl_ = nullptr;
    NativeHandler rollDown_ = nullptr;
    NativeHandler rollUp_ = nullptr;
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
    NativeHandler setCustomPrimary_ = nullptr;
    NativeHandler setCustomSecondary_ = nullptr;
    NativeHandler clearCustomPrimary_ = nullptr;
    NativeHandler clearCustomSecondary_ = nullptr;
    NativeHandler getExtraColours_ = nullptr;
    NativeHandler setExtraColours_ = nullptr;
    NativeHandler getPlate_ = nullptr;
    NativeHandler setPlate_ = nullptr;
    NativeHandler getPlateStyle_ = nullptr;
    NativeHandler setPlateStyle_ = nullptr;
    NativeHandler getLivery_ = nullptr;
    NativeHandler setLivery_ = nullptr;
    NativeHandler getDirt_ = nullptr;

    // Неон, дым из-под колёс и дополнения кузова: у alt:V они синхронизируются,
    // а у нас машина без них выглядела заводской, в чём бы её ни собрал хозяин.
    NativeHandler getNeonColour_ = nullptr;
    NativeHandler setNeonColour_ = nullptr;
    NativeHandler neonOn_ = nullptr;
    NativeHandler setNeonOn_ = nullptr;
    NativeHandler getTyreSmoke_ = nullptr;
    NativeHandler setTyreSmoke_ = nullptr;
    NativeHandler extraOn_ = nullptr;
    NativeHandler setExtra_ = nullptr;
    NativeHandler extraExists_ = nullptr;
    NativeHandler getModVariation_ = nullptr;
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
