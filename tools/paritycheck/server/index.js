// Серверная половина стенда: события и свойства, спрошенные у живого сервера.
//
// Дело его — кричать в журнал о каждом событии, которое пришло, и о каждом
// свойстве, которое ответило. Ответ читается глазами, а не сверяется с
// образцом: у большинства из них правильного значения нет вовсе, а неправильное
// видно сразу — пустая строка, ноль вместо хеша, «нигде» вместо места в машине.
//
// Зачем он здесь, а не в наборе, — в README рядом.

const alt = require('alt-server');

const seen = new Set();

function once(what) {
    if (seen.has(what)) {
        return false;
    }

    seen.add(what);
    return true;
}

alt.log('=== paritycheck поднялся ===');

// --- Жизнь ресурса ----------------------------------------------------------

alt.on('resourceStart', (errored) => alt.log(`[ok] resourceStart errored=${errored}`));
alt.on('resourceStop', () => alt.log('[ok] resourceStop'));
alt.on('anyResourceStart', (name) => alt.log(`[ok] anyResourceStart ${name}`));
alt.on('anyResourceStop', (name) => alt.log(`[ok] anyResourceStop ${name}`));
alt.on('serverStarted', () => alt.log('[ok] serverStarted'));

// --- Консоль ----------------------------------------------------------------

alt.on('consoleCommand', (name, ...args) => {
    alt.log(`[ok] consoleCommand "${name}" [${args.join(', ')}]`);

    if (name === 'look' && alt.Player.all.length > 0) {
        report(alt.Player.all[0]);
    }

    if (name === 'face' && alt.Player.all.length > 0) {
        dressUp(alt.Player.all[0]);
    }

    if (name === 'car' && alt.Player.all.length > 0) {
        putInCar(alt.Player.all[0]);
    }
});

// --- Машины -----------------------------------------------------------------

alt.on('playerEnteringVehicle', (player, vehicle, seat) =>
    alt.log(`[ok] playerEnteringVehicle ${player.name} -> ${vehicle.id} место ${seat}`));

alt.on('playerEnteredVehicle', (player, vehicle, seat) => {
    alt.log(`[ok] playerEnteredVehicle ${player.name} -> ${vehicle.id} место ${seat}`);

    // Место водителя у alt:V — единица. Раньше здесь пришла бы минус единица, и
    // всякая проверка «за рулём ли он» не сошлась бы никогда.
    alt.log(`     player.seat = ${player.seat} (у водителя обязана быть 1)`);
    alt.log(`     passengers = ${JSON.stringify(
        Object.fromEntries(Object.entries(vehicle.passengers).map(([at, who]) => [at, who.name])))}`);
});

alt.on('playerLeftVehicle', (player, vehicle, seat) =>
    alt.log(`[ok] playerLeftVehicle ${player.name} <- ${vehicle.id} место ${seat}`));

alt.on('playerChangedVehicleSeat', (player, vehicle, was, now) =>
    alt.log(`[ok] playerChangedVehicleSeat ${player.name}: ${was} -> ${now}`));

// --- Оружие и урон ----------------------------------------------------------

alt.on('playerWeaponChange', (player, was, now) =>
    alt.log(`[ok] playerWeaponChange ${player.name}: ${was} -> ${now}`));

alt.on('weaponDamage', (source, target, weapon, damage) => {
    alt.log(`[ok] weaponDamage ${source?.name} -> ${target.name}, ${damage} из ${weapon}`);
    // Не отменяем: цель проверки — что событие приходит, а не что оно работает
    // щитом. Отмена проверяется отдельной командой.
});

alt.on('playerDamage', (victim, attacker, health, armour, weapon) =>
    alt.log(`[ok] playerDamage ${victim.name}: -${health} здоровья, -${armour} брони ` +
            `от ${attacker?.name ?? 'никого'} из ${weapon}`));

// --- Метаданные -------------------------------------------------------------

alt.on('syncedMetaChange', (entity, key, value, was) =>
    alt.log(`[ok] syncedMetaChange ${key}: ${JSON.stringify(was)} -> ${JSON.stringify(value)}`));

alt.on('globalSyncedMetaChange', (key, value, was) =>
    alt.log(`[ok] globalSyncedMetaChange ${key}: ${JSON.stringify(was)} -> ${JSON.stringify(value)}`));

alt.on('metaChange', (target, key, value) =>
    alt.log(`[ok] metaChange ${key} = ${JSON.stringify(value)}`));

// --- Зоны -------------------------------------------------------------------

alt.on('entityEnterColshape', (shape, entity) =>
    alt.log(`[ok] entityEnterColshape ${entity.name} вошёл в ${shape.id}`));

alt.on('entityLeaveColshape', (shape, entity) =>
    alt.log(`[ok] entityLeaveColshape ${entity.name} вышел из ${shape.id}`));

// --- Машина: гудок, сирена, ведущий -----------------------------------------

alt.on('vehicleHorn', (vehicle, player, state) =>
    alt.log(`[ok] vehicleHorn #${vehicle.id} от ${player?.name}: ${state}`));

alt.on('vehicleSiren', (vehicle, state) =>
    alt.log(`[ok] vehicleSiren #${vehicle.id}: ${state}`));

alt.on('netOwnerChange', (vehicle, owner, was) =>
    alt.log(`[ok] netOwnerChange #${vehicle.id}: ${was?.name ?? 'никто'} -> ${owner?.name ?? 'никто'}`));

// --- Вход -------------------------------------------------------------------

