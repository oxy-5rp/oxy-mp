// Запуск ресурса: подстановка модулей alt:V и исполнение точки входа.
//
// Исполняется последним — после alt_enums.js, alt_shared.js и alt_server.js, —
// потому что подставлять модули можно только когда они собраны.

'use strict';

(function boot(alt) {
    const Module = require('module');
    const { createRequire } = Module;

    /// Что отдавать вместо поиска на диске.
    ///
    /// `alt` рядом с `alt-server` не для красоты: ресурсы пишут и так и так, а
    /// сборщики вроде webpack оставляют в собранном файле то имя, которое стояло
    /// в исходнике.
    const modules = {
        'alt-server': alt.server,
        'alt-shared': alt.shared,
        'alt': alt.server,
    };

    // Подстановка делается перехватом Module._load, а не подменой глобального
    // require, и это не вкусовщина, а необходимость.
    //
    // Глобальный require видит только точка входа. Всякий файл, загруженный
    // через require, получает от Node свой собственный require, привязанный к
    // его пути, — и наш глобальный ему не виден. Ресурс же почти никогда не
    // состоит из одного файла: даже собранный webpack бандл делает
    // `require('alt-server')` из своего модуля, а не из точки входа. Подмени мы
    // только глобальный — работал бы ровно один файл, а остальные жаловались бы
    // на «Cannot find module 'alt-server'», и причину искали бы долго.
    //
    // Module._load же — единственная дверь, через которую проходят все require
    // без исключения.
    const load = Module._load;

    Module._load = function loadWithAlt(request, parent, isMain) {
        // Object.hasOwn, а не `request in modules`: имя вроде 'constructor'
        // нашлось бы в прототипе и вернуло бы функцию вместо модуля.
        if (Object.hasOwn(modules, request)) {
            return modules[request];
        }

        return load.call(this, request, parent, isMain);
    };

    // Своего require у встроенного окружения нет, и это не упущение Node, а его
    // устройство: require принадлежит модулю, а модуля здесь ещё нет. Строится он
    // от пути к точке входа — и потому ресурс тянет пакеты из своего
    // node_modules, а не из чужого.
    //
    // process.argv[1] задаётся при создании окружения и содержит полный путь к
    // точке входа.
    const entry = process.argv[1];

    globalThis.require = createRequire(entry);

    // Необработанный отказ обещания печатается, но не роняет процесс.
    //
    // По умолчанию Node с некоторых пор завершает процесс — разумно для
    // приложения и гибельно для сервера: один ресурс, забывший `catch` на
    // запросе к базе, унёс бы с собой сессию всех игроков.
    process.on('unhandledRejection', (failure) => {
        alt.server.logError('необработанный отказ обещания:', failure);
    });

    process.on('uncaughtException', (failure) => {
        alt.server.logError('необработанное исключение:', failure);
    });

    globalThis.require(entry);
})(globalThis.__oxympAlt);
