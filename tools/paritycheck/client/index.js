// Клиентская половина стенда: события и свойства, спрошенные у живой игры.
//
// Главное здесь — `connectionComplete` и кадровое событие. Ни то ни другое
// однажды не приходило вовсе: `ScriptHost::sessionEvent` был написан и не
// звался ниоткуда, а на кадровом висит весь `everyTick`. Мёртвая дорога
// выглядит точно так же, как живая, и увидеть разницу можно только отсюда.

const alt = require('alt-client');

let frames = 0;
let told = false;

function say(line) {
    alt.log(line);
    alt.emitServer('paritycheck:say', line);
}

say('[клиент] paritycheck поднялся');

// --- Вход и выход -----------------------------------------------------------

alt.on('connectionComplete', () => {
    say('[ok] connectionComplete пришёл');
    say(`     игроков в сессии: ${alt.Player.all.length}, рядом ${alt.Player.streamedIn.length}`);
    say(`     я: ${alt.Player.local.name} (#${alt.Player.local.id})`);
});

alt.on('disconnect', () => alt.log('[ok] disconnect пришёл'));

// --- Кадр -------------------------------------------------------------------

alt.everyTick(() => {
    frames += 1;

    // Один раз, на сотом кадре: раньше сюда не доходило ни одного.
    if (frames === 100 && !told) {
        told = true;
        say('[ok] everyTick работает: сто кадров пришло');
    }

    рисуемТекст();
});

// --- Текст ------------------------------------------------------------------
//
// Три чертежа рядом, и они не для красоты: текст ломается молча. Голая цепочка
// нативов, `alt.Utils.drawText2d` и `TextLabel` — каждый из троих однажды не
// рисовал ничего, и ни один не сказал об этом ни слова.
//
// Ширина в журнале — тот же вопрос числом: ширина пустой строки `0.001`
// означает, что текстовая команда потеряла свою подстроку.

let ширинаНазвана = false;

function рисуемТекст() {
    const natives = alt.natives;

    // 1. Голая цепочка, как её пишет всякий ресурс.
    natives.setTextFont(0);
    natives.setTextScale(0.6, 0.6);
    natives.setTextColour(255, 255, 0, 255);
    natives.setTextOutline();
    natives.beginTextCommandDisplayText('STRING');
    natives.addTextComponentSubstringPlayerName('js natives chain');
    natives.endTextCommandDisplayText(0.05, 0.30, 0);

    // 2. Через слой.
    alt.Utils.drawText2dThisFrame('alt.Utils.drawText2d', { x: 0.05, y: 0.35 }, 0, 0.6,
                                  new alt.RGBA(0, 255, 255, 255), true, false, 0);

    if (ширинаНазвана) {
        return;
    }

    ширинаНазвана = true;

    natives.beginTextCommandWidth('STRING');
    natives.addTextComponentSubstringPlayerName('js natives chain');
    say(`[ok] ширина из JS: ${natives.endTextCommandGetWidth(true)}`);

    // 3. Надпись в точке мира — над собственной головой.
    try {
        const где = alt.Player.local.pos;
        const метка = new alt.TextLabel(
            'TextLabel here', 'chalet london', 8.0, 1.0,
            new alt.Vector3(где.x, где.y, где.z + 1.2), new alt.Vector3(0, 0, 0),
            new alt.RGBA(255, 0, 255, 255), 1.0, new alt.RGBA(0, 0, 0, 255), true, 30.0);

        say(`[ok] TextLabel #${метка.id}: виден=${метка.visible}`
            + ` в дальности=${метка.isStreamedIn}, всего ${alt.TextLabel.count}`);
    } catch (беда) {
        say(`[!!] TextLabel: ${беда.message}`);
    }
}

// --- Появление тел ----------------------------------------------------------

alt.on('worldObjectStreamIn', (entity) =>
    say(`[ok] worldObjectStreamIn #${entity.id} (тело ${entity.scriptID})`));

alt.on('worldObjectStreamOut', (entity) =>
    say(`[ok] worldObjectStreamOut #${entity.id}`));

alt.on('gameEntityCreate', (entity) =>
    alt.log(`[ok] gameEntityCreate #${entity.id}`));

alt.on('gameEntityDestroy', (entity) =>
    alt.log(`[ok] gameEntityDestroy #${entity.id}`));

// --- Машина под собой -------------------------------------------------------

alt.on('enteredVehicle', (vehicle, seat) =>
    say(`[ok] enteredVehicle #${vehicle?.id} место ${seat} (у водителя обязана быть 1)`));

alt.on('leftVehicle', (vehicle, seat) =>
    say(`[ok] leftVehicle #${vehicle?.id} место ${seat}`));