alt.on('playerConnect', (player) => {
    alt.log(`=== вошёл ${player.name} ===`);
    alt.log(`[личность] hwidHash=${player.hwidHash}, socialID=${player.socialID}, ` +
            `socialClubName=${JSON.stringify(player.socialClubName)}`);

    // Зона вокруг точки появления: шагнув в сторону и обратно, игрок увидит оба
    // события.
    const shape = new alt.ColshapeSphere(player.pos.x, player.pos.y, player.pos.z, 8);
    shape.playersOnly = true;

    alt.setSyncedMeta('paritycheck:started', Date.now());
    player.setSyncedMeta('paritycheck:name', player.name);
    player.setMeta('paritycheck:local', true);

    setTimeout(() => report(player), 3000);

    // Дальше — само, без консоли: сервер запущен скрытым окном, и подать ему
    // команду руками нельзя.
    setTimeout(() => dressUp(player), 6000);
    setTimeout(() => putInCar(player), 9000);
    setTimeout(() => checkBlips(player), 25000);
    setTimeout(() => report(player), 20000);

    // Заморозка и неуязвимость: раньше их не было вовсе. Держим четыре
    // секунды — заморозка навсегда была бы неотличима от зависшей игры.
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        player.frozen = true;
        player.invincible = true;
        alt.log(`[ok] заморожен=${player.frozen}, неуязвим=${player.invincible}`);
    }, 12000);

    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        player.frozen = false;
        alt.log(`[ok] отпущен: заморожен=${player.frozen}, неуязвим=${player.invincible}`);
    }, 45000);
});

// --- Что мы теперь умеем спросить -------------------------------------------

function report(player) {
    if (!player.valid) {
        return;
    }

    alt.log('--- признаки состояния (раньше их не было видно вовсе) ---');

    for (const flag of ['isDead', 'isAiming', 'isShooting', 'isInRagdoll', 'isJumping',
                        'isCrouching', 'isParachuting', 'isReloading', 'isInCover',
                        'isInMelee', 'isEnteringVehicle', 'isLeavingVehicle', 'isInWater',
                        'isSpawned']) {
        alt.log(`     ${flag} = ${player[flag]}`);
    }

    alt.log(`     aimPos = ${JSON.stringify(player.aimPos)}`);
    alt.log(`     currentWeapon = ${player.currentWeapon}`);
    alt.log(`     seat = ${player.seat}`);
    alt.log(`     weapons = ${JSON.stringify(player.weapons)}`);

    alt.log('--- внешность (раньше её нельзя было ни назначить, ни прочесть) ---');
    alt.log(`     getClothes(11) = ${JSON.stringify(player.getClothes(11))}`);
    alt.log(`     getHeadBlendData = ${JSON.stringify(player.getHeadBlendData())}`);
    alt.log(`     getHairColor = ${player.getHairColor()}, getEyeColor = ${player.getEyeColor()}`);
    alt.log(`     getFaceFeatureScale(3) = ${player.getFaceFeatureScale(3)?.toFixed(3)}, ` +
            `(0) = ${player.getFaceFeatureScale(0)?.toFixed(3)}`);

    alt.log('--- кто рядом (раньше спросить было нечем) ---');
    alt.log(`     getEntitiesInRange = ${
        alt.getEntitiesInRange(player.pos, 50, player.dimension).length}`);
    alt.log(`     getEntitiesInDimension = ${
        alt.getEntitiesInDimension(player.dimension).length}`);
    alt.log(`     getClosestPlayer = ${
        alt.getClosestPlayer({ pos: player.pos, range: 100 })?.name ?? 'никого'}`);
    alt.log(`     getClosestVehicle = ${
        alt.getClosestVehicle({ pos: player.pos, range: 100 })?.id ?? 'ничего'}`);
    alt.log(`     hasResource('paritycheck') = ${alt.hasResource('paritycheck')}`);
    alt.log(`     getAllResources = ${alt.getAllResources().map((r) => r.name).join(', ')}`);
    alt.log(`     stringToSHA256('oxy') = ${alt.stringToSHA256('oxy').slice(0, 16)}...`);

    alt.time('замер');
    alt.timeEnd('замер');

    alt.log('--- отказы, которые обязаны быть слышны ---');

    for (const absent of ['isOnLadder', 'isSuperJumpEnabled']) {
        try {
            alt.log(`     ${absent} = ${player[absent]}`);
            alt.log(`     !! ${absent} обязан был отказать вслух`);
        } catch (failure) {
            alt.log(`[ok] ${absent} отказывает вслух: ${failure.message}`);
        }
    }
}

function dressUp(player) {
    alt.log('--- назначаем внешность ---');

    player.setHeadBlendData(21, 33, 0, 14, 27, 0, 0.75, 0.25, 0);
    player.setHairColor(12);
    player.setHairHighlightColor(3);
    player.setEyeColor(7);
    player.setHeadOverlay(1, 5, 0.8);
    player.setHeadOverlayColor(1, 1, 4, 4);
    player.setClothes(11, 15, 0, 0);

    // И вещь из дополнений: её номер больше байта, и до расширения поля она
    // была недостижима — просивший триста получал сорок четвёртое.
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        player.setClothes(11, 300, 0, 0);

        setTimeout(() => {
            alt.log(`[одежда] просили 300, надето ` +
                    `${JSON.stringify(player.getClothes(11))}`);
        }, 3000);
    }, 6000);
    player.setFaceFeature(3, 0.5);
    player.setFaceFeature(0, -0.25);

    // Татуировки: набор и рисунок именами, как их зовут режимы.
    player.addDecoration('mpbeach_overlays', 'FM_hair_fuzz');
    player.addDecoration('multiplayer_overlays', 'FM_Tat_M_000');
    alt.log(`[ok] татуировок: ${player.getDecorations().length}, ` +
            `первая ${JSON.stringify(player.getDecorations()[0])}`);

    alt.log(`[ok] назначено; обратно читается ` +
            `${JSON.stringify(player.getHeadBlendData())}, волосы ${player.getHairColor()}/` +
            `${player.getHairHighlightColor()}, глаза ${player.getEyeColor()}`);
}

