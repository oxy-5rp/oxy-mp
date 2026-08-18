// Серверная половина показательного режима.
//
// Написана так, как её написали бы под alt:V: ни одной строки, которую пришлось
// бы менять при переносе туда.

'use strict';

const alt = require('alt-server');

alt.log('показательный режим поднят');

alt.on('playerConnect', (player) => {
    alt.log(`вошёл ${player.name}`);

    // Метаданные игрока — из тех, что видит и клиент.
    player.setSyncedMeta('вошёл', new Date().toLocaleTimeString('ru-RU'));

    // Приветствие уходит клиентской половине, а та показывает его страницей.
    alt.emitClient(player, 'демо:привет', player.name, alt.hash('adder'));
});

// Клиент рассказывает, что у него получилось, — сервер это записывает.
alt.onClient('демо:доклад', (player, строки) => {
    for (const строка of строки) {
        alt.log(`[клиент ${player.name}] ${строка}`);
    }
});

// Метаданные сессии: их видит и клиент.
alt.setSyncedMeta('погода', 'ясно');
alt.setSyncedMeta('режим', 'показательный');

alt.onClient('демо:нажали', (player, что) => {
    alt.log(`${player.name} нажал на странице: ${что}`);
    player.tell(`страница ответила серверу: ${что}`);
});