alt.on('changedVehicleSeat', (vehicle, was, now) =>
    say(`[ok] changedVehicleSeat #${vehicle?.id}: ${was} -> ${now}`));

// --- Метаданные -------------------------------------------------------------

alt.on('syncedMetaChange', (entity, key, value) =>
    say(`[ok] syncedMetaChange у #${entity?.id}: ${key} = ${JSON.stringify(value)}`));

alt.on('globalSyncedMetaChange', (key, value) =>
    say(`[ok] globalSyncedMetaChange ${key} = ${JSON.stringify(value)}`));

// --- Нативы, добавленные в этот заход ---------------------------------------

const natives = require('natives');

setTimeout(() => {
    const named = ['setPedHairTint', 'setPedHeadOverlayTint', 'setHeadBlendEyeColor',
                   'getActualScreenResolution', 'setCursorPosition', 'animpostfxPlay',
                   'animpostfxStopAll', 'setArtificialLightsState', 'useParticleFxAsset',
                   'beginTextCommandGetScreenWidthOfDisplayText',
                   'endTextCommandGetScreenWidthOfDisplayText'];

    const missing = named.filter((name) => typeof natives[name] !== 'function');

    if (missing.length === 0) {
        say(`[ok] все ${named.length} новых натива нашлись по имени`);
    } else {
        say(`[!!] не нашлись: ${missing.join(', ')}`);
    }

    // Зовём тот, у которого ответ видно: разрешение экрана.
    try {
        const [, width, height] = natives.getActualScreenResolution(0, 0);
        say(`[ok] getActualScreenResolution ответил ${width}x${height}`);
    } catch (failure) {
        say(`[!!] getActualScreenResolution упал: ${failure.message}`);
    }
}, 4000);

// --- Метки: какие из них доехали до клиента --------------------------------
//
// Список тех, кому метка видна, живёт на сервере и по сети не едет: метка
// просто не уходит тем, кого в нём нет. Проверить это можно только отсюда, и
// не через `alt.Blip.all` — там лежат метки клиентских ресурсов, а не
// присланные сервером, — а спросив саму игру. Считаем до и после: чужих
// значков 402 на карте хватает и без нас.
const kBlipSprite = 402;

function countBlips() {
    let found = 0;

    for (let blip = natives.getFirstBlipInfoId(kBlipSprite); natives.doesBlipExist(blip);
         blip = natives.getNextBlipInfoId(kBlipSprite)) {
        ++found;
    }

    return found;
}

let before = 0;

setTimeout(() => {
    before = countBlips();
    say(`[метки] до: значков ${kBlipSprite} у игры ${before}`);
}, 23000);

setTimeout(() => {
    const after = countBlips();

    say(`[метки] после: ${after} — прибавилось ${after - before}, ` +
        `а поставлено три, из которых одна не для нас`);
}, 30000);

// --- Одежда из дополнений: что надела сама игра ----------------------------
//
// Ответ сервера — его же память о том, что ему велели. Надела ли игра именно
// эту вещь, знает только она: номер за двести пятьдесят пять до расширения поля
// заворачивался, и просивший триста получал сорок четвёртое.
setTimeout(() => {
    const me = alt.Player.local.scriptID;

    say(`[одежда] у игры на торсе ` +
        `${natives.getPedDrawableVariation(me, 11)}`);
}, 32000);

// --- Что добавилось клиенту в этот заход -----------------------------------
setTimeout(() => {
    const me = alt.Player.local;

    // Своя метаданная: у alt:V она живёт на сущности и никуда не уходит.
    me.setMeta('проверка', { было: 1 });

    say(`[клиентское] setMeta/getMeta: ${JSON.stringify(me.getMeta('проверка'))}, ` +
        `hasMeta ${me.hasMeta('проверка')}, ключи ${JSON.stringify(me.getMetaKeys())}`);

    me.deleteMeta('проверка');
    say(`[клиентское] после deleteMeta: hasMeta ${me.hasMeta('проверка')}`);

    // Поиск по номеру игры.
    const found = alt.Player.getByScriptID(me.scriptID);
    say(`[клиентское] getByScriptID нашёл себя: ${found === me || found?.scriptID === me.scriptID}`);
    say(`[клиентское] getByScriptID на чепухе: ${alt.Vehicle.getByScriptID(1) === null}`);

    // Признаки, которых не было.
    say(`[клиентское] isReloading=${me.isReloading}`);

    const car = alt.Vehicle.all[0];
    if (car !== undefined) {
        say(`[клиентское] машина: lockState=${car.lockState}`);

        try {
            car.gear;
            say('[!!] vehicle.gear промолчал вместо отказа');
        } catch (failure) {
            say(`[клиентское] vehicle.gear отказал вслух: ${failure.message}`);
        }
    }

    // Многоугольная зона: считает так же, как серверная.
    const zone = new alt.ColshapePolygon(0, 100, [{ x: 0, y: 0 }, { x: 10, y: 0 },
                                                  { x: 10, y: 10 }, { x: 0, y: 10 }]);

    say(`[клиентское] многоугольник: внутри ${zone.isPointIn({ x: 5, y: 5, z: 50 })}, ` +
        `снаружи ${zone.isPointIn({ x: 50, y: 5, z: 50 })}, ` +
        `по высоте мимо ${zone.isPointIn({ x: 5, y: 5, z: 500 })}, ` +
        `minZ=${zone.minZ} maxZ=${zone.maxZ} углов=${zone.points.length}`);

    zone.destroy();

    // Метка: приоритет и раздел.
    const blip = new alt.PointBlip(me.pos.x, me.pos.y, me.pos.z);
    blip.priority = 7;
    blip.category = 2;
    say(`[клиентское] метка: priority=${blip.priority}, category=${blip.category}`);
    blip.destroy();
}, 34000);

