// Запуск клиентского ресурса: подстановка модулей alt:V и исполнение точки входа.
//
// Исполняется последним — после alt_enums.js, alt_shared.js, alt_natives_table.js,
// alt_natives.js, alt_client_entities.js, alt_client_objects.js и alt_client.js.

'use strict';

(function boot(alt) {
    const Module = require('module');
    const { createRequire } = Module;
    const { pathToFileURL } = require('url');
    const nodePath = require('path');

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

        // Файл ресурса, лежащий в свёртке, собирается здесь же — см. ниже
        // `изСвёрткаТребованием`. Перехваты `Module.registerHooks` до сюда не
        // достают: объявленный ими CommonJS загрузчик всё равно идёт читать файл
        // с диска, а на диске его нет. Проверено набором — до этой двери
        // `require('./сосед')` не находил ничего.
        const свой = изСвёрткаТребованием(request, parent);

        if (свой !== undefined) {
            return свой;
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

    // --- Файлы ресурса: свёрток вместо диска -----------------------------------
    //
    // Ресурс приезжает к игроку одним запечатанным свёртком и на диск не
    // раскладывается никогда — ради этого свёрток и заведён. Значит и модули
    // ресурса взять с диска нельзя: файлов там нет.
    //
    // **Адреса при этом остаются обычными файловыми, и это решение, а не
    // недосмотр.** Своя схема выглядела бы честнее, но сломала бы всё, что чужой
    // ресурс делает со своим путём: `__dirname`, `import.meta.url`,
    // `path.join(__dirname, 'assets')`, счёт `../` от файла к файлу. Здесь же
    // адрес — тот самый путь, по которому файл лежал бы, разложи мы его; не
    // совпадает только одно — на диске его нет, и до диска дело не доходит,
    // потому что перехват отвечает раньше.

    const native = alt.native;

    /// Корень ресурса адресом.
    ///
    /// Косая черта на конце обязательна дважды. Без неё сосед с похожим именем
    /// (`mode-old` рядом с `mode`) считался бы своим, а `pathToFileURL` вдобавок
    /// принял бы корень за файл, и относительные пути от него поехали бы на
    /// каталог выше.
    ///
    /// Берётся корень ресурса, а не каталог точки входа: точка входа лежит
    /// внутри (`client/index.js`), и считать от неё значило бы не найти ничего,
    /// что лежит рядом с ней в соседних каталогах ресурса.
    const корень =
        new URL('./', pathToFileURL(nodePath.join(native.resourcePath, 'корень'))).href;

    /// Что уже спрашивали у свёртка: путь в содержимое либо в `undefined`.
    ///
    /// Память нужна не ради скорости, а ради перебора: имя модуля разрешается
    /// перебором окончаний — `./утилиты` это и `утилиты.js`, и `утилиты/index.js`,
    /// — и без памяти каждая проба расшифровывала бы кусок свёртка заново.
    const спрошенное = new Map();

    function изСвёртка(путь) {
        if (спрошенное.has(путь)) {
            return спрошенное.get(путь);
        }

        const содержимое = native.readResourceFile(путь);

        спрошенное.set(путь, содержимое);

        return содержимое;
    }

    /// Лежит ли адрес внутри ресурса.
    function внутри(url) {
        return typeof url === 'string' && url.startsWith(корень);
    }

    /// Путь от корня ресурса — тот самый, которым файл назван в свёртке.
    function путьОт(url) {
        return decodeURIComponent(url.slice(корень.length));
    }

    /// Чем догадываться, если имя названо без окончания.
    ///
    /// Порядок тот же, что у Node: сперва сам файл, потом окончания, потом
    /// каталог с точкой входа внутри. Модули ES окончаний не досчитывают, но
    /// собранные бандлы приходят и с `require('./chunk')`, и отказать им значило
    /// бы не поднять половину чужих режимов.
    const kОкончания = ['', '.js', '.cjs', '.mjs', '.json'];

    function кандидаты(путь) {
        const голый = путь.endsWith('/') ? путь.slice(0, -1) : путь;

        const список = [];

        for (const окончание of kОкончания) {
            список.push(голый + окончание);
        }

        for (const окончание of kОкончания) {
            if (окончание !== '') {
                список.push(голый + '/index' + окончание);
            }
        }

        return список;
    }

    /// Первый существующий в свёртке. Пусто — нет ни одного.
    function найти(путь) {
        for (const кандидат of кандидаты(путь)) {
            if (изСвёртка(кандидат) !== undefined) {
                return кандидат;
            }
        }

        // Каталог со своим package.json: `main` оттуда — то, чем каталог
        // называет свою точку входа. Так собрано всё, что лежит в node_modules.
        const голый = путь.endsWith('/') ? путь.slice(0, -1) : путь;
        const описание = изСвёртка(голый + '/package.json');

        if (описание === undefined) {
            return undefined;
        }

        try {
            const main = JSON.parse(описание).main;

            if (typeof main === 'string' && main !== '') {
                const внутренний = new URL(main, корень + голый + '/').href;

                if (внутри(внутренний)) {
                    return найти(путьОт(внутренний));
                }
            }
        } catch {
            // Испорченный package.json — не повод отказывать всему ресурсу:
            // перебор просто закончится ничем, и Node пожалуется сам, назвав имя
            // модуля.
        }

        return undefined;
    }

    /// Чем считать файл: модулем ES, CommonJS или данными.
    ///
    /// Окончание сильнее содержимого только у `.json` и `.mjs`: у первого не
    /// бывает другого толкования, у второго оно объявлено. Во всех остальных
    /// случаях смотрим внутрь — по той же причине, по какой смотрим ниже в ответ
    /// Node: чужой бандл зовётся `index.cjs`, а внутри у него `import`.
    function видФайла(путь, исходник) {
        if (путь.endsWith('.json')) {
            return 'json';
        }

        if (путь.endsWith('.mjs')) {
            return 'module';
        }

        return looksLikeModule(исходник) ? 'module' : 'commonjs';
    }

    /// Во что превращается имя модуля, названное изнутри ресурса.
    ///
    /// Пусто — в свёртке такого нет, и разбираться дальше будет сам Node: файл
    /// мог остаться отдельным (модели, звуки), и тогда он лежит на диске.
    function разрешить(specifier, parentURL) {
        // Относительное имя считается от того, кто его назвал.
        if (specifier.startsWith('./') || specifier.startsWith('../')) {
            if (!внутри(parentURL)) {
                return undefined;
            }

            const адрес = new URL(specifier, parentURL).href;

            return внутри(адрес) ? найти(путьОт(адрес)) : undefined;
        }

        // Готовый адрес: так приходит точка входа и так Node переспрашивает уже
        // разрешённое.
        if (внутри(specifier)) {
            return найти(путьОт(specifier));
        }

        // Полный путь Windows: так зовёт `require`, когда имя уже разобрано, — и
        // так же приходит переходник, которым модуль CommonJS отдаётся
        // загрузчику модулей ES.
        if (nodePath.isAbsolute(specifier)) {
            const адрес = pathToFileURL(specifier).href;

            return внутри(адрес) ? найти(путьОт(адрес)) : undefined;
        }

        // Голое имя — пакет. Ищется там же, где его ищет Node, только внутри
        // свёртка: ресурс мог привезти свои зависимости с собой.
        if (внутри(parentURL) && !specifier.startsWith('node:')) {
            return найти('node_modules/' + specifier);
        }

        return undefined;
    }

    // --- Тот же свёрток, но для require ---------------------------------------
    //
    // Загрузчик CommonJS живёт своей жизнью и о перехватах модулей ES не знает:
    // объявив файл своим, он идёт читать его с диска сам. Поэтому модуль
    // собирается здесь целиком — от разбора имени до компиляции, — и Node в этом
    // месте не участвует вовсе.
    //
    // Собранные модули помнятся: `require` одного и того же файла обязан отдавать
    // один и тот же объект, иначе состояние ресурса раздваивается.

    const собранные = new Map();

    /// От кого считать относительное имя. Пусто — спрашивают не из ресурса.
    function откуда(parent) {
        const filename = parent?.filename;

        if (typeof filename !== 'string') {
            return undefined;
        }

        const адрес = pathToFileURL(filename).href;

        return внутри(адрес) ? адрес : undefined;
    }

    /// Собирает модуль ресурса по требованию `require`.
    ///
    /// `undefined` — такого в свёртке нет, и дальше пусть разбирается Node: файл
    /// мог остаться отдельным и лежать на диске.
    function изСвёрткаТребованием(request, parent) {
        const родитель = откуда(parent);

        // Точку входа зовут по полному пути, а не относительно кого-то: у неё
        // родителя нет вовсе.
        const путь = разрешить(request, родитель);

        return путь === undefined ? undefined : собрать(путь, parent);
    }

    /// Исполняет файл ресурса как модуль CommonJS и отдаёт его exports.
    ///
    /// Отдельно от разбора имени, потому что зовут отсюда двое: `require` — уже
    /// разобрав имя, — и загрузчик модулей ES, которому имя разбирать не нужно,
    /// он приходит с готовым адресом.
    function собрать(путь, parent) {
        if (собранные.has(путь)) {
            return собранные.get(путь).exports;
        }

        const исходник = изСвёртка(путь);

        if (исходник === undefined) {
            return undefined;
        }

        const filename = nodePath.join(native.resourcePath, путь);

        const модуль = new Module(filename, parent ?? null);

        модуль.filename = filename;
        модуль.paths = Module._nodeModulePaths(nodePath.dirname(filename));

        // В памяти — до компиляции, а не после. Круговые зависимости этим и
        // держатся: модуль, потянувший сам себя, получит наполовину собранный
        // объект вместо бесконечного спуска. Так делает и сам Node.
        собранные.set(путь, модуль);

        try {
            if (путь.endsWith('.json')) {
                модуль.exports = JSON.parse(исходник);
            } else {
                модуль._compile(исходник, filename);
            }

            модуль.loaded = true;
        } catch (беда) {
            // Развалившийся модуль забывается: иначе следующая попытка получила
            // бы его наполовину собранным и пожаловалась бы не на то.
            собранные.delete(путь);
            throw беда;
        }

        return модуль.exports;
    }

    /// Модули CommonJS, отданные загрузчику модулей ES. Читает их переходник.
    const отданные = Object.create(null);
    alt.__cjs = отданные;

    /// Исходник переходника: он берёт готовый объект и выпускает его наружу.
    ///
    /// Именованные выпуски выписываются по ключам — ровно как у модулей alt:V, —
    /// иначе `import { что-то } from './кусок.js'` не нашло бы ничего, хотя
    /// `require` того же файла отдаёт это самое `что-то`.
    function переходникДля(путь) {
        отданные[путь] = собрать(путь, null);

        const держатель = `globalThis.__oxympAlt.__cjs[${JSON.stringify(путь)}]`;

        let текст = `const модуль = ${держатель};\nexport default модуль;\n`;

        const объект = отданные[путь];

        if (объект !== null && (typeof объект === 'object' || typeof объект === 'function')) {
            for (const ключ of Object.keys(объект)) {
                if (exportable(ключ)) {
                    текст += `export const ${ключ} = модуль[${JSON.stringify(ключ)}];\n`;
                }
            }
        }

        return текст;
    }

    Module.registerHooks({
        resolve(specifier, context, next) {
            if (Object.hasOwn(modules, specifier)) {
                return { url: kScheme + specifier, shortCircuit: true };
            }

            const путь = разрешить(specifier, context?.parentURL);

            if (путь !== undefined) {
                // Адрес — тот, по которому файл лежал бы на диске. Обратно в путь
                // его превращает `путьОт`, и круг сходится.
                return { url: корень + путь, shortCircuit: true };
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

            if (внутри(url)) {
                const путь = путьОт(url);
                const исходник = изСвёртка(путь);

                if (исходник !== undefined) {
                    const вид = видФайла(путь, исходник);

                    // Модуль ES и данные загрузчик берёт исходником — он их
                    // разбирает сам.
                    if (вид !== 'commonjs') {
                        return { format: вид, source: исходник, shortCircuit: true };
                    }

                    // А CommonJS — не берёт. Объявленный своим, он идёт читать
                    // файл с диска, и наш исходник пропадает впустую: на диске
                    // файла нет. Это стоило половины проверок набора, пока не
                    // выяснилось.
                    //
                    // Поэтому файл собирается здесь, нашим же загрузчиком, а
                    // загрузчику ES отдаётся переходник — тем же приёмом, каким
                    // отдаются модули alt:V (см. `sourceFor`). Подмены здесь нет:
                    // `import` и `require` одного файла дают один и тот же
                    // объект, а не два похожих.
                    return { format: 'module', source: переходникДля(путь), shortCircuit: true };
                }

                // Не нашлось в свёртке — значит файл остался отдельным и лежит на
                // диске. Пусть Node возьмёт его оттуда.
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
