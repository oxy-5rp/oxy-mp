#include "../src/game/puppet_duty.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace oxymp::client::game;
using oxymp::shared::PlayerFlag;

namespace {

/// Признаки одним числом, чтобы случай читался строкой.
[[nodiscard]] std::uint32_t with(std::initializer_list<PlayerFlag> flags) {
    std::uint32_t value = 0;

    for (const PlayerFlag flag : flags) {
        value |= static_cast<std::uint32_t>(flag);
    }

    return value;
}

/// Падение, которое считается полётом: быстрее порога и без своего движения.
[[nodiscard]] PuppetSituation falling() {
    return PuppetSituation{.flags = with({PlayerFlag::Falling}), .verticalSpeed = -12.0F};
}

} // namespace

TEST_CASE("a puppet nobody else drives is driven by us", "[puppet]") {
    const PuppetDuty duty = dutyFor(PuppetSituation{});

    CHECK(duty.body == PuppetBody::Ours);
    CHECK(duty.tasks);
    CHECK(duty.posture);
    CHECK(duty.action);
    CHECK_FALSE(duty.falling);
}

TEST_CASE("a limp body is left to physics and to nothing else", "[puppet]") {
    // Переставленное каждый кадр обмякшее тело вертится волчком, а ведомое
    // задачей — ползёт по земле на животе. Оба зрелища здесь уже были.
    const PuppetDuty duty = dutyFor(PuppetSituation{.limp = true});

    CHECK(duty.body == PuppetBody::Physics);
    CHECK_FALSE(duty.tasks);
    CHECK_FALSE(duty.posture);
    CHECK_FALSE(duty.action);
}

TEST_CASE("a limp body stays with physics whatever else is going on", "[puppet]") {
    // Рэгдолл сильнее всего остального: и движения сервера, и машины, и входа.
    for (const PuppetSituation situation : {
             PuppetSituation{.limp = true, .scripted = true},
             PuppetSituation{.flags = with({PlayerFlag::InVehicle}), .limp = true,
                             .carried = true},
             PuppetSituation{.flags = with({PlayerFlag::EnteringVehicle}), .limp = true},
             PuppetSituation{.limp = true, .leaving = true},
         }) {
        CHECK(dutyFor(situation).body == PuppetBody::Physics);
    }
}

TEST_CASE("a server animation is never cancelled by a task of ours", "[puppet]") {
    // Всякая наша задача — походка, прицел, поворот головы, удар, поза —
    // отменила бы движение в тот же кадр. Ровно так оно и отменялось, пока
    // распоряжение разбиралось мимо этого разбора.
    const PuppetDuty duty = dutyFor(PuppetSituation{.scripted = true});

    CHECK(duty.body == PuppetBody::Scripted);
    CHECK_FALSE(duty.tasks);
    CHECK_FALSE(duty.posture);
    CHECK_FALSE(duty.action);
}

TEST_CASE("a seated player playing a server animation is not led by position", "[puppet]") {
    // Положение сидящему задаёт машина, а не снимок: подводить его — значит
    // тянуть в две стороны.
    const PuppetSituation seated{
        .flags = with({PlayerFlag::InVehicle}),
        .scripted = true,
        .carried = true,
    };

    CHECK(dutyFor(seated).body == PuppetBody::Boarding);

    // А вот седок машины, которой у нас ещё нет, стоит посреди дороги, и вести
    // его положением по-прежнему приходится нам.
    PuppetSituation waiting = seated;
    waiting.carried = false;

    CHECK(dutyFor(waiting).body == PuppetBody::Scripted);
}

TEST_CASE("climbing in and out belongs to its own task", "[puppet]") {
    for (const PuppetSituation situation : {
             PuppetSituation{.flags = with({PlayerFlag::EnteringVehicle})},
             PuppetSituation{.leaving = true},
         }) {
        const PuppetDuty duty = dutyFor(situation);

        CHECK(duty.body == PuppetBody::Boarding);
        CHECK_FALSE(duty.tasks);

        // Поза при этом разрешена: она не спорит с задачей входа.
        CHECK(duty.posture);
    }
}

TEST_CASE("a passenger of a vehicle we have is carried by it", "[puppet]") {
    const PuppetDuty duty =
        dutyFor(PuppetSituation{.flags = with({PlayerFlag::InVehicle}), .carried = true});

    CHECK(duty.body == PuppetBody::Riding);
    CHECK_FALSE(duty.tasks);
    CHECK_FALSE(duty.falling);
}

TEST_CASE("a passenger of a vehicle we do not have is still led by position", "[puppet]") {
    // Случай не выдуманный: модель машины грузится не мгновенно, и всё это время
    // его персонаж стоял столбом посреди дороги, пока сама машина уезжала.
    const PuppetDuty duty = dutyFor(PuppetSituation{.flags = with({PlayerFlag::InVehicle})});

    CHECK(duty.body == PuppetBody::Ours);
    CHECK_FALSE(duty.tasks);
}

TEST_CASE("a man falling from a roof is not led by his legs", "[puppet]") {
    const PuppetDuty duty = dutyFor(falling());

    CHECK(duty.body == PuppetBody::Ours);
    CHECK(duty.falling);
    CHECK_FALSE(duty.tasks);
}

TEST_CASE("a step off a kerb is not a fall", "[puppet]") {
    // Признак падения игра выставляет и на бордюре. Снимать задачу ходьбы с
    // того, кто просто сошёл со ступеньки, — значит спотыкаться на каждой.
    PuppetSituation kerb = falling();
    kerb.verticalSpeed = -2.0F;

    const PuppetDuty duty = dutyFor(kerb);

    CHECK_FALSE(duty.falling);
    CHECK(duty.tasks);
}