// --- Рождение и смерть объектов слоя на клиенте -----------------------------
let рождено = 0;
let убрано = 0;

alt.on('baseObjectCreate', () => ++рождено);
alt.on('baseObjectRemove', () => ++убрано);

setTimeout(() => say(`[объекты] у клиента рождено ${рождено}, убрано ${убрано}`), 45000);

// --- Свой выстрел -----------------------------------------------------------
//
// Событие приходит на каждую пулю, а не на нажатие спуска. Считаем их, чтобы
// увидеть очередь, а не одно нажатие.
let выстрелов = 0;

alt.on('playerWeaponShoot', (weapon, total, clip) => {
    ++выстрелов;

    if (выстрелов <= 5) {
        say(`[выстрел] ${выстрелов}: оружие ${weapon}, всего ${total}, в обойме ${clip}`);
    }
});

alt.onServer('паритет:стреляй', () => {
    const me = alt.Player.local.scriptID;
    const here = alt.Player.local.pos;

    // В небо: важно, что патроны тратятся, а не куда они летят.
    natives.taskShootAtCoord(me, here.x + 20, here.y, here.z + 5, 3000, 0);
    say('[выстрел] задача стрельбы выдана');

    // Сам счётчик патронов — раз в секунду. Так видно, кто виноват, если
    // события нет: не стреляет игра или не замечает наблюдение.
    let раз = 0;
    const watch = alt.setInterval(() => {
        if (++раз > 6) {
            alt.clearInterval(watch);
            return;
        }

        const weapon = natives.getSelectedPedWeapon(me);
        const [, clip] = natives.getAmmoInClip(me, weapon, 0);

        say(`[выстрел] ${раз}с: оружие ${weapon}, в обойме ${clip}, ` +
            `всего ${natives.getAmmoInPedWeapon(me, weapon)}, ` +
            `стреляет ${natives.isPedShooting(me)}`);
    }, 1000);
});

// --- Стёкла: у игры на них только запись, и проверить можно лишь глазами ----
//
// Прочесть «опущено ли стекло» нечем. Зато видно, что натив не отказал: у
// машины с опущенным стеклом целость его не меняется, а вот сама машина обязана
// остаться на месте.
alt.onServer('паритет:стёкла', (id) => {
    const car = alt.Vehicle.all.find((one) => one.id === id);

    if (car === undefined) {
        say('[стёкла] машины у клиента нет');
        return;
    }

    say(`[стёкла] машина ${id} на месте, стёкла целы: ` +
        `${natives.isVehicleWindowIntact(car.scriptID, 0)}, крыша у игры ` +
        `${natives.getConvertibleRoofState(car.scriptID)}`);
});

// --- Личные метаданные у клиента --------------------------------------------
alt.on('localMetaChange', (key, value, was) =>
    say(`[личное] ${key}: ${JSON.stringify(was)} -> ${JSON.stringify(value)}, ` +
        `читается ${JSON.stringify(alt.getLocalMeta(key))}, ` +
        `ключи ${JSON.stringify(alt.getLocalMetaKeys())}`));

// --- Что игра показывает на самом деле --------------------------------------
//
// Снимок экрана после потери сессии показывает вид сверху на аэропорт с
// облаками. Загрузочный экран мы гасим каждый кадр, значит это не он — надо
// спросить у самой игры, что у неё сейчас открыто.
setInterval(() => {
    const me = alt.Player.local.scriptID;

    say(`[экран] загрузка ${natives.getIsLoadingScreenActive()}, ` +
        `пауза ${natives.isPauseMenuActive()}, ` +
        `играет ${natives.isPlayerPlaying(natives.playerId())}, ` +
        `в сети ${natives.networkIsGameInProgress()}, ` +
        `тело ${me}, где ${JSON.stringify(alt.Player.local.pos)}`);
}, 20000);

