// Образцовый игровой режим oxyMP.
//
// Показывает всё, что умеет скриптовый слой: события сессии, команды чата,
// сущности, машины, погоду. Ничего из этого нет в сервере — сервер сам по себе
// умеет только чат и появление игрока, как голый сервер RAGE MP. Всё остальное
// пишется здесь.
//
// Объект `oxymp` есть всегда, без импорта. Обычный `require` тоже работает и
// ищет пакеты в node_modules рядом с этим ресурсом.

'use strict';

// --- Команды чата -----------------------------------------------------------
//
// Своего понятия «команда» у сервера нет и не будет: что считать командой,
// решает режим. Здесь это строка, начинающаяся с косой черты.

const commands = new Map();

function command(name, help, handler) {
    commands.set(name, { help, handler });
}

command('help', 'этот список', (player) => {
    player.tell('Команды:');

    for (const [name, { help }] of commands) {
        player.tell(`  /${name} — ${help}`);
    }
});

command('who', 'кто в сессии', (player) => {
    const everyone = oxymp.players();

    player.tell(`В сессии ${everyone.length}:`);
    for (const other of everyone) {
        player.tell(`  ${other.name} [${other.id}]`);
    }
});

command('pos', 'где вы стоите', (player) => {
    const { x, y, z } = player.position;
    player.tell(`Вы здесь: ${x.toFixed(1)} ${y.toFixed(1)} ${z.toFixed(1)}`);
});

// Дальше — то, что меняет сессию. Право спрашивается на каждое распоряжение и
// спрашивается заново: признак `admin` ставит сервер по перечню в server.cfg, и
// подделать его скрипт не может.

command('veh', 'завести машину: /veh <хеш модели>', (player, rest) => {
    if (!player.admin) {
        player.tell('Это может только распорядитель сессии.');
        return;
    }

    const model = Number(rest);
    if (!Number.isFinite(model) || model === 0) {
        player.tell('Нужен числовой хеш модели: /veh 3078201489');
        return;
    }

    // Машина ставится перед игроком, а не в него: заведённая в той же точке, она
    // вытолкнула бы его из мира.
    const angle = (player.heading * Math.PI) / 180;
    const where = {
        x: player.position.x - Math.sin(angle) * 5,
        y: player.position.y + Math.cos(angle) * 5,
        z: player.position.z,
    };

    const vehicle = oxymp.createVehicle(model, where, player.heading);

    if (vehicle === null) {
        player.tell('Не вышло: в сессии слишком много машин.');
        return;
    }

    player.tell(`Готово, машина ${vehicle.id}.`);
});

command('heal', 'восстановить здоровье и броню', (player) => {
    if (!player.admin) {
        player.tell('Это может только распорядитель сессии.');
        return;
    }

    player.health = 200;
    player.armour = 100;
    player.tell('Здоровье и броня восстановлены.');
});

command('weather', 'сменить погоду: /weather THUNDER', (player, rest) => {
    if (!player.admin) {
        player.tell('Это может только распорядитель сессии.');
        return;
    }

    if (!oxymp.setWeather(rest)) {
        player.tell('Такой погоды нет. Бывают: EXTRASUNNY, CLEAR, CLOUDS, RAIN, THUNDER, FOGGY, SNOW.');
        return;
    }

    oxymp.broadcast(`Погода сменилась на ${rest}.`);
});

command('time', 'сменить время: /time 21:30', (player, rest) => {
    if (!player.admin) {
        player.tell('Это может только распорядитель сессии.');
        return;
    }

    const [hour, minute] = rest.split(':').map(Number);

    if (!oxymp.setTime(hour, minute)) {
        player.tell('Время задаётся как /time 21:30.');
        return;
    }

    oxymp.broadcast(`Теперь ${rest}.`);
});

// --- События сессии ---------------------------------------------------------

oxymp.on('playerConnect', (player) => {
    oxymp.log(`вошёл ${player.name} [${player.id}]`);

    player.tell('Добро пожаловать. /help — что тут можно.');
});

oxymp.on('playerDisconnect', (player, reason) => {
    // Игрок здесь ещё жив: событие объявляется до уборки, и его имя, положение и
    // машина всё ещё на месте. Это и есть место, где режим сохраняет игрока.
    oxymp.log(`вышел ${player.name} [${player.id}]: ${reason}`);
});

oxymp.on('playerDeath', (player, killer, weapon) => {
    if (killer === null) {
        oxymp.broadcast(`${player.name} погиб.`);
        return;
    }

    oxymp.broadcast(`${killer.name} убил ${player.name} (оружие ${weapon}).`);
});

// Единственное событие, которое можно отменить. Возврат false означает «в общий
// чат это не пускать» — ровно то, что нужно команде.
oxymp.on('playerChat', (player, text) => {
    if (!text.startsWith('/')) {
        return true;
    }

    const space = text.indexOf(' ');
    const name = (space === -1 ? text.slice(1) : text.slice(1, space)).toLowerCase();
    const rest = space === -1 ? '' : text.slice(space + 1).trim();

    const found = commands.get(name);

    if (found === undefined) {
        player.tell(`Нет такой команды: /${name}. /help — список.`);
        return false;
    }

    found.handler(player, rest);
    return false;
});

oxymp.log('образцовый режим поднят');
