// Запуск клиентского ресурса: подстановка модулей alt:V и исполнение точки входа.
//
// Исполняется последним — после alt_enums.js, alt_shared.js и alt_client.js.

'use strict';

(function boot(alt) {
    const Module = require('module');
    const { createRequire } = Module;

    const modules = {
        'alt-client': alt.client,
        'alt-shared': alt.shared,
        'alt': alt.client,

        // Нативы отдельным модулем — так их и тянет всякий клиентский ресурс
        // alt:V: `import * as natives from 'natives'`.
        'natives': alt.natives,
    };

    // Перехват Module._load, а не подмена глобального require, — по той же
    // причине, что и на сервере: глобальный видит одна лишь точка входа, а
    // собранный сборщиком бандл делает require из своего модуля. Подробности —
    // в script-js/js/alt_bootstrap.js и в CLAUDE.md.
    const load = Module._load;

    Module._load = function loadWithAlt(request, parent, isMain) {
        if (Object.hasOwn(modules, request)) {
            return modules[request];
        }

        return load.call(this, request, parent, isMain);
    };

    const entry = process.argv[1];

    globalThis.require = createRequire(entry);

    // Необработанный отказ обещания печатается, но не роняет процесс.
    //
    // На сервере это стоило бы сессии; здесь — самой игры. Node по умолчанию
    // завершает процесс, и один забытый `catch` в чужом ресурсе выкидывал бы
    // игрока в рабочий стол без объяснения.
    process.on('unhandledRejection', (failure) => {
        alt.client.logError('необработанный отказ обещания:', failure);
    });

    process.on('uncaughtException', (failure) => {
        alt.client.logError('необработанное исключение:', failure);
    });

    globalThis.require(entry);
})(globalThis.__oxympAlt);