// --- Что добавилось метке и событиям клиента --------------------------------
alt.on('anyResourceStart', (имя) => say(`[клиентское] anyResourceStart ${имя}`));
alt.on('resourceStart', (сломан) => say(`[клиентское] resourceStart errored=${сломан}`));

alt.on('metaChange', (кто, ключ, стало, было) =>
    say(`[клиентское] metaChange ${ключ}: ${JSON.stringify(было)} -> ${JSON.stringify(стало)}`));

alt.on('entityEnterColshape', () => say('[клиентское] entityEnterColshape пришёл'));

alt.on('playerWeaponChange', (было, стало) =>
    say(`[клиентское] playerWeaponChange ${было} -> ${стало}`));

setTimeout(() => {
    const метка = new alt.PointBlip(alt.Player.local.pos);

    метка.sprite = 1;
    метка.flashes = true;
    метка.flashTimer = 5000;
    метка.flashInterval = 500;
    метка.bright = true;
    метка.isFriendly = true;
    метка.showCone = true;
    метка.display = 4;
    метка.highDetail = true;
    метка.asMissionCreator = true;
    метка.headingIndicatorVisible = true;
    метка.tickVisible = true;
    метка.shrinked = true;
    метка.number = 7;
    метка.secondaryColor = { r: 200, g: 30, b: 40, a: 255 };
    метка.gxtName = 'BLIP_INFO_ICON';
    метка.pulse();
    метка.fade(128, 300);

    say(`[метка] мигает ${метка.flashes}, таймер ${метка.flashTimer}, ` +
        `яркая ${метка.bright}, дружеская ${метка.isFriendly}, конус ${метка.showCone}, ` +
        `показ ${метка.display}, подробная ${метка.highDetail}, номер ${метка.number}, ` +
        `сжата ${метка.shrinked}, подпись ${JSON.stringify(метка.gxtName)}`);

    try {
        метка.friendIndicatorVisible = true;
        say('[!!] friendIndicatorVisible промолчал вместо отказа');
    } catch (failure) {
        say(`[метка] friendIndicatorVisible отказал вслух: ${failure.message}`);
    }

    метка.destroy();

    // Своя метаданная — заодно проверим, что о ней объявляют.
    alt.Player.local.setMeta('проба', 1);
    alt.Player.local.deleteMeta('проба');
}, 38000);

// --- События вокруг игрока --------------------------------------------------
alt.on('windowResolutionChange', (было, стало) =>
    say(`[клиентское] windowResolutionChange ${было.x}x${было.y} -> ${стало.x}x${стало.y}`));

alt.on('windowFocusChange', (внимание) =>
    say(`[клиентское] windowFocusChange ${внимание}`));

alt.on('playerInteriorChange', (кто, было, стало) =>
    say(`[клиентское] playerInteriorChange ${было} -> ${стало}`));

alt.on('startEnteringVehicle', (машина, место) =>
    say(`[клиентское] startEnteringVehicle в ${машина?.scriptID}, место ${место}`));

alt.on('startLeavingVehicle', (машина, место) =>
    say(`[клиентское] startLeavingVehicle из ${машина?.scriptID}, место ${место}`));

// Сажаем игрока в машину и высаживаем: без этого события посадки не проверить.
alt.onServer('паритет:садись', (id) => {
    const car = alt.Vehicle.all.find((one) => one.id === id);

    if (car === undefined) {
        say('[клиентское] машины для посадки нет');
        return;
    }

    const я = alt.Player.local.scriptID;

    say('[клиентское] лезем в машину');
    natives.taskEnterVehicle(я, car.scriptID, 10000, -1, 2.0, 1, 0);

    setTimeout(() => {
        say('[клиентское] вылезаем');
        natives.taskLeaveVehicle(я, car.scriptID, 0);

    }, 12000);
});

// --- Мелочь, закрывающая целые классы ---------------------------------------
setTimeout(() => {
    const зона = new alt.ColshapeSphere(0, 0, 0, 5);
    const круг = new alt.ColshapeCircle(0, 0, 5);
    const метка = new alt.PointBlip(0, 0, 0);

    say(`[мелочь] зона: тип ${зона.colshapeType} (шар — 0), ` +
        `круг ${круг.colshapeType} (2), только игроки ${зона.playersOnly}`);
    say(`[мелочь] метка: remoteID ${метка.remoteID} (у своей его нет)`);
    say(`[мелочь] я: isSpawned ${alt.Player.local.isSpawned}`);

    зона.destroy();
    круг.destroy();
    метка.destroy();
}, 44000);

