// Модуль `natives`: нативы игры по имени.
//
// Ресурс, написанный под alt:V, зовёт их так же, как звал бы там:
//
//     const natives = require('natives');
//     natives.setEntityCoords(натив.playerPedId(), 0, 0, 72, false, false, false, false);
//
// Таблица имён, хешей и подписей лежит рядом (`alt_natives_table.js`) и
// порождена из открытой базы. Здесь — только то, что превращает запись таблицы
// в вызываемую функцию.
//
// Файл исполняется после alt_natives_table.js и до alt_client.js.

'use strict';

(function build(alt) {
    const native = alt.native;
    const table = alt.nativeTable;

    /// Готовые функции, по имени.
    ///
    /// Собираются по требованию, а не все сразу, и это не преждевременная
    /// бережливость. Нативов пять тысяч; режим зовёт из них десятки. Собрать
    /// пять тысяч замыканий при запуске значило бы потратить память и время на
    /// то, к чему никто не обратится, — и потратить их в мгновение входа в
    /// сессию, когда игра и так занята загрузкой.
    const built = new Map();

    function make(name) {
        const entry = table[name];

        if (entry === undefined) {
            return undefined;
        }

        const split = entry.indexOf('|');

        // Хеш шестнадцатеричный и в double не помещается без потери старших
        // разрядов — отсюда BigInt.
        const hash = BigInt('0x' + entry.slice(0, split));
        const signature = entry.slice(split + 1);

        const call = (...args) => native.callNative(hash, signature, ...args);

        built.set(name, call);
        return call;
    }

    /// Сам модуль.
    ///
    /// Посредник, а не обычный объект: он и позволяет собирать функции по
    /// требованию. Ценой одного лишнего шага при первом обращении — дальше
    /// функция лежит в `built` и берётся оттуда.
    const natives = new Proxy({}, {
        get(_target, name) {
            if (typeof name !== 'string') {
                return undefined;
            }

            const ready = built.get(name);
            if (ready !== undefined) {
                return ready;
            }

            const made = make(name);
            if (made !== undefined) {
                return made;
            }

            // Нативы, которых нет в нашей сборке игры, в таблицу не попадают
            // вовсе: выдумывать им хеш нельзя — неверный даёт не строку в
            // журнале, а вылет игры в мгновение вызова. Поэтому здесь пусто, а
            // не заглушка: `natives.чегоНет` — это undefined, и вызов его
            // сломается сразу и понятно.
            return undefined;
        },

        has(_target, name) {
            return typeof name === 'string' && Object.hasOwn(table, name);
        },

        ownKeys() {
            return Object.keys(table);
        },

        getOwnPropertyDescriptor() {
            // Ключи обязаны выглядеть перечислимыми, иначе Object.keys по
            // посреднику бросает.
            return { enumerable: true, configurable: true };
        },
    });

    alt.natives = natives;
})(globalThis.__oxympAlt);