TEST_CASE("the tail of a jump is not a fall either", "[puppet]") {
    // Прыжок к концу спуска набирает те же метры в секунду, что и падение. Сочти
    // мы его падением — и приземление срезалось бы у каждого прыжка.
    PuppetSituation jump = falling();
    jump.flags |= static_cast<std::uint32_t>(PlayerFlag::Jumping);

    CHECK_FALSE(dutyFor(jump).falling);
}

TEST_CASE("a falling vehicle does not throw its passenger out", "[puppet]") {
    // Летящая с обрыва машина считается падением и для своего седока, а снятая с
    // него задача вынесла бы его из неё посреди полёта.
    PuppetSituation inCar = falling();
    inCar.flags |= static_cast<std::uint32_t>(PlayerFlag::InVehicle);
    inCar.carried = true;

    const PuppetDuty duty = dutyFor(inCar);

    CHECK(duty.body == PuppetBody::Riding);
    CHECK_FALSE(duty.falling);
}

TEST_CASE("a parachute is a fall the game plays itself", "[puppet]") {
    PuppetSituation canopy = falling();
    canopy.flags |= static_cast<std::uint32_t>(PlayerFlag::Parachuting);

    const PuppetDuty duty = dutyFor(canopy);

    CHECK_FALSE(duty.falling);

    // И задачей его вести нельзя: парашют — своё движение персонажа.
    CHECK_FALSE(duty.tasks);
}

TEST_CASE("a man busy with his own movement is not led by a walk task", "[puppet]") {
    // Задача ходьбы, выданная поверх прыжка, отменяет прыжок — то есть ровно то,
    // ради чего он и заказан. Именно поэтому раньше из всех признаков было видно
    // только бег.
    for (const PlayerFlag flag : {PlayerFlag::Jumping, PlayerFlag::Climbing,
                                  PlayerFlag::Vaulting, PlayerFlag::InCover,
                                  PlayerFlag::Parachuting, PlayerFlag::GettingUp}) {
        const PuppetDuty duty = dutyFor(PuppetSituation{.flags = with({flag})});

        CHECK(duty.body == PuppetBody::Ours);
        CHECK_FALSE(duty.tasks);

        // Положение при этом подводится, а поза задаётся: тело обязано оказаться
        // там, где хозяин, а прыжок и укрытие как раз позой и заказываются.
        CHECK(duty.posture);
    }
}

TEST_CASE("a swimmer is still led to where he swims", "[puppet]") {
    // Плавание — состояние движения, а не задача, и плывущего вести к цели
    // по-прежнему нужно: иначе он поплывёт на месте.
    const PuppetDuty duty = dutyFor(PuppetSituation{.flags = with({PlayerFlag::Swimming})});

    CHECK(duty.tasks);
}

TEST_CASE("no situation leaves a puppet with neither position nor task", "[puppet]") {
    // Это главное, что здесь проверяется, и проверяется оно перебором.
    //
    // Почти всякая ошибка в этом разборе обращается не в уродство, а в
    // замирание: кукла, у которой отняли и задачи, и подведение положения,
    // просто стоит столбом до конца сессии. Понять по такому зрелищу, какой из
    // шести признаков залип, нельзя ничем — а вот перебрать все их сочетания
    // здесь можно за миллисекунду.
    //
    // Неподвижной кукле разрешено оставаться ровно в трёх случаях, и у каждого
    // есть тот, кто её ведёт вместо нас: физика, задача входа-выхода и машина.
    constexpr PlayerFlag kFlags[] = {
        PlayerFlag::InVehicle,   PlayerFlag::EnteringVehicle, PlayerFlag::Falling,
        PlayerFlag::Jumping,     PlayerFlag::Climbing,        PlayerFlag::InCover,
        PlayerFlag::Parachuting, PlayerFlag::GettingUp,       PlayerFlag::Swimming,
    };

    constexpr std::size_t kCombinations = std::size_t{1} << std::size(kFlags);

    for (std::size_t mask = 0; mask < kCombinations; ++mask) {
        std::uint32_t flags = 0;

        for (std::size_t bit = 0; bit < std::size(kFlags); ++bit) {
            if ((mask & (std::size_t{1} << bit)) != 0) {
                flags |= static_cast<std::uint32_t>(kFlags[bit]);
            }
        }

        for (const bool limp : {false, true}) {
            for (const bool scripted : {false, true}) {
                for (const bool leaving : {false, true}) {
                    for (const bool carried : {false, true}) {
                        for (const float speed : {0.0F, -12.0F}) {
                            const PuppetDuty duty = dutyFor(PuppetSituation{
                                .flags = flags,
                                .verticalSpeed = speed,
                                .limp = limp,
                                .scripted = scripted,
                                .leaving = leaving,
                                .carried = carried,
                            });

                            const bool ledByUs =
                                duty.body == PuppetBody::Ours ||
                                duty.body == PuppetBody::Scripted;

                            const bool ledByAnother =
                                duty.body == PuppetBody::Physics ||
                                duty.body == PuppetBody::Boarding ||
                                duty.body == PuppetBody::Riding;

                            CHECK(ledByUs != ledByAnother);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("nothing but us may be given tasks", "[puppet]") {
    // Задача, выданная поверх чужого распоряжения телом, его и отменяет: поверх
    // рэгдолла — поднимает тело, поверх движения сервера — сносит движение,
    // поверх входа в машину — уводит от двери.
    for (const PuppetSituation situation : {
             PuppetSituation{.limp = true},
             PuppetSituation{.scripted = true},
             PuppetSituation{.flags = with({PlayerFlag::EnteringVehicle})},
             PuppetSituation{.leaving = true},
             PuppetSituation{.flags = with({PlayerFlag::InVehicle}), .carried = true},
         }) {
        CHECK_FALSE(dutyFor(situation).tasks);
    }
}