// --- Маркеры ----------------------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        const я = alt.Player.local.pos;

        const близкий = new alt.Marker(1, я, new alt.RGBA(124, 198, 255, 140), true, 50);
        const далёкий = new alt.Marker(1, new alt.Vector3(я.x + 900, я.y, я.z),
                                       new alt.RGBA(255, 80, 80, 140), true, 50);
        const всегда = new alt.Marker(1, я, new alt.RGBA(255, 255, 255, 140));

        близкий.scale = new alt.Vector3(2, 2, 1);

        alt.log(`[ok] marker: номера ${близкий.id}/${далёкий.id}/${всегда.id}, `
                + `всего ${alt.Marker.count}`);
        alt.log(`[ok] marker.getByID: ${alt.Marker.getByID(близкий.id) === близкий}`);
        alt.log(`[ok] marker.streamingDistance: ${близкий.streamingDistance} / `
                + `${всегда.streamingDistance}`);
        alt.log(`[ok] marker.isStreamedIn: близкий ${близкий.isStreamedIn}, `
                + `далёкий ${далёкий.isStreamedIn}, без подгрузки ${всегда.isStreamedIn}`);
        alt.log(`[ok] marker.isGlobal ${всегда.isGlobal}, target ${всегда.target}`);
        alt.log(`[ok] marker.scale ${близкий.scale.x},${близкий.scale.y},${близкий.scale.z}`);
    }, 20000);
});

// --- Оружие, лежащее в мире -------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(async () => {
        const я = alt.Player.local.pos;

        // Модель ствола грузится заранее — ровно так это делает всякий ресурс
        // alt:V. Хеш модели у оружия свой, и берётся он у игры по хешу оружия.
        const модель = alt.hash('w_pi_pistol');
        await alt.Utils.requestModel(модель);

        // Доводы в порядке alt:V: хеш, точка, поворот, вид, патроны.
        const ствол = new alt.WeaponObject('weapon_pistol',
                                           new alt.Vector3(я.x + 1, я.y, я.z),
                                           new alt.Vector3(0, 0, 0),
                                           undefined, 30);

        alt.log(`[ok] WeaponObject: id ${ствол.id}, оружейный ${ствол.isWeaponObject}, `
                + `всего ${alt.WeaponObject.count}`);

        ствол.tintIndex = 3;
        alt.log(`[ok] WeaponObject.tintIndex: ${ствол.tintIndex}`);

        ствол.giveComponent('COMPONENT_AT_PI_FLSH');
        alt.log('[ok] WeaponObject.giveComponent: не бросил');

        try {
            ствол.getComponentTintIndex(0);
            alt.log('[!!] getComponentTintIndex промолчал');
        } catch (беда) {
            alt.log(`[ok] getComponentTintIndex отказал вслух: ${беда.message.slice(0, 40)}`);
        }

        setTimeout(() => ствол.destroy(), 3000);
    }, 24000);
});

// --- Ошибка обработчика доходит до режима -----------------------------------
alt.on('resourceError', (беда, файл, строка) => {
    alt.log(`[ok] resourceError: «${беда?.message}», файл ${файл ? 'есть' : 'нет'}, `
            + `строка ${строка}, стек ${typeof беда?.stack}`);
});

alt.on('connectionComplete', () => {
    setTimeout(() => {
        alt.on('нарочноЛомаюсь', () => { нетТакойФункции(); });
        alt.emit('нарочноЛомаюсь');
    }, 28000);
});

// --- Куски мира -------------------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        // Интерьеры GTA V почти все лежат отдельными IPL и без просьбы не
        // грузятся. Подгрузка занимает кадры, поэтому ответ спрашивается позже.
        const кусок = 'coroner_int_on';

        alt.log(`[ok] isIplActive до просьбы: ${alt.isIplActive(кусок)}`);
        alt.requestIpl(кусок);

        setTimeout(() => {
            alt.log(`[ok] isIplActive через секунду: ${alt.isIplActive(кусок)}`);
            alt.removeIpl(кусок);

            setTimeout(() => alt.log(`[ok] isIplActive после снятия: ${alt.isIplActive(кусок)}`),
                       1000);
        }, 1000);
    }, 32000);
});

