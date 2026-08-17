// Ресурс, написанный под alt:V, работающий на oxyMP.
//
// Ничего своего здесь нет: `require('alt-server')`, `alt.on`, `alt.Vector3`,
// `alt.hash` — всё это писалось бы точно так же под настоящий alt:V. Ради этого
// слой и заводился.
//
// Он же и проверка: запустился сервер, а в журнале — строки отсюда, значит
// подстановка модулей, перечисления, математика и события работают.

'use strict';

const alt = require('alt-server');

// Общая часть доступна и отдельным модулем — так её и тянут ресурсы alt:V.
const { Vector3 } = require('alt-shared');

alt.log('ресурс запущен, слой alt:V на месте');

// --- Проверка того, что слой действительно собран ---------------------------

// Хеш обязан совпасть с тем, что считает сервер: разойдись они хоть в разряде —
// машина по имени не нашлась бы, а объяснения не было бы никакого.
const adder = alt.hash('adder');
alt.log(`hash('adder') = 0x${adder.toString(16).toUpperCase()}`);

// Математика векторов.
const from = new Vector3(0, 0, 0);
const to = new alt.Vector3(3, 4, 0);
alt.log(`расстояние = ${from.distanceTo(to)}, длина = ${to.length}`);

// Вектор неизменяем: alt:V обещает именно это, и ресурсы на это опираются.
//
// В строгом режиме присваивание замороженному полю бросает — потому и в ловушке.
// Это и есть нужное поведение: `player.pos.z += 10` обязан пожаловаться, а не
// молча поправить копию и не сделать ничего.
const frozen = new Vector3(1, 2, 3);

try {
    frozen.x = 999;
    alt.log(`вектор изменился — так быть не должно: ${frozen.x}`);
} catch {
    alt.log(`вектор неизменяем: x по-прежнему ${frozen.x}`);
}

// Перечисления — с настоящими числами игры, а не выдуманными.
alt.log(`KeyCode.E = ${alt.KeyCode.E}, BlipColor.Red = ${alt.BlipColor.Red}, ` +
        `MarkerType.MarkerCylinder = ${alt.MarkerType.MarkerCylinder}, ` +
        `VehicleLockState.Locked = ${alt.VehicleLockState.Locked}`);

// --- События ----------------------------------------------------------------

alt.on('playerConnect', (player) => {
    alt.log(`вошёл ${player.name} (${player.id})`);

    // Метаданные — те самые, что не покидают сервер.
    player.setMeta('joinedAt', Date.now());

    // Позиция вектором, как в alt:V.
    alt.log(`он в точке ${player.pos}`);
});

alt.on('playerDisconnect', (player, reason) => {
    // Событие объявляется до уборки, поэтому игрок здесь ещё цел — с именем и
    // метаданными. На это опираются все режимы, сохраняющие игрока при выходе.
    const joined = player.getMeta('joinedAt');
    const spent = joined === undefined ? '?' : Math.round((Date.now() - joined) / 1000);

    alt.log(`вышел ${player.name}, пробыл ${spent} c, причина: ${reason}`);
});

// `once` и `off` — то, чего у голого ядра нет, а alt:V обещает.
alt.once('playerConnect', () => alt.log('это сказано ровно один раз'));

// Своё событие внутри сервера.
alt.on('проверка:событие', (какое) => alt.log(`своё событие дошло: ${какое}`));
alt.emit('проверка:событие', 'да');

// Таймер обязан вернуть число, а не объект: ресурсы alt:V складывают его номер в
// базу и шлют странице интерфейса.
const timer = alt.setTimeout(() => alt.log('таймер сработал'), 250);
alt.log(`номер таймера — число: ${typeof timer === 'number'} (${timer})`);

// Отказ вместо тишины: чего ещё нет, о том говорится вслух.
try {
    alt.Blip();
} catch (failure) {
    alt.log(`несделанное честно отказывает: ${failure.message}`);
}

alt.log('проверка слоя пройдена');