function putInCar(player) {
    // Место у машины своё, а не «рядом с игроком»: в первые секунды игрок ещё
    // падает к точке появления, мир под ним не загружен, и заведённая там
    // машина висит в воздухе. Дверь, которой не во что упереться, открывается
    // на семь сотых и там остаётся — а со стороны это неотличимо от «натив не
    // работает». Точка взята с той же полосы аэропорта, что и spawn.
    const car = new alt.Vehicle('adder', -1030.0, -2738.0, 20.2, 0, 0, 0);

    alt.log(`[ok] машина ${car.id} заведена на ${JSON.stringify(car.pos)}`);

    // Замок ставится не сразу, а после проверки дверей: запертая машина
    // закрывает их сама, и наложенные степени пропадали бы по её вине.
    setTimeout(() => {
        if (!car.valid) {
            return;
        }

        car.lockState = 2;
        alt.log(`[ok] lockState = ${car.lockState} (2 — заперта)`);
    }, 40000);

    // Двери: багажник настежь, водительская приоткрыта на три ступени из семи.
    // Двери — не раньше, чем мир вокруг машины загрузится у ведущего: до того
    // её у клиента нет вовсе, и распоряжение уходит в пустоту.
    setTimeout(() => {
        if (!car.valid) {
            return;
        }

        // Двери берутся те, что у модели есть: у `adder` нет багажника, и
        // объявленная ему степень не сдвинется никогда — проверка на нём
        // проверяла бы отсутствие двери, а не нашу работу.
        car.setDoorState(0, 7);
        car.setDoorState(1, 3);

        alt.log(`[ok] двери: водительская ${car.getDoorState(0)}, ` +
                `пассажирская ${car.getDoorState(1)}, задняя ${car.getDoorState(3)}`);

        // Стёкла и крыша: первое — распоряжение сервера, второе — снимок
        // ведущего.
        car.setWindowOpened(0, true);
        car.setWindowOpened(1, true);

        alt.emitAllClients('паритет:стёкла', car.id);

        alt.log(`[стёкла] водительское ${car.isWindowOpened(0)}, ` +
                `пассажирское ${car.isWindowOpened(1)}, ` +
                `заднее ${car.isWindowOpened(2)}, крыша ${car.roofState}`);

        // И подвинем игрока к машине: стёкла проверяются глазами — прочесть их
        // у игры нечем.
        setTimeout(() => {
            const кто = alt.Player.all[0];

            if (кто !== undefined) {
                кто.pos = { x: car.pos.x + 3, y: car.pos.y + 3, z: car.pos.z };
                alt.log('[стёкла] игрок поставлен рядом с машиной');
            }
        }, 2000);

        // Дальше — раз в секунду. Немедленное чтение отдаёт память сервера,
        // которую мы только что и записали; настоящий ответ игры приходит со
        // снимком ведущего и перекрывает её со следующего же такта.
        let sample = 0;
        const watch = setInterval(() => {
            if (!car.valid || ++sample > 12) {
                clearInterval(watch);
                return;
            }

            alt.log(`[двери] ${sample}с: водительская ${car.getDoorState(0)}, ` +
                    `пассажирская ${car.getDoorState(1)}`);
        }, 1000);
    }, 12000);

    // Поворот: раньше присваивание молча не делало ничего, а чтение отдавало
    // градусы там, где ждут радианы.
    alt.log(`     rot до = ${JSON.stringify(car.rot)}`);
    car.rot = new alt.Vector3(0, 0, Math.PI / 2);
    alt.log(`     rot после = ${JSON.stringify(car.rot)}`);

    setTimeout(() => {
        if (!car.valid) {
            return;
        }

        alt.log(`     bodyHealth = ${car.bodyHealth}, engineHealth = ${car.engineHealth}, ` +
                `petrolTankHealth = ${car.petrolTankHealth}`);
        alt.log(`     engineOn = ${car.engineOn}, sirenActive = ${car.sirenActive}, ` +
                `destroyed = ${car.destroyed}`);
        alt.log(`     velocity = ${JSON.stringify(car.velocity)}`);

        // Место водителя у alt:V — единица.
        // За руль в этот заход не сажаем: персонажа надо разглядеть целиком.
        alt.log('[ok] машина рядом, за руль не сажаем — смотрим на персонажа');
    }, 1500);

    // Предмет: раньше его нельзя было переставить вовсе.
    const box = new alt.Object('prop_box_wood02a', player.pos.x - 3, player.pos.y, player.pos.z,
                               0, 0, 0);
    alt.log(`[ok] предмет ${box.id} поставлен`);

    setTimeout(() => {
        if (!box.valid) {
            return;
        }

        box.pos = new alt.Vector3(player.pos.x - 3, player.pos.y + 3, player.pos.z);
        alt.log(`[ok] предмет переставлен: ${JSON.stringify(box.pos)}`);
    }, 3000);
}

// --- Что говорит клиент -----------------------------------------------------

alt.onClient('paritycheck:say', (player, line) => {
    if (once(`client:${line}`)) {
        alt.log(`[клиент] ${line}`);
    }
});

// --- Метки: приоритет и список тех, кому они видны -------------------------