// --- Состояние игрока из снимка ---------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        const я = alt.Player.local;

        alt.log(`[ok] player из снимка: мёртв ${я.isDead}, целится ${я.isAiming}, `
                + `стреляет ${я.isShooting}, крадётся ${я.isCrouching}, `
                + `скрытно ${я.isStealthy}, в укрытии ${я.isInCover}, `
                + `перезаряжает ${я.isReloading}, на машине ${я.isOnVehicle}`);
        alt.log(`[ok] player скорости: move ${я.moveSpeed.toFixed(2)}, `
                + `forward ${я.forwardSpeed.toFixed(2)}, strafe ${я.strafeSpeed.toFixed(2)}`);
        alt.log(`[ok] player.aimPos: ${я.aimPos.x.toFixed(1)},${я.aimPos.y.toFixed(1)},`
                + `${я.aimPos.z.toFixed(1)}, оружие ${я.currentWeapon}`);
        alt.log(`[ok] хеш беззнаковый: currentWeapon===hash(weapon_unarmed) `
                + `${я.currentWeapon === alt.hash('weapon_unarmed')}, `
                + `model===hash(mp_m_freemode_01) `
                + `${я.model === alt.hash('mp_m_freemode_01')}`);

        try {
            void я.isOnLadder;
            alt.log('[!!] isOnLadder промолчал');
        } catch (беда) {
            alt.log(`[ok] isOnLadder отказал вслух: ${беда.message.slice(0, 40)}`);
        }
    }, 36000);
});

// --- Машина и перечисления --------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        alt.log(`[ok] перечисления: GameFont.Pricedown=${alt.GameFont.Pricedown}, `
                + `TextAlign.Rigth=${alt.TextAlign.Rigth}, `
                + `Locale.Russian=${alt.Locale.Russian}, `
                + `VehicleIndicatorLights.BlinkPermBoth=`
                + `${alt.VehicleIndicatorLights.BlinkPermBoth}, `
                + `StatName.Stamina=${alt.StatName.Stamina}`);

        const машина = alt.Vehicle.all.find((one) => one.valid);

        if (машина === undefined) {
            alt.log('[..] машин рядом нет — проверка машины пропущена');
            return;
        }

        const v = машина.speedVector;

        alt.log(`[ok] vehicle: мест ${машина.seatCount}, бак `
                + `${машина.petrolTankHealth.toFixed(0)}, скорость по осям `
                + `${v.x.toFixed(1)},${v.y.toFixed(1)},${v.z.toFixed(1)}`);

        машина.indicatorLights = alt.VehicleIndicatorLights.BlinkLeft;
        alt.log('[ok] vehicle.indicatorLights записался');

        try {
            void машина.indicatorLights;
            alt.log('[!!] indicatorLights на чтение промолчал');
        } catch (беда) {
            alt.log(`[ok] indicatorLights на чтение отказал вслух`);
        }
    }, 40000);
});

// --- Местный предмет --------------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(async () => {
        const модель = alt.hash('prop_barrel_02a');
        await alt.Utils.requestModel(модель);

        const я = alt.Player.local.pos;
        const бочка = new alt.LocalObject(модель, new alt.Vector3(я.x + 2, я.y, я.z),
                                          new alt.Vector3(0, 0, 0));

        alt.log(`[ok] LocalObject: всего ${alt.LocalObject.count}, мировой `
                + `${бочка.isWorldObject}, дальность ${бочка.lodDistance}`);

        бочка.alpha = 128;
        alt.log(`[ok] LocalObject.alpha: ${бочка.alpha}`);
        бочка.resetAlpha();
        alt.log(`[ok] после resetAlpha: ${бочка.alpha}`);

        бочка.hasGravity = false;
        бочка.positionFrozen = true;
        бочка.toggleCollision(false, true);
        бочка.textureVariation = 1;
        бочка.placeOnGroundProperly();
        alt.log('[ok] LocalObject: тяжесть, заморозка, столкновения, раскраска — не бросили');

        бочка.attachToEntity(alt.Player.local, 0, new alt.Vector3(0, 0, 1),
                             new alt.Vector3(0, 0, 0));
        alt.log('[ok] LocalObject.attachToEntity не бросил');
        бочка.detach();

        await бочка.waitForSpawn();
        alt.log('[ok] LocalObject.waitForSpawn исполнилось');

        for (const имя of ['hasGravity', 'isCollisionEnabled', 'positionFrozen',
                           'textureVariation']) {
            try {
                void бочка[имя];
                alt.log(`[!!] ${имя} на чтение промолчал`);
            } catch (беда) {
                alt.log(`[ok] ${имя} на чтение отказал вслух`);
            }
        }

        setTimeout(() => бочка.destroy(), 3000);
    }, 44000);
});

