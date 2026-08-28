#include "puppet_duty.hpp"

namespace oxymp::client::game {

PuppetDuty dutyFor(const PuppetSituation& situation) noexcept {
    const bool riding = shared::has(situation.flags, shared::PlayerFlag::InVehicle);
    const bool entering = shared::has(situation.flags, shared::PlayerFlag::EnteringVehicle);
    const bool boarding = entering || situation.leaving;

    PuppetDuty duty;

    // Обмякшее тело — раньше всего прочего, и это не порядок ради порядка: пока
    // им распоряжается физика, всё остальное здесь не просто бесполезно, а
    // вредно. Переставленное каждый кадр, оно вертится волчком; ведомое задачей
    // — ползёт на животе.
    if (situation.limp) {
        duty.body = PuppetBody::Physics;
        return duty;
    }

    // Движение сервера — на тех же правах: всякая наша задача отменила бы его в
    // тот же кадр. Раньше именно так и выходило, и `player.playAnimation` у
    // чужого игрока не доживало до следующего кадра ни разу.
    //
    // Положение при этом подводится — но только тому, кого не везёт машина и кто
    // не лезет в дверь. Там положение задаёт не снимок.
    if (situation.scripted) {
        duty.body = situation.carried || boarding ? PuppetBody::Boarding : PuppetBody::Scripted;
        return duty;
    }

    // Дальше поза разрешена всегда: она не спорит ни с машиной, ни с задачей
    // входа — это присед, плавание и короткие движения самого персонажа.
    duty.posture = true;
    duty.action = true;

    // Влезающим и вылезающим распоряжается своя задача: она ведёт персонажа к
    // двери и обратно сама, и вести его при этом ещё и снимками значит тянуть в
    // две стороны.
    if (boarding) {
        duty.body = PuppetBody::Boarding;
        return duty;
    }

    if (situation.carried) {
        duty.body = PuppetBody::Riding;
        return duty;
    }

    duty.body = PuppetBody::Ours;

    // Падение вниз без опоры — не задача, а её отсутствие: своего движения у
    // падающего нет, игра отыгрывает полёт сама, стоит перестать вести его
    // ногами.
    //
    // Сидящего в машине это не касается вовсе: летящая с обрыва машина считается
    // падением и для своего седока, а снятая с него задача вынесла бы его из
    // машины посреди полёта. И только тому, у кого нет своего движения: прыжок,
    // перемах и подъём после падения — задачи, и снятая с них задача это они
    // сами.
    duty.falling = !riding && !ownMovement(situation.flags) &&
                   shared::has(situation.flags, shared::PlayerFlag::Falling) &&
                   !shared::has(situation.flags, shared::PlayerFlag::Jumping) &&
                   !shared::has(situation.flags, shared::PlayerFlag::Parachuting) &&
                   !shared::has(situation.flags, shared::PlayerFlag::Swimming) &&
                   situation.verticalSpeed <= kFreeFallSpeed;

    // Занятый своим движением ведётся им, а не нами. Сидящий в ненайденной
    // машине — тот же случай: положение ему подводим, а походку и направление не
    // трогаем. Ставить его на ноги и заставлять идти незачем — он едет.
    duty.tasks = !riding && !duty.falling && !ownMovement(situation.flags);

    return duty;
}

} // namespace oxymp::client::game