function checkBlips(player) {
    if (!player.valid) {
        return;
    }

    const mine = new alt.PointBlip(player.pos.x + 6, player.pos.y, player.pos.z);
    mine.sprite = 402;
    mine.name = 'Моя';
    mine.priority = 5;
    mine.addTarget(player);
    mine.update();

    const alien = new alt.PointBlip(player.pos.x + 10, player.pos.y, player.pos.z);
    alien.sprite = 402;
    alien.name = 'Чужая';
    alien.addTarget(9999);
    alien.update();

    const global = new alt.PointBlip(player.pos.x + 14, player.pos.y, player.pos.z);
    global.sprite = 402;
    global.name = 'Общая';
    global.update();

    try {
    alt.log(`[метки] моя: список ${mine.targets.map((one) => one.name).join(',')}, ` +
            `глобальная ${mine.isGlobal}, приоритет ${mine.priority}`);
    alt.log(`[метки] чужая: глобальная ${alien.isGlobal}, ` +
            `в списке ${alien.targets.length} живых`);
    alt.log(`[метки] общая: глобальная ${global.isGlobal}`);
    } catch (failure) {
        alt.log(`[!!] метки упали: ${failure.stack ?? failure.message}`);
    }

    // И снимем список у своей — она обязана стать общей.
    setTimeout(() => {
        mine.removeTarget(player);
        alt.log(`[метки] после removeTarget моя глобальная: ${mine.isGlobal}`);
    }, 4000);
}

// --- Отказ во входе ---------------------------------------------------------
//
// Игрока здесь нет и быть не может: отказ случается раньше, чем игрок заведён.
alt.on('playerConnectDenied', (reason, name, ip) =>
    alt.log(`[отказ] причина ${reason}, назвался ${JSON.stringify(name)}, адрес ${ip}`));

// --- Рождение и смерть объектов слоя ----------------------------------------
let рождено = 0;
let убрано = 0;

alt.on('baseObjectCreate', (object) => {
    ++рождено;

    if (рождено <= 3) {
        alt.log(`[объекты] родился ${object.type} #${object.id}`);
    }
});

alt.on('baseObjectRemove', (object) => {
    ++убрано;
    alt.log(`[объекты] убран ${object.type} #${object.id}, valid=${object.valid}`);
});

setTimeout(() => alt.log(`[объекты] всего рождено ${рождено}, убрано ${убрано}`), 45000);

// --- Оружие для проверки выстрела -------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        const хеш = alt.hash('weapon_pistol');
        const дано = player.giveWeapon(хеш, 250, true);

        alt.log(`[выстрел] пистолет ${хеш}: giveWeapon вернул ${дано}, ` +
                `оружие в руках ${player.currentWeapon}, ` +
                `набор ${JSON.stringify(player.weapons)}`);

        setTimeout(() => {
            alt.log(`[выстрел] спустя 2с: в руках ${player.currentWeapon}`);
            alt.emitClient(player, 'паритет:стреляй');
        }, 2000);
    }, 55000);
});

// --- Патроны, состав оружия и предел брони ----------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        const пистолет = alt.hash('weapon_pistol');

        player.giveWeapon(пистолет, 250, true);
        player.addWeaponComponent(пистолет, alt.hash('COMPONENT_AT_PI_FLSH'));
        player.setWeaponTintIndex(пистолет, 3);

        setTimeout(() => {
            alt.log(`[оружие] в руках ${player.currentWeapon}, ` +
                    `насадки ${JSON.stringify(player.currentWeaponComponents)}, ` +
                    `расцветка ${player.currentWeaponTintIndex}`);
            alt.log(`[оружие] фонарь стоит: ` +
                    `${player.hasWeaponComponent(пистолет, alt.hash('COMPONENT_AT_PI_FLSH'))}, ` +
                    `глушитель: ${player.hasWeaponComponent(пистолет, 12345)}`);

            player.setWeaponAmmo(пистолет, 7);
            alt.log(`[оружие] патронов после setWeaponAmmo: ` +
                    `${player.getWeaponAmmo(пистолет)}`);

            player.maxArmour = 200;
            player.armour = 200;
            alt.log(`[броня] предел ${player.maxArmour}, надето ${player.armour}`);

            player.maxArmour = 50;
            alt.log(`[броня] предел опущен до ${player.maxArmour}, осталось ${player.armour}`);
        }, 3000);
    }, 62000);
});

// --- Скорости и руль --------------------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        alt.log(`[скорость] полная ${player.moveSpeed.toFixed(2)}, ` +
                `вперёд ${player.forwardSpeed.toFixed(2)}, ` +
                `вбок ${player.strafeSpeed.toFixed(2)}, ` +
                `вектор ${JSON.stringify(player.velocity)}`);

        const car = alt.Vehicle.all[0];

        if (car !== undefined) {
            alt.log(`[руль] ${car.steeringAngle.toFixed(3)}`);
        }
    }, 70000);
});

// --- Скорости у того, кто действительно идёт --------------------------------
//
// Своего игрока в проверке почти всегда держат стоящим, и нули у него честны, но
// ничего не доказывают. Ходят боты — на них и смотрим.
let сказано = 0;

setInterval(() => {
    if (сказано >= 3) {
        return;
    }

    for (const кто of alt.Player.all) {
        if (кто.moveSpeed < 0.5) {
            continue;
        }

        ++сказано;
        alt.log(`[скорость] ${кто.name}: полная ${кто.moveSpeed.toFixed(2)}, ` +
                `вперёд ${кто.forwardSpeed.toFixed(2)}, ` +
                `вбок ${кто.strafeSpeed.toFixed(2)}, ` +
                `курс ${кто.heading.toFixed(0)}°`);
        break;
    }
}, 2000);

// --- Личные метаданные ------------------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        player.setLocalMeta('тайна', { счёт: 42 });

        alt.log(`[личное] у сервера: ${JSON.stringify(player.getLocalMeta('тайна'))}, ` +
                `есть ${player.hasLocalMeta('тайна')}, ` +
                `ключи ${JSON.stringify(player.getLocalMetaKeys())}`);

        setTimeout(() => {
            player.deleteLocalMeta('тайна');
            alt.log(`[личное] после удаления: есть ${player.hasLocalMeta('тайна')}`);
        }, 4000);
    }, 20000);
});