// --- Своё оружие и выносливость ---------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        const я = alt.Player.local;

        alt.log(`[ok] своё оружие: в обойме ${я.currentAmmo}, всего кулаком `
                + `${я.getWeaponAmmo('weapon_unarmed')}, кулак есть `
                + `${я.hasWeapon('weapon_unarmed')}, пистолет есть `
                + `${я.hasWeapon('weapon_pistol')}`);
        alt.log(`[ok] обвес на кулаке: `
                + `${я.hasWeaponComponent('weapon_unarmed', 'COMPONENT_AT_PI_FLSH')}`);
        alt.log(`[ok] выносливость: ${я.stamina.toFixed(2)}`);

        for (const имя of ['maxStamina', 'currentWeaponData']) {
            try {
                void я[имя];
                alt.log(`[!!] ${имя} промолчал`);
            } catch (беда) {
                alt.log(`[ok] ${имя} отказал вслух`);
            }
        }

        try {
            я.getWeaponComponents('weapon_pistol');
            alt.log('[!!] getWeaponComponents промолчал');
        } catch (беда) {
            alt.log('[ok] getWeaponComponents отказал вслух');
        }
    }, 48000);
});

// --- Оружие после выдачи ----------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        const я = alt.Player.local;

        alt.log(`[ok] после выдачи: пистолет есть ${я.hasWeapon('weapon_pistol')}, `
                + `патронов ${я.getWeaponAmmo('weapon_pistol')}, `
                + `в руках ${я.currentWeapon === alt.hash('weapon_pistol')}, `
                + `в обойме ${я.currentAmmo}`);
    }, 55000);
});

// --- Разбор доводов у двух нативов оружия -----------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        const тело = natives.playerPedId();
        const пистолет = alt.hash('weapon_pistol');

        alt.log(`[..] hasPedGotWeapon(true)=${natives.hasPedGotWeapon(тело, пистолет, true)} `
                + `(false)=${natives.hasPedGotWeapon(тело, пистолет, false)}`);
        alt.log(`[..] getAmmoInClip целиком: `
                + `${JSON.stringify(natives.getAmmoInClip(тело, пистолет, 0))}`);
        alt.log(`[..] getSelectedPedWeapon=${natives.getSelectedPedWeapon(тело) >>> 0} `
                + `пистолет=${пистолет}`);
        alt.log(`[..] isPedArmed=${natives.isPedArmed(тело, 7)} `
                + `maxAmmoInClip=${JSON.stringify(natives.getMaxAmmoInClip(тело, пистолет, true))}`);

        // Достаём оружие в руки и спрашиваем снова: GET_AMMO_IN_CLIP отвечает
        // только про то, что действительно в руках.
        natives.setCurrentPedWeapon(тело, пистолет, true);
        setTimeout(() => {
            alt.log(`[..] после setCurrentPedWeapon: `
                    + `${JSON.stringify(natives.getAmmoInClip(тело, пистолет, 0))}, `
                    + `armed=${natives.isPedArmed(тело, 7)}`);
        }, 1500);
    }, 57000);
});

// --- Надпись в мире ---------------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        const я = alt.Player.local.pos;

        const надпись = new alt.TextLabel(
            'oxyMP: надпись в мире', 'pricedown', 2, 1,
            new alt.Vector3(я.x, я.y, я.z + 1.2),
            new alt.Vector3(0, 0, 0),
            new alt.RGBA(124, 198, 255, 255), 1, new alt.RGBA(0, 0, 0, 255));

        alt.log(`[ok] TextLabel: номер ${надпись.id}, всего ${alt.TextLabel.count}, `
                + `общая ${надпись.isGlobal}, шрифт ${надпись.font}, `
                + `getByID ${alt.TextLabel.getByID(надпись.id) === надпись}`);

        const далёкая = new alt.TextLabel('далеко', 'chaletlondon', 1, 1,
                                          new alt.Vector3(я.x + 900, я.y, я.z),
                                          new alt.Vector3(0, 0, 0),
                                          alt.RGBA.white, 0, alt.RGBA.black, true, 50);

        alt.log(`[ok] TextLabel подгрузка: близкая ${надпись.isStreamedIn}, `
                + `далёкая ${далёкая.isStreamedIn}`);

        setTimeout(() => { надпись.destroy(); далёкая.destroy(); }, 120000);
    }, 60000);
});

// --- alt.Utils --------------------------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(async () => {
        await alt.Utils.requestAnimDict('anim@heists@humane_labs@finale@keycards');
        alt.log('[ok] Utils.requestAnimDict дождался');

        await alt.Utils.requestClipSet('move_m@casual@a');
        alt.log('[ok] Utils.requestClipSet дождался');

        // Текст игра не рисует — проверяем, что вызов не бросает и жалуется.
        alt.Utils.drawText2dThisFrame('проба', { x: 0.5, y: 0.3 });
        alt.log('[ok] Utils.drawText2dThisFrame не бросил');

        const номер = alt.Utils.drawText3d('проба', alt.Player.local.pos);
        alt.log(`[ok] Utils.drawText3d вернул номер ${typeof номер}`);
        alt.clearEveryTick(номер);
    }, 30000);
});

