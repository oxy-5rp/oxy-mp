// Запуск клиентского ресурса: подстановка модулей alt:V и исполнение точки входа.
//
// Исполняется последним — после alt_enums.js, alt_shared.js, alt_natives_table.js,
// alt_natives.js, alt_client_entities.js, alt_client_objects.js и alt_client.js.

'use strict';

(function boot(alt) {
    const Module = require('module');
    const { createRequire } = Module;
    const { pathToFileURL } = require('url');

    const modules = {
        'alt-client': alt.client,
        'alt-shared': alt.shared,
        'alt': alt.client,

        // Нативы отдельным модулем — так их и тянет всякий клиентский ресурс
        // alt:V: `import * as natives from 'natives'`.
        'natives': alt.natives,
    };

    // Модули подставляются дважды, и это не перестраховка.
    //
    // В JavaScript два разных загрузчика, и они не знают друг о друге. Ресурс,
    // написанный под alt:V, может прийти и тем и другим: клиентская половина
    // почти всегда модуль ES (`import * as alt from 'alt-client'`), серверная
    // чаще собрана в CommonJS (`require('alt-server')`). Перехвати мы только
    // один — половина чужих режимов не поднялась бы вовсе, и жалоба была бы
    // невнятной: «Cannot use import statement outside a module».

    // --- CommonJS -------------------------------------------------------------
    //
    // Module._load — единственная дверь, через которую проходят все require без
    // исключения. Глобального require хватило бы ровно на точку входа: всякий
    // файл, загруженный через require, получает от Node свой собственный.
    const load = Module._load;

    Module._load = function loadWithAlt(request, parent, isMain) {
        if (Object.hasOwn(modules, request)) {
            return modules[request];
        }

        return load.call(this, request, parent, isMain);
    };

    // --- Модули ES ------------------------------------------------------------
    //
    // Здесь дверь другая: загрузчик модулей спрашивает сперва «во что
    // превращается это имя» (resolve), потом «что лежит по этому адресу» (load).
    // Перехватываются оба.

    /// Своя схема адреса. Настоящего файла за ней нет и быть не должно.
    const kScheme = 'oxymp-module:';

    /// Можно ли выпустить это имя как отдельный экспорт.
    ///
    /// Имя экспорта — это имя переменной, и не всякий ключ объекта им годится:
    /// ключ может оказаться словом языка (`default`, `class`), начинаться с
    /// цифры или содержать что угодно. Такие пропускаются: они остаются
    /// доступны через экспорт по умолчанию.
    const kReserved = new Set([
        'default', 'class', 'const', 'let', 'var', 'function', 'return', 'new',
        'delete', 'typeof', 'in', 'of', 'do', 'if', 'else', 'switch', 'case',
        'break', 'continue', 'for', 'while', 'try', 'catch', 'finally', 'throw',
        'this', 'super', 'null', 'true', 'false', 'void', 'with', 'yield',
        'await', 'import', 'export', 'extends', 'enum', 'static',
    ]);

    function exportable(name) {
        return typeof name === 'string' && !kReserved.has(name) &&
               /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name);
    }

    /// Собирает исходник модуля-переходника.
    ///
    /// Переходник, а не сам объект, потому что отдать загрузчику готовый объект
    /// нельзя: он принимает исходный текст. Текст этот берёт значения из
    /// глобального хранилища — то есть из того же самого объекта, — и потому
    /// подмены здесь нет: `import { Vector3 }` и `require('alt-shared').Vector3`
    /// дают один и тот же Vector3, а не два похожих.
    function sourceFor(name) {
        const holder = `globalThis.__oxympAlt.__modules[${JSON.stringify(name)}]`;

        let text = `const модуль = ${holder};\nexport default модуль;\n`;

        // Object.keys по посреднику нативов отдаёт все пять тысяч имён, и это
        // именно то, что нужно: `import { getGameTimer } from 'natives'`
        // обязан работать так же, как в alt:V.
        for (const key of Object.keys(modules[name])) {
            if (exportable(key)) {
                text += `export const ${key} = модуль[${JSON.stringify(key)}];\n`;
            }
        }

        return text;
    }


    /// Похоже ли содержимое на модуль ES.
    ///
    /// Node решает это по расширению файла и по `"type"` в package.json. alt:V —
    /// не решает вовсе: клиентская половина у него всегда модуль. Отсюда
    /// расхождение, на которое напарывается всякий перенесённый режим: бандл
    /// зовётся `index.cjs`, а внутри у него `import * as alt from 'alt-client'`.
    /// Node глядит на расширение, объявляет файл CommonJS и жалуется «Cannot use
    /// import statement outside a module».
    ///
    /// Поэтому смотрим в сам файл. Проверка нарочно грубая — начало строки, а не
    /// разбор языка: `import` внутри строкового литерала встречается, но не в
    /// начале строки и без пробела следом, а разбирать ради этого весь файл
    /// дороже, чем стоит ошибка.
    function looksLikeModule(source) {
        return /^\s*(?:import|export)[\s{*]/m.test(source);
    }

    alt.__modules = modules;

    Module.registerHooks({
        resolve(specifier, context, next) {
            if (Object.hasOwn(modules, specifier)) {
                return { url: kScheme + specifier, shortCircuit: true };
            }

            return next(specifier, context);
        },

        load(url, context, next) {
            if (url.startsWith(kScheme)) {
                return {
                    format: 'module',
                    source: sourceFor(url.slice(kScheme.length)),
                    shortCircuit: true,
                };
            }

            const answer = next(url, context);

            // Node объявил файл CommonJS, а внутри у него модуль — поправляем.
            //
            // Молчать нельзя: без этого чужой бандл не поднимается вовсе, а
            // жалоба указывает на первую строку файла и ничего не объясняет.
            if (answer?.format === 'commonjs' && answer.source !== undefined) {
                const source = String(answer.source);

                if (looksLikeModule(source)) {
                    return { format: 'module', source, shortCircuit: true };
                }
            }

            return answer;
        },
    });

    // --- Запуск ---------------------------------------------------------------

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

    // Точка входа исполняется через import(), а не require, и это не
    // предпочтение стиля. Загрузчик модулей ES умеет и то и другое: модуль он
    // разберёт как модуль, а файл CommonJS — как CommonJS. Обратное неверно:
    // require спотыкается о первое же `import` и жалуется невнятно.
    //
    // Цена — асинхронность: import() возвращает обещание, и до его разрешения
    // ресурс ещё не поднят. Отсюда признак ниже: по нему сторона на C++ узнаёт,
    // чем всё кончилось, прокрутив цикл событий нужное число раз.
    const запуск = { готов: false, ошибка: null };
    globalThis.__oxympStart = запуск;

    import(pathToFileURL(entry).href)
        .then(() => {
            запуск.готов = true;
        })
        .catch((failure) => {
            запуск.ошибка = failure;
            запуск.готов = true;
        });
})(globalThis.__oxympAlt);