// --- Сценарий ---------------------------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        const дано = player.playScenario('WORLD_HUMAN_SMOKING');

        alt.log(`[сценарий] WORLD_HUMAN_SMOKING: ${дано}`);
    }, 75000);
});

// Просим клиента сесть в машину — иначе событий посадки не увидеть.
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        const car = alt.Vehicle.all.find((one) => one.model === alt.hash('adder'));

        if (car !== undefined && player.valid) {
            alt.emitClient(player, 'паритет:садись', car.id);
        }
    }, 50000);
});

// --- Лечение и личные метаданные: события ------------------------------------
alt.on('playerHeal', (кто, здоровьеБыло, здоровьеСтало, броняБыла, броняСтала) =>
    alt.log(`[ok] playerHeal ${кто.name}: здоровье ${здоровьеБыло}->${здоровьеСтало}, ` +
            `броня ${броняБыла}->${броняСтала}`));

alt.on('localMetaChange', (кто, ключ, стало, было) =>
    alt.log(`[ok] localMetaChange ${ключ}: ${JSON.stringify(было)} -> ${JSON.stringify(стало)}`));

alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (player.valid) {
            player.health = 120;
            player.health = 190;
        }
    }, 28000);
});

// --- Сцепка машин -----------------------------------------------------------
alt.on('vehicleAttach', (тягач, прицеп) =>
    alt.log(`[ok] vehicleAttach ${тягач?.id} <- ${прицеп?.id}`));

alt.on('vehicleDetach', (тягач, прицеп) =>
    alt.log(`[ok] vehicleDetach ${тягач?.id} <- ${прицеп?.id}`));

alt.on('playerDimensionChange', (кто, было, стало) =>
    alt.log(`[ok] playerDimensionChange ${кто.name}: ${было} -> ${стало}`));

alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (player.valid) {
            player.dimension = 42;
            player.dimension = 0;
        }
    }, 32000);
});

// --- Прохожие и уход сущностей ----------------------------------------------
alt.on('pedHeal', (кукла, зБыло, зСтало) =>
    alt.log(`[ok] pedHeal #${кукла?.id}: здоровье ${зБыло} -> ${зСтало}`));

alt.on('pedDeath', (кукла, убийца, оружие) =>
    alt.log(`[ok] pedDeath #${кукла?.id}, убийца ${убийца}, оружие ${оружие}`));

alt.on('removeEntity', (сущность) =>
    alt.log(`[ok] removeEntity #${сущность?.id}`));

alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        const кукла = new alt.Ped('a_m_y_business_01', player.pos, 0);

        кукла.health = 40;
        кукла.health = 190;
        кукла.health = 0;

        setTimeout(() => кукла.destroy(), 2000);
    }, 36000);
});

// --- Общность метки ---------------------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        const общая = new alt.PointBlip(player.pos.x, player.pos.y, player.pos.z);
        const ничья = new alt.PointBlip(player.pos.x + 5, player.pos.y, player.pos.z, false);
        const поточке = new alt.PointBlip(player.pos, true);

        общая.name = 'общая';
        ничья.name = 'ничья';
        поточке.name = 'по точке';
        общая.update();
        ничья.update();
        поточке.update();

        alt.log(`[ok] blip.isGlobal: общая ${общая.isGlobal}, ничья ${ничья.isGlobal}, `
                + `по точке ${поточке.isGlobal}`);

        общая.addTarget(player);
        alt.log(`[ok] общая после addTarget осталась общей: ${общая.isGlobal}`);

        ничья.addTarget(player);
        alt.log(`[ok] ничья после addTarget: получателей ${ничья.targets.length}`);
    }, 30000);
});

// --- Внешность машины одной строкой -----------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        const машина = new alt.Vehicle('sultan', player.pos, new alt.Vector3(0, 0, 0));

        машина.primaryColor = 12;
        машина.setMod(11, 3);
        машина.numberPlateText = 'OXYMP';

        const запись = машина.getAppearanceDataBase64();
        alt.log(`[ok] getAppearanceDataBase64: ${запись.length} знаков`);

        машина.primaryColor = 0;
        машина.setMod(11, 0);
        машина.numberPlateText = 'ДРУГОЕ';

        машина.setAppearanceDataBase64(запись);
        alt.log(`[ok] после восстановления: цвет ${машина.primaryColor}, `
                + `деталь ${машина.getMod(11)}, знак «${машина.numberPlateText}»`);

        try {
            машина.setAppearanceDataBase64(Buffer.from('{"чужое":1}').toString('base64'));
            alt.log('[!!] чужая запись принята молча');
        } catch (беда) {
            alt.log(`[ok] чужая запись отказана вслух: ${беда.message.slice(0, 50)}`);
        }

        setTimeout(() => машина.destroy(), 3000);
    }, 42000);
});

// --- Чем сервер объявил себя ------------------------------------------------
alt.on('playerConnect', () => {
    setTimeout(() => {
        const настройки = alt.getServerConfig();

        alt.log(`[ok] getServerConfig: «${настройки.name}», порт ${настройки.port}, `
                + `мест ${настройки.players}, тактов ${настройки.tickRate}, `
                + `дальность ${настройки.streamingDistance}, пароль ${настройки.passworded}, `
                + `debug ${настройки.debug}, ресурсов ${настройки.resources.length}`);
        alt.log(`[ok] getServerConfig.resources: ${настройки.resources.join(', ')}`);
        alt.log(`[ok] пароля в настройках нет: ${настройки.password === undefined}`);
    }, 48000);
});

// --- Управление ресурсами ---------------------------------------------------
setTimeout(() => {
    alt.log(`[ok] restartResource соседа: ${alt.restartResource('selfstop')}`);
}, 40000);

setTimeout(() => {
    alt.log(`[ok] startResource несуществующего: ${alt.startResource('нетТакого')}`);
    alt.log('[ok] сервер жив спустя всё это');
}, 50000);