// --- Сплошной обход свойств: что бросает и что молчит ------------------------
//
// Проверки по именам говорят, чего нет. Эта говорит другое: работает ли то, что
// есть. Свойство, читающее необъявленную переменную, для сверки по именам
// выглядит существующим — а падает при первом обращении.
function обойти(имя, объект) {
    if (объект === null || объект === undefined) {
        alt.log(`[..] ${имя}: нечего обходить`);
        return;
    }

    const бросили = [];
    const пусто = [];
    let прочитано = 0;

    // Идём по цепочке прототипов: свойства объявлены на разных её ступенях.
    // С самого объекта, а не с прототипа: часть свойств ставится на экземпляр.
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
                // Объявленный отказ — не поломка: он и должен бросать.
                if (!/в oxyMP этого ещё нет|у игры|нельзя|не умеет|справочник/.test(
                        беда.message)) {
                    бросили.push(`${ключ}: ${беда.message.slice(0, 60)}`);
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

alt.on('connectionComplete', () => {
    setTimeout(() => {
        обойти('LocalPlayer', alt.Player.local);
        обойти('Vehicle', alt.Vehicle.all.find((one) => one.valid));

        const метка = new alt.PointBlip(alt.Player.local.pos);
        обойти('Blip', метка);
        метка.destroy();

        const маркер = new alt.Marker(1, alt.Player.local.pos, alt.RGBA.white);
        обойти('Marker', маркер);
        маркер.destroy();
    }, 34000);
});

// --- Прохожие сессии у клиента ----------------------------------------------
alt.on('connectionComplete', () => {
    setTimeout(() => {
        const все = alt.Ped.all;

        alt.log(`[ok] Ped.all: ${все.length}, с телом ${alt.Ped.streamedIn.length}, `
                + `count ${alt.Ped.count}`);

        if (все.length === 0) {
            alt.log('[..] прохожих сессии нет — сравнивать нечего');
            return;
        }

        const один = все[0];

        alt.log(`[ok] Ped из сессии: номер ${один.id}, дескриптор ${один.scriptID}, `
                + `здоровье ${один.health}, мёртв ${один.isDead}`);
        alt.log(`[ok] Ped.getByID: ${alt.Ped.getByID(один.id) === один}`);
        alt.log(`[ok] Ped.getByScriptID: `
                + `${alt.Ped.getByScriptID(один.scriptID) === один}`);
    }, 76000);
});

// --- Кто чем распоряжается --------------------------------------------------
alt.on('netOwnerChange', (сущность, стал, был) =>
    alt.log(`[ok] netOwnerChange: #${сущность?.id ?? сущность?.sessionId}, `
            + `${был?.name ?? 'никто'} -> ${стал?.name ?? 'никто'}`));

alt.on('connectionComplete', () => {
    setTimeout(() => {
        const я = alt.Player.local;

        alt.log(`[ok] netOwner своего игрока: ${я.netOwner?.name ?? 'никто'}, `
                + `сам себе ${я.netOwner === я}`);


        const машина = alt.Vehicle.all.find((one) => one.valid);

        if (машина !== undefined) {
            alt.log(`[ok] netOwner машины: ${машина.netOwner?.name ?? 'никто'}`);
        }

        const прохожий = alt.Ped.all[0];

        if (прохожий !== undefined) {
            alt.log(`[ok] netOwner прохожего: ${прохожий.netOwner?.name ?? 'никто'} `
                    + '(ждём «никто» — им распоряжается сервер)');
        }
    }, 84000);
});

// --- Свои общие метаданные клиента ------------------------------------------
alt.on('globalMetaChange', (ключ, стало, было) =>
    alt.log(`[ok] globalMetaChange: ${ключ}: ${JSON.stringify(было)} -> `
            + `${JSON.stringify(стало)}`));

alt.on('connectionComplete', () => {
    setTimeout(() => {
        alt.setMeta('своё', { счёт: 3 });
        alt.setMeta({ первое: 1, второе: 2 });

        alt.log(`[ok] alt.getMeta: ${JSON.stringify(alt.getMeta('своё'))}, есть `
                + `${alt.hasMeta('своё')}, ключи ${JSON.stringify(alt.getMetaKeys().sort())}`);

        alt.deleteMeta('своё');
        alt.log(`[ok] после deleteMeta: есть ${alt.hasMeta('своё')}`);
    }, 88000);
});
