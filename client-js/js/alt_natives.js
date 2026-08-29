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

    /// О чём уже жаловались. По имени, а не по обращению: за одно имя в
    /// кадровом обработчике набегает тысяча обращений в минуту.
    const сказано = new Set();

    function пожаловатьсяОдинРаз(name) {
        // Имена, начинающиеся не с буквы, — это внутренние вопросы движка к
        // объекту (`then`, `Symbol.toPrimitive` и прочее), а не нативы.
        if (сказано.has(name) || !/^[a-z_]/.test(name)) {
            return;
        }

        сказано.add(name);

        // Через `native`, а не через `alt.client`: этот файл исполняется раньше
        // `alt_client.js`, и полагаться на то, что тот уже собран, нельзя.
        native.logWarning(
            `натив «${name}» нашей таблице неизвестен: его нет в открытой базе имён, ` +
            'а выдумывать ему хеш нельзя — неверный роняет игру');
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

            // Нативы, которых нет в нашей таблице, отдаются пустотой, а не
            // заглушкой, и это не лень: выдумывать хеш нельзя — неверный даёт не
            // строку в журнале, а вылет игры в мгновение вызова. Пустота же
            // сломается сразу.
            //
            // Но одной пустоты мало, и это выяснил живой чужой режим: «native.X
            // is not a function» не говорит, чья тут вина — опечатка режима или
            // наша нехватка. Поэтому причина называется вслух, один раз на имя.
            //
            // Нехватка эта большая и известная: у открытой базы CitizenFX 5180
            // имён, у alt:V около восьми с половиной тысяч. Подробности и что для
            // её закрытия нужно — в `CLAUDE.md`.
            //
            // **Слышно это только тому, кто зовёт через `require`.** Запись
            // `import * as natives from 'natives'` — а её и пишут чаще — снимает
            // с модуля пространство имён по `ownKeys`, и до посредника дело
            // больше не доходит: недостающего имени в снимке просто нет, и
            // обращение к нему пусто без всякого нашего участия. Проверено живым
            // режимом: он зовёт `import * as`, и предупреждения не увидел ни
            // разу, хотя натива ему не хватало тысячу раз за минуту.
            пожаловатьсяОдинРаз(name);
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