// --- Стоит ли игрок на машине -----------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        alt.log(`[ok] isOnVehicle до: ${player.isOnVehicle}, `
                + `isStealthy ${player.isStealthy}, isCrouching ${player.isCrouching}`);

        // Ставим машину под ногами и роняем игрока на её крышу.
        const машина = new alt.Vehicle('rumpo', player.pos, new alt.Vector3(0, 0, 0));

        setTimeout(() => {
            player.pos = new alt.Vector3(машина.pos.x, машина.pos.y, машина.pos.z + 2.5);

            setTimeout(() => {
                alt.log(`[ok] isOnVehicle на крыше: ${player.isOnVehicle}, `
                        + `в машине ${player.vehicle !== null}`);
                машина.destroy();
            }, 4000);
        }, 2000);
    }, 55000);
});

// --- Справочники моделей ----------------------------------------------------
setTimeout(() => {
    const машина = alt.getVehicleModelInfoByHash(alt.hash('sultan'));

    if (машина === null) {
        alt.log('[!!] справочник машин пуст');
    } else {
        alt.log(`[ok] vehicleModelInfo: «${машина.title}», колёс ${машина.wheelsCount}, `
                + `цвет ${машина.primaryColor}, костей ${машина.bones.length}, `
                + `набор ${машина.modKit}, прицеп ${машина.hasAutoAttachTrailer}`);
        alt.log(`[ok] hasExtra(1)=${машина.hasExtra(1)} hasExtra(9)=${машина.hasExtra(9)} `
                + `availableModkits[${машина.modKit}]=${машина.availableModkits[машина.modKit]}`);
    }

    const человек = alt.getPedModelInfoByHash('mp_m_freemode_01');
    alt.log(человек === null ? '[!!] справочник людей пуст'
            : `[ok] pedModelInfo: «${человек.name}», род ${человек.type}, `
              + `костей ${человек.bones.length}, кулак ${человек.defaultUnarmedWeapon}`);

    const ствол = alt.getWeaponModelInfoByHash('WEAPON_PISTOL');
    alt.log(ствол === null ? '[!!] справочник оружия пуст'
            : `[ok] weaponModelInfo: «${ствол.name}», модель ${ствол.modelName} `
              + `(${ствол.modelHash}), патронов ${ствол.defaultMaxAmmoMp}, ${ствол.damageType}`);

    alt.log(`[ok] нет такой модели -> ${alt.getVehicleModelInfoByHash('нетТакой')}`);
}, 8000);

alt.on('playerConnect', (player) => {
    setTimeout(() => {
        const тачка = new alt.Vehicle('sultan', player.pos, new alt.Vector3(0, 0, 0));

        alt.log(`[ok] getModsCount(11)=${тачка.getModsCount(11)} `
                + `getModsCount(23)=${тачка.getModsCount(23)}`);

        setTimeout(() => тачка.destroy(), 2000);
    }, 62000);
});

// --- Справочник против самой игры -------------------------------------------
//
// Самая крепкая проверка разбора: справочник говорит, какие дополнения кузова у
// модели стоят по умолчанию, а игра у клиента отвечает, какие стоят на самом
// деле. Сойдутся — значит и биты опознаны верно, и нумерация та же.
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        const справка = alt.getVehicleModelInfoByHash('police');
        const машина = new alt.Vehicle('police', player.pos, new alt.Vector3(0, 0, 0));

        const ждём = [];
        for (let id = 1; id <= 14; id += 1) {
            if (справка.hasDefaultExtra(id)) {
                ждём.push(id);
            }
        }

        // Внешность приезжает от того, кто машину ведёт: даём ей доехать.
        setTimeout(() => {
            const есть = [];
            for (let id = 1; id <= 14; id += 1) {
                if (машина.getExtra(id)) {
                    есть.push(id);
                }
            }

            alt.log(`[ok] справочник против игры: ждали ${JSON.stringify(ждём)}, `
                    + `в игре ${JSON.stringify(есть)}, сошлось `
                    + `${JSON.stringify(ждём) === JSON.stringify(есть)}`);

            машина.destroy();
        }, 6000);
    }, 70000);
});

// --- Оружие для проверки клиентского чтения ---------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (player.valid) {
            player.giveWeapon(alt.hash('weapon_pistol'), 60, true);
            alt.log('[ok] пистолет выдан для клиентской проверки');
        }
    }, 50000);
});

// --- Попадание по прохожему -------------------------------------------------
alt.on('pedDamage', (кукла, ударивший, здоровью, броне, оружие) =>
    alt.log(`[ok] pedDamage #${кукла?.id}: ударил ${ударивший?.name}, `
            + `здоровью ${здоровью}, броне ${броне}, оружие ${оружие}`));

alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        const жертва = new alt.Ped('a_m_y_business_01',
                                   new alt.Vector3(player.pos.x + 2, player.pos.y,
                                                   player.pos.z),
                                   0);

        жертва.armour = 30;
        alt.log(`[..] прохожий для стрельбы заведён: #${жертва.id}, `
                + `здоровье ${жертва.health}, броня ${жертва.armour}`);

        setTimeout(() => жертва.destroy(), 90000);
    }, 66000);
});

// --- Признаки метки ---------------------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        const метка = new alt.PointBlip(player.pos.x + 12, player.pos.y, player.pos.z);

        метка.sprite = 1;
        метка.color = 1;
        метка.name = 'проба признаков';
        метка.flashes = true;
        метка.bright = true;
        метка.showCone = true;
        метка.isFriendly = true;
        метка.highDetail = true;
        метка.tickVisible = true;
        метка.number = 7;
        метка.flashInterval = 300;
        метка.flashTimer = 60000;
        метка.secondaryColor = new alt.RGBA(255, 0, 0, 255);
        метка.update();

        alt.log(`[ok] признаки метки: flags=${метка.flags}, мигает ${метка.flashes}, `
                + `яркая ${метка.bright}, конус ${метка.showCone}, `
                + `галочка ${метка.tickVisible}, номер ${метка.number}`);
        alt.log(`[ok] второй цвет: ${JSON.stringify(метка.secondaryColor)}, род `
                + `${метка.blipType}, привязана ${метка.isAttached}`);

        метка.secondaryColor = null;
        alt.log(`[ok] второй цвет снят: ${метка.secondaryColor}`);
        метка.secondaryColor = new alt.RGBA(255, 0, 0, 255);
        метка.update();
    }, 80000);
});

// --- Записи состояния машины -------------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        const тачка = new alt.Vehicle('sultan', player.pos, new alt.Vector3(0, 0, 0));

        alt.log(`[ok] vehicle.quaternion: ${JSON.stringify(тачка.quaternion)}, `
                + `крыша закрыта ${тачка.roofClosed}`);

        тачка.bodyHealth = 700;
        тачка.engineHealth = 600;
        тачка.setWindowOpened(0, true);
        тачка.setDoorState(1, 3);

        const здоровье = тачка.getHealthDataBase64();
        const урон = тачка.getDamageStatusBase64();

        тачка.bodyHealth = 1000;
        тачка.engineHealth = 1000;
        тачка.setWindowOpened(0, false);
        тачка.setDoorState(1, 0);

        const принято = тачка.setHealthDataBase64(здоровье);
        тачка.setDamageStatusBase64(урон);

        alt.log(`[ok] после записи: прочность наложена ${принято} (ждём false — `
                + `её назначает ведущий), окно ${тачка.isWindowOpened(0)}, `
                + `дверь ${тачка.getDoorState(1)}`);

        try {
            тачка.setHealthDataBase64(Buffer.from('{"чужое":1}').toString('base64'));
            alt.log('[!!] чужая запись прочностей принята молча');
        } catch (беда) {
            alt.log('[ok] чужая запись прочностей отказана вслух');
        }

        for (const имя of ['getGamestateDataBase64', 'getScriptDataBase64']) {
            try {
                тачка[имя]();
                alt.log(`[!!] ${имя} промолчал`);
            } catch (беда) {
                alt.log(`[ok] ${имя} отказал вслух`);
            }
        }

        setTimeout(() => тачка.destroy(), 4000);
    }, 86000);
});

// --- Снятие внешности и дальность раздачи -----------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        alt.log(`[ok] removeHeadOverlay: ${player.removeHeadOverlay(1)}`);
        alt.log(`[ok] removeFaceFeature: ${player.removeFaceFeature(0)}`);
        player.removeHeadBlendData();
        alt.log('[ok] removeHeadBlendData не бросил');

        const рядом = new alt.Vehicle('sultan', player.pos, new alt.Vector3(0, 0, 0));
        const далеко = new alt.Vehicle('sultan',
                                       new alt.Vector3(player.pos.x + 9000, player.pos.y,
                                                       player.pos.z),
                                       new alt.Vector3(0, 0, 0));

        setTimeout(() => {
            alt.log(`[ok] isEntityInStreamRange: рядом `
                    + `${player.isEntityInStreamRange(рядом)}, далеко `
                    + `${player.isEntityInStreamRange(далеко)}, пустота `
                    + `${player.isEntityInStreamRange(null)}`);

            рядом.destroy();
            далеко.destroy();
        }, 2000);
    }, 94000);
});

// --- Сквозная проба записи ---------------------------------------------------
//
// Обход чтением отвечает, работает ли вопрос. Этот отвечает на другое: делает ли
// что-нибудь **распоряжение**. Свойство, принявшее присваивание и не изменившееся,
// для всех прежних проверок выглядит исправным — исключения нет, строки в журнале
// нет, — а режим, положивший в него значение, узнает об этом в игре и не сразу.
//
// Так уже ловилось дважды: `setHealthDataBase64` объявлял успех, ничего не делая,
// и `giveWeapon` соглашался вооружить человека третьим доводом, которого не читал.
//
// Значение подбирается по нынешнему: число — соседнее, признак — обратный, строка
// — своя, вектор и цвет — сдвинутые. Прочитанное сверяется, прежнее возвращается
// назад: стенд не должен оставлять после себя изменённую сессию.

/// Чего не трогать: перезаводит тело, уносит игрока или рвёт связь.
const неТрогать = new Set([
    'model', 'dimension', 'pos', 'rot', 'position', 'rotation',
    'ip', 'socialID', 'hwidHash', 'hwidExHash', 'authToken', 'discordUser',
]);

function подобратьЗначение(было) {
    if (typeof было === 'number') {
        return Number.isInteger(было) ? было + 1 : было + 0.5;
    }

    if (typeof было === 'boolean') {
        return !было;
    }

    if (typeof было === 'string') {
        return `oxymp-${было.length}`;
    }

    if (было !== null && typeof было === 'object') {
        if ('x' in было && 'y' in было && 'z' in было) {
            return { x: было.x + 1, y: было.y, z: было.z };
        }

        if ('r' in было && 'g' in было && 'b' in было) {
            return { r: 11, g: 22, b: 33, a: 255 };
        }
    }

    return undefined;
}

function одинаковы(a, b) {
    if (a !== null && typeof a === 'object' && b !== null && typeof b === 'object') {
        return ['x', 'y', 'z', 'r', 'g', 'b', 'a']
            .every((ось) => !(ось in a) || Math.abs((a[ось] ?? 0) - (b[ось] ?? 0)) < 0.01);
    }

    return typeof a === 'number' && typeof b === 'number'
        ? Math.abs(a - b) < 0.01
        : a === b;
}

function пробаЗаписи(имя, объект) {
    if (объект === null || объект === undefined) {
        alt.log(`[..] ${имя}: нечего пробовать`);
        return;
    }

    const молчат = [];
    let проверено = 0;

    for (let слой = объект; слой !== null; слой = Object.getPrototypeOf(слой)) {
        for (const ключ of Object.getOwnPropertyNames(слой)) {
            const опись = Object.getOwnPropertyDescriptor(слой, ключ);

            if (ключ === 'constructor' || опись === undefined || неТрогать.has(ключ) ||
                typeof опись.value === 'function') {
                continue;
            }

            let было;

            try {
                было = объект[ключ];
            } catch (беда) {
                continue;
            }

            const станет = подобратьЗначение(было);

            if (станет === undefined) {
                continue;
            }

            try {
                объект[ключ] = станет;
            } catch (беда) {
                // Громкий отказ — это правильно, а не находка.
                continue;
            }

            проверено += 1;

            let стало;

            try {
                стало = объект[ключ];
            } catch (беда) {
                continue;
            }

            if (одинаковы(было, стало)) {
                молчат.push(ключ);
            }

            // Возвращаем как было: стенд не оставляет после себя следов.
            try {
                объект[ключ] = было;
            } catch (беда) {
                // Не вернулось — не беда: значение всё равно из этой же сессии.
            }
        }
    }

    alt.log(`[ok] проба записи ${имя}: проверено ${проверено}, `
            + `приняли и не изменились ${молчат.length}`);

    if (молчат.length > 0) {
        alt.log(`     молча: ${молчат.join(', ')}`);
    }
}

// --- Сплошной обход свойств на сервере --------------------------------------
//
// Отвечает не «есть ли имя», а «работает ли то, что есть»: свойство, читающее
// необъявленную переменную, для сверки по именам выглядит существующим, а
// падает при первом обращении.
function обойтиСервер(имя, объект) {
    if (объект === null || объект === undefined) {
        alt.log(`[..] ${имя}: нечего обходить`);
        return;
    }

    const бросили = [];
    const пусто = [];
    let прочитано = 0;

    // Начинаем с самого объекта, а не с его прототипа: аксессоры сущностей
    // ставятся ядром **на экземпляр** (`InstanceTemplate`), и обход по одним
    // прототипам прошёл бы мимо почти всего — у игрока нашлось бы три свойства
    // из полутора сотен.
    for (let слой = объект; слой !== null; слой = Object.getPrototypeOf(слой)) {
        for (const ключ of Object.getOwnPropertyNames(слой)) {
            const опись = Object.getOwnPropertyDescriptor(слой, ключ);

            // Требовать геттера нельзя: аксессоры ядра описываются не им, и
            // проверка «есть ли get» проходила мимо почти всего — у игрока
            // читалось три свойства из полутора сотен. Пропускаем только
            // методы: звать их без разбора значило бы менять сессию обходом.
            if (ключ === 'constructor' || опись === undefined ||
                typeof опись.value === 'function') {
                continue;
            }

            try {
                const значение = объект[ключ];
                прочитано += 1;

                if (значение === undefined) {
                    пусто.push(ключ);
                }
            } catch (беда) {
                if (!/в oxyMP этого ещё нет|у игры|нельзя|не умеет|справочник|назначает|состав/
                        .test(беда.message)) {
                    бросили.push(`${ключ}: ${беда.message.slice(0, 70)}`);
                }
            }
        }
    }

    alt.log(`[ok] обход ${имя}: прочитано ${прочитано}, `
            + `неожиданных отказов ${бросили.length}, пустых ${пусто.length}`);

    for (const строка of бросили) {
        alt.log(`     [!!] ${строка}`);
    }

    if (пусто.length > 0) {
        alt.log(`     пустые: ${пусто.join(', ')}`);
    }
}

alt.on('playerConnect', (player) => {
    setTimeout(() => {
        обойтиСервер('Player', player);

        const машина = new alt.Vehicle('sultan', player.pos, new alt.Vector3(0, 0, 0));
        обойтиСервер('Vehicle', машина);

        const метка = new alt.PointBlip(player.pos);
        обойтиСервер('Blip', метка);

        const кукла = new alt.Ped('a_m_y_business_01', player.pos, 0);
        обойтиСервер('Ped', кукла);

        const предмет = new alt.Object('prop_barrel_02a', player.pos,
                                       new alt.Vector3(0, 0, 0));
        обойтиСервер('Object', предмет);

        // Проба записи — после обхода чтением и по тем же сущностям: сперва
        // выясняем, отвечают ли они, и только потом — слушают ли.
        пробаЗаписи('Player', player);
        пробаЗаписи('Vehicle', машина);
        пробаЗаписи('Blip', метка);
        пробаЗаписи('Ped', кукла);
        пробаЗаписи('Object', предмет);

        setTimeout(() => {
            машина.destroy();
            метка.destroy();
            кукла.destroy();
            предмет.destroy();
        }, 2000);
    }, 100000);
});

// --- Что игроку раздаётся ---------------------------------------------------
alt.on('playerConnect', (player) => {
    setTimeout(() => {
        if (!player.valid) {
            return;
        }

        const рядом = new alt.Vehicle('sultan', player.pos, new alt.Vector3(0, 0, 0));
        const кукла = new alt.Ped('a_m_y_business_01', player.pos, 0);

        setTimeout(() => {
            const роздано = player.streamedEntities;

            alt.log(`[ok] streamedEntities: ${роздано.length} штук`);

            for (const { entity, distance } of роздано.slice(0, 4)) {
                alt.log(`     #${entity.id} ${entity.constructor?.name ?? '?'} `
                        + `на ${distance.toFixed(1)} м`);
            }

            рядом.destroy();
            кукла.destroy();
        }, 4000);
    }, 108000);
});
