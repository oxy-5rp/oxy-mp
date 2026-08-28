// Серверная часть API alt:V поверх ядра oxyMP.
//
// Здесь живёт переходник между двумя моделями мира, и он неизбежен. У alt:V
// сущность — объект с временем жизни, метаданными и измерением; у oxyMP — номер,
// разрешаемый заново на каждое обращение (см. docs/scripting.md). Ни ту ни
// другую модель нельзя объявить неправильной: первая удобнее скрипту, вторая не
// даёт скрипту подержать у себя машину, которой уже нет. Переходник берёт
// удобство первой, не отдавая безопасности второй: объект alt:V здесь — тонкая
// обёртка над номером, и всякое обращение к полю уходит в ядро.
//
// Файл исполняется после alt_shared.js и складывает готовое в __oxympAlt.server.

'use strict';

(function build(alt) {
    const native = alt.native;
    const shared = alt.shared;

    function absent(what) {
        return function () {
            throw new Error(`${what}: в oxyMP этого ещё нет`);
        };
    }

    /// О чём уже говорили. По поводу, а не по вызову: то, что зовут на каждом
    /// входе, залило бы журнал одинаковыми строками.
    const warned = new Set();

    /// Говорит о недоделанном один раз — и не бросает.
    ///
    /// Разница с `absent` не в громкости, а в последствиях. `absent` бросает, и
    /// это верно для того, чего нет вовсе: ресурс узнаёт сразу и на месте.
    /// Но для того, что исполняется наполовину и стоит посреди чужого
    /// обработчика, бросок уносит с собой всё, что шло следом, — и вместо одной
    /// незакрытой мелочи ресурс теряет половину своего входа.
    /// Распоряжение, которого мы не умеем исполнить.
    ///
    /// Не бросает — говорит один раз и возвращается. Разница с `absent` не в
    /// громкости, а в цене: распоряжение стоит посреди чужого обработчика, и
    /// брошенное отсюда исключение уносит с собой всё, что шло следом. Так уже
    /// было дважды: `player.model` унёс показ интерфейса при входе, а
    /// `vehicle.repair` — работу починки машин целиком.
    ///
    /// Вопросы этим не покрываются и покрываться не должны: у вопроса без
    /// ответа тишина — это ложь, и `absent` для них остаётся.
    function unperformed(what, why) {
        return function () {
            warnOnce(what, why);
        };
    }

    function warnOnce(what, why) {
        if (warned.has(what)) {
            return;
        }

        warned.add(what);
        native.logWarning(`${what}: ${why}`);
    }

    // --- Ресурсы -------------------------------------------------------------

    /// Поднятый ресурс, каким его видит скрипт.
    ///
    /// Назван ScriptResource, а не Resource: наружу он уходит именем
    /// `alt.Resource`, но внутри этого файла `Resource` уже занят классом
    /// движка, и одноимённые встречались бы в одном месте.
    class ScriptResource {
        constructor(name, path) {
            this.name = name;
            this.path = path;

            // Тип у нас один: движок здесь только для JavaScript. Врать про
            // остальные незачем — их нет.
            this.type = 'js';
            this.isStarted = true;
        }

        /// Настройки ресурса из его resource.toml.
        ///
        /// Пустой объект, и это честнее отказа: у alt:V сюда попадает то, что
        /// хозяин дописал в описание сверх обязательного, и обычно там пусто.
        /// Отказ же бросал бы посреди чужого обработчика на ровном месте.
        get config() {
            return {};
        }

        /// То, чем ресурсы alt:V делятся друг с другом.
        ///
        /// Отказывает вслух, и это не пробел, а следствие устройства. У alt:V
        /// все ресурсы живут в одном изоляте, и функция одного в другом
        /// работает; здесь у каждого свой — уронивший свою кучу не должен
        /// уносить чужие, — и значение из одного изолята в другом не живёт
        /// вовсе. Отдать копию значило бы отдать не то, что просили: изменения
        /// в неё не вернутся, а функции в ней не будет.
        ///
        /// Чем это заменяется: `alt.emit` — его слышат все поднятые ресурсы, и
        /// доводы через него ходят по-настоящему.
        get exports() {
            throw new Error('resource.exports: у каждого ресурса свой изолят, ' +
                            'и значение одного в чужом не живёт — пользуйтесь alt.emit');
        }

        static get current() {
            return new ScriptResource(native.resourceName, native.resourcePath);
        }

        static get all() {
            return native.resources().map((each) => new ScriptResource(each.name, each.path));
        }

        static get(name) {
            return ScriptResource.all.find((each) => each.name === String(name)) ?? null;
        }

        static exists(name) {
            return ScriptResource.get(name) !== null;
        }
    }

    // --- События -------------------------------------------------------------

    /// Подписки, заведённые ресурсом.
    ///
    /// Свой список, а не только родной для ядра, и причина не в удобстве. Ядро
    /// умеет одно — «позвать всех, кто подписан на это имя»; alt:V же обещает
    /// `off`, `once` и перечисление подписчиков. Держа список у себя, мы получаем
    /// всё три даром, а ядру отдаём один мостик на имя — и оно остаётся простым.
    const listeners = new Map();

    /// Имена, по которым мостик в ядро уже поставлен.
    const bridged = new Set();

    function listenersFor(name) {
        let found = listeners.get(name);

        if (found === undefined) {
            found = [];
            listeners.set(name, found);
        }

        return found;
    }

    /// Зовёт подписчиков имени. Возвращает false, если хоть один отменил событие.
    function fire(name, args) {
        // Копия списка нарочно: обработчик волен отписаться прямо отсюда, и
        // перебор по живому списку пропустил бы следующего за ним.
        const called = listenersFor(name).slice();
        let proceed = true;

        for (const entry of called) {
            if (entry.once) {
                off(name, entry.handler);
            }

            let outcome;

            try {
                outcome = entry.handler(...args);
            } catch (failure) {
                // Упавший обработчик не останавливает ни остальных, ни сервер:
                // одна ошибка в одном режиме не повод обрывать сессию всем. Так же
                // поступает и ядро (см. Resource::call).
                logError(`ошибка в обработчике «${name}»:`, failure?.stack ?? failure);
                continue;
            }

            // Отмена считается только явным false: обработчик, ничего не
            // вернувший, отменять не собирался.
            if (outcome === false) {
                proceed = false;
            } else if (name === 'weaponDamage' && typeof outcome === 'number') {
                // У alt:V число, возвращённое отсюда, заменяет урон. У нас — нет:
                // ядро возвращает признак, а не число. Промолчать было бы хуже
                // всего — режим, правящий урон, решил бы, что правит, а урон шёл
                // бы прежний, и найти это было бы не по чему.
                warnOnce('weaponDamage',
                         'число, возвращённое обработчиком, урон не меняет: ' +
                         'у oxyMP отсюда можно только отменить попадание (return false)');
            }
        }

        return proceed;
    }

    /// Ставит мостик в ядро, если его ещё нет.
    ///
    /// Один на имя: ядро зовёт мостик, мостик зовёт подписчиков. Ставить по
    /// мостику на подписчика значило бы терять возможность отписаться — снять
    /// подписку у ядра нельзя.
    function bridge(name, coreName) {
        if (bridged.has(coreName ?? name)) {
            return;
        }

        bridged.add(coreName ?? name);

        if (coreName === undefined) {
            // Мостиков два, и они разведены по именам, а не по форме доводов.
            //
            // Через одно имя приходят двое: события самой сессии — их ядро зовёт
            // с готовыми доводами — и события, объявленные ресурсами, у которых
            // довод один, строкой JSON.
            //
            // **Прежде их различали счётом доводов, и это сломалось на первом
            // же событии сессии с одним строковым доводом.**
            // `consoleCommand('stop')` без доводов слой принял за объявление
            // ресурса, попробовал разобрать «stop» как JSON и не смог —
            // обработчику пришло имя `undefined`. Команда с доводами при этом
            // работала: у неё доводов было больше одного, и потому беда сидела
            // ровно в половине случаев.
            //
            // Теперь объявленное ресурсом приезжает под приставкой `local:`
            // (см. `NodeEngine::announce`): прийти из ядра она не может, а ядро
            // не может прислать её. Гадать больше не о чем.
            native.on(name, (...args) => fire(name, args));
            native.on(`local:${name}`, (payload) => fire(name, decodeLocal(payload)));
        } else {
            // Событие от клиента приходит своим путём: первым доводом игрок.
            native.onClient(coreName, (player, payload) =>
                fire(name, [player, ...decodeArgs(payload)]));
        }
    }

    function on(name, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.on ждёт имя события и обработчик');
        }

        listenersFor(name).push({ handler, once: false });
        bridge(name);
    }

    function once(name, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.once ждёт имя события и обработчик');
        }

        listenersFor(name).push({ handler, once: true });
        bridge(name);
    }

    function off(name, handler) {
        const found = listenersFor(name);
        const at = found.findIndex((entry) => entry.handler === handler);

        if (at >= 0) {
            found.splice(at, 1);
        }
    }

    /// Укладывает доводы события между ресурсами.
    ///
    /// Уложить их приходится, и обойти это нельзя: у каждого ресурса свой
    /// изолят, и значение одного в чужом не живёт вовсе — там его попросту нет
    /// в памяти. Строка же переживает границу.
    ///
    /// Сущности переживают её тоже, и в этом отличие от событий клиенту.
    /// Там сущность запрещена: клиент — чужая сторона, и `Player` у него свой.
    /// Здесь же обе стороны — сервер, номер сущности означает у них одно и то
    /// же, и терять её было бы обидно: `alt.emit('банк:ограблен', player)` —
    /// то, как это пишут.
    function encodeLocal(args) {
        return JSON.stringify(args, (key, value) => {
            if (value instanceof Player) {
                return { __entity: 'player', id: value.id };
            }

            if (value instanceof Vehicle) {
                return { __entity: 'vehicle', id: value.id };
            }

            return value;
        });
    }

    function decodeLocal(payload) {
        if (typeof payload !== 'string' || payload.length === 0) {
            return [];
        }

        let parsed;

        try {
            parsed = JSON.parse(payload, (key, value) => {
                if (value === null || typeof value !== 'object') {
                    return value;
                }

                if (value.__entity === 'player') {
                    return Player.getByID(value.id);
                }

                if (value.__entity === 'vehicle') {
                    return Vehicle.getByID(value.id);
                }

                return value;
            });
        } catch (failure) {
            logError('доводы события между ресурсами не разобрались:', failure?.message);
            return [];
        }

        return Array.isArray(parsed) ? parsed : [parsed];
    }

    /// Событие внутри сервера. Сеть не задействована.
    ///
    /// Слышат его **все поднятые ресурсы**, как и у alt:V, — включая тот,
    /// который его объявил, если он на это имя подписан. Разносит движок: сам
    /// ресурс про остальные не знает и знать не должен.
    ///
    /// Обратно к себе оно приходит тем же мостиком, что и события сессии, а не
    /// прямым вызовом. Разница видна в одном случае, и он не выдуманный: два
    /// ресурса, подписанные на одно имя, обязаны получить событие в одном и том
    /// же порядке независимо от того, кто из них его объявил.
    function emit(name, ...args) {
        native.emit(name, encodeLocal(args));
    }

    // --- События с клиентом --------------------------------------------------

    /// Доводы события укладываются в JSON, а не в MValue.
    ///
    /// Временно и намеренно. MValue в протоколе уже есть (shared/script/mvalue),
    /// но клиентской части alt:V у oxyMP пока нет вовсе, и класть доводы в него
    /// сейчас было бы укладкой в никуда. JSON же читается тем самым слоем
    /// интерфейса, который у клиента есть уже сегодня, — значит ресурс,
    /// говорящий со своей страницей, работает без обмана прямо сейчас.
    ///
    /// Что теряется по сравнению с MValue: сущности не переживают укладку
    /// (Player превратился бы в пустой объект), и потому запрещены явно —
    /// молчаливая потеря игрока в доводах искалась бы днями.
    function encodeArgs(args) {
        return JSON.stringify(args, (key, value) => {
            if (value instanceof Player || value instanceof Vehicle) {
                throw new TypeError(
                    'сущность нельзя передать доводом события клиенту: ' +
                    'пошлите её номер (entity.id) и разрешите его на месте');
            }

            return value;
        });
    }

    function decodeArgs(payload) {
        if (typeof payload !== 'string' || payload.length === 0) {
            return [];
        }

        try {
            const parsed = JSON.parse(payload);
            return Array.isArray(parsed) ? parsed : [parsed];
        } catch {
            // Разобрать не вышло — отдаём как есть. Клиент мог послать простую
            // строку, и терять её из-за того, что она не JSON, незачем.
            return [payload];
        }
    }

    function onClient(name, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.onClient ждёт имя события и обработчик');
        }

        const local = `client:${name}`;

        listenersFor(local).push({ handler, once: false });
        bridge(local, name);
    }

    function offClient(name, handler) {
        off(`client:${name}`, handler);
    }

    function emitClient(target, name, ...args) {
        // Пустой цели alt:V придаёт смысл «всем», и ресурсы этим пользуются.
        if (target === null || target === undefined) {
            return emitAllClients(name, ...args);
        }

        const payload = encodeArgs(args);

        if (Array.isArray(target)) {
            for (const each of target) {
                native.emitClient(each, name, payload);
            }
            return;
        }

        native.emitClient(target, name, payload);
    }

    function emitAllClients(name, ...args) {
        const payload = encodeArgs(args);

        for (const player of native.players()) {
            native.emitClient(player, name, payload);
        }
    }

    // --- Вызовы с ответом (RPC) ----------------------------------------------
    //
    // Событие уходит и забывается; вызов уходит и ждёт ответа. Разница для
    // игрового режима существенная: «покажи окно» — событие, а «что игрок выбрал
    // в этом окне» — вызов, и писать второе поверх первого пришлось бы с
    // собственными номерами запросов в каждом ресурсе.

    /// Сколько ждать ответа, прежде чем считать вызов пропавшим.
    ///
    /// Ждать бесконечно нельзя: обещание, которое никогда не разрешится, — это
    /// утечка, и ресурс, сделавший вызов в цикле, съел бы память сервера. Пять
    /// секунд — столько же, сколько ждёт alt:V.
    const kRpcTimeout = 5000;

    /// Обработчики вызовов по имени. По одному на имя, в отличие от событий:
    /// ответ может быть только один, и двое отвечающих означали бы гонку.
    const answerers = new Map();

    /// Вызовы, ожидающие ответа, по номеру запроса.
    const pending = new Map();
    let nextCall = 1;

    function onRpc(name, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.onRpc ждёт имя вызова и обработчик');
        }

        if (answerers.has(name)) {
            logWarning(`обработчик вызова «${name}» заменён: отвечать может только один`);
        }

        answerers.set(name, handler);
    }

    function offRpc(name) {
        answerers.delete(name);
    }

    /// Отвечает на вызов, пришедший от клиента.
    function answerRpc(player, id, name, args) {
        const handler = answerers.get(name);

        const reply = (ok, value) =>
            native.emitClient(player, '__oxymp:rpc:answer',
                              encodeArgs([id, ok, ok ? value : String(value)]));

        if (handler === undefined) {
            reply(false, `на вызов «${name}» никто не отвечает`);
            return;
        }

        // Обработчик волен вернуть и обещание, и готовое значение: Promise.resolve
        // сводит оба случая к одному, и ветвиться по типу не приходится.
        Promise.resolve()
            .then(() => handler(player, ...args))
            .then((value) => reply(true, value === undefined ? null : value),
                  (failure) => {
                      logError(`ошибка в обработчике вызова «${name}»:`, failure);
                      reply(false, failure?.message ?? failure);
                  });
    }

    /// Делает вызов клиенту и ждёт ответа.
    function callClient(player, name, args) {
        return new Promise((resolve, reject) => {
            const id = nextCall++;

            const timer = setTimeout(() => {
                pending.delete(id);
                reject(new Error(`вызов «${name}» остался без ответа за ${kRpcTimeout} мс`));
            }, kRpcTimeout);

            pending.set(id, { resolve, reject, timer });

            native.emitClient(player, '__oxymp:rpc:call', encodeArgs([id, name, args]));
        });
    }

    /// Принимает ответ на свой вызов.
    function takeAnswer(id, ok, value) {
        const waiting = pending.get(id);
        if (waiting === undefined) {
            // Ответ на вызов, которого уже никто не ждёт: он опоздал и был
            // отброшен по времени. Это не ошибка — просто поздно.
            return;
        }

        clearTimeout(waiting.timer);
        pending.delete(id);

        if (ok) {
            waiting.resolve(value);
        } else {
            waiting.reject(new Error(String(value)));
        }
    }

    onClient('__oxymp:rpc:call', (player, id, name, args) =>
        answerRpc(player, id, name, Array.isArray(args) ? args : []));

    onClient('__oxymp:rpc:answer', (_player, id, ok, value) => takeAnswer(id, ok, value));

    // --- Журнал --------------------------------------------------------------

    function log(...args) {
        native.log(...args.map(render));
    }

    function logWarning(...args) {
        native.logWarning(...args.map(render));
    }

    function logError(...args) {
        native.logError(...args.map(render));
    }

    /// Во что превращается довод журнала.
    ///
    /// Объект — в JSON, а не в «[object Object]»: второе не сообщает ничего, а
    /// журнал сервера читают именно тогда, когда нужно узнать, что внутри.
    function render(value) {
        if (typeof value === 'string') {
            return value;
        }
        if (value instanceof Error) {
            return value.stack ?? value.message;
        }
        if (typeof value === 'object' && value !== null) {
            try {
                return JSON.stringify(value);
            } catch {
                return String(value);
            }
        }

        return String(value);
    }

    // --- Метаданные ----------------------------------------------------------

    /// Метаданные сущностей, разложенные по роду и номеру.
    ///
    /// Живут в скрипте, а не в ядре, и для `meta` это ровно то, что обещает
    /// alt:V: meta не покидает сервер. Разложены по номеру, а не по объекту:
    /// объект-обёртка заводится заново на каждое обращение к сущности, и
    /// WeakMap по нему терял бы записи сразу же.
    const metaStore = new Map();

    function metaFor(kind, id) {
        const key = `${kind}:${id}`;
        let found = metaStore.get(key);

        if (found === undefined) {
            found = new Map();
            metaStore.set(key, found);
        }

        return found;
    }

    /// Прибирает метаданные ушедшей сущности.
    ///
    /// Без этого сервер, переживший тысячу входов, держал бы тысячу словарей
    /// покойников. Зовётся по событию выхода, то есть после того, как обработчики
    /// ресурса отработали, — иначе `playerDisconnect` не нашёл бы того, ради чего
    /// его и объявляют до уборки.
    function forget(kind, id) {
        metaStore.delete(`${kind}:${id}`);
        syncedStore.delete(`${kind}:${id}`);
    }

    /// Метаданные, которые видит и клиент.
    ///
    /// Отдельно от `meta`, и разница не в удобстве, а в том, кто их видит.
    /// `meta` не покидает сервер — так обещает alt:V, и так оно и есть. А
    /// `syncedMeta` обязана дойти до клиента: на ней держатся половина режимов —
    /// имя над головой, состояние двери, номер организации.
    const syncedStore = new Map();

    function syncedFor(kind, id) {
        const key = `${kind}:${id}`;
        let found = syncedStore.get(key);

        if (found === undefined) {
            found = new Map();
            syncedStore.set(key, found);
        }

        return found;
    }

    /// Имя служебного события, которым метаданные уходят клиенту.
    ///
    /// Служебным событием, а не своим сообщением в протоколе, и это осознанно.
    /// Сообщение стоило бы номера, разбора на обеих сторонах и подъёма версии
    /// протокола — ради того, что уже умеет ходить. Две приставки-подчёркивания
    /// говорят читающему, что имя занято и режиму его брать нельзя.
    const kSyncedMetaEvent = '__oxymp:meta';

    /// То же для метаданных, которые видит один игрок.
    const kLocalMetaEvent = '__oxymp:localmeta';

    /// Метаданные, назначенные игроку и видимые только ему.
    ///
    /// Отдельно от synced, а не заодно: synced на то и synced, что её видит
    /// каждый, — а эта принадлежит одному, и попади она в общий склад, ушла бы
    /// всем при первом же снимке для вошедшего.
    const localStore = new Map();

    function localFor(id) {
        let found = localStore.get(id);

        if (found === undefined) {
            found = new Map();
            localStore.set(id, found);
        }

        return found;
    }

    /// Рассылает изменение одному — тому, кому оно назначено.
    function publishLocal(player, key, value) {
        native.emitClient(player, kLocalMetaEvent,
                          encodeArgs([key, value === undefined ? null : value]));
    }

    /// Рассылает изменение всем, кто его увидит.
    ///
    /// Всем, а не одному владельцу: `syncedMeta` на то и synced, что её видит
    /// каждый. Клиент, знающий чужое имя над головой, узнаёт его отсюда.
    ///
    /// Пятым едет род метаданных — обычные или потоковые. Хранятся они у нас в
    /// одном месте и доходят одинаково, но события у alt:V для них разные:
    /// `syncedMetaChange` и `streamSyncedMetaChange`. Не различай мы их здесь,
    /// клиенту пришлось бы либо молчать об одном из двух, либо объявлять оба —
    /// и режим, подписанный на оба, считал бы каждое изменение дважды.
    function publishSynced(kind, id, key, value, streamed) {
        const payload = encodeArgs([kind, id, key, value === undefined ? null : value,
                                    streamed === true]);

        for (const player of native.players()) {
            native.emitClient(player, kSyncedMetaEvent, payload);
        }
    }

    /// Отдаёт вошедшему всё, что уже накоплено.
    ///
    /// Без этого игрок, вошедший вторым, не знал бы о первом ничего: рассылка
    /// случилась до него. Ошибка эта из тех, что не видно на одном игроке и
    /// видно сразу на двух.
    function sendSyncedSnapshot(player) {
        for (const [where, values] of syncedStore) {
            const split = where.indexOf(':');
            const kind = where.slice(0, split);
            const id = Number(where.slice(split + 1));

            for (const [key, value] of values) {
                // Вошедшему всё уходит обычной synced: потоковую от обычной у
                // накопленного не отличить — хранятся они в одном месте. Разница
                // сказывается только на живом изменении, а его вошедший
                // получит уже с признаком.
                native.emitClient(player, kSyncedMetaEvent,
                                  encodeArgs([kind, id, key, value, false]));
            }
        }
    }

    /// Примешивает набор meta-действий классу сущности.
    ///
    /// Обычным присваиванием, а не через Object.defineProperties, и это не
    /// вкусовщина. Последний запечатывает свойство намертво: второе объявление
    /// того же имени бросает «Cannot redefine property», и падает при этом не
    /// то место, где ошибка, а подъём ресурса целиком — без внятной причины.
    /// Проверено: так и случилось, когда рядом с настоящим setSyncedMeta
    /// осталась его прежняя заглушка.
    function addMeta(target, kind) {
        // Обычным объектом, а не набором дескрипторов: так все свойства выходят
        // переопределяемыми сами собой, и заводить их вручную не приходится.
        Object.assign(target.prototype, {
            setMeta(key, value) {
                const store = metaFor(kind, this.id);
                const was = store.get(key);

                store.set(key, value);
                server.fireLocal('metaChange', [this, key, value, was]);
            },
            getMeta(key) { return metaFor(kind, this.id).get(key); },
            hasMeta(key) { return metaFor(kind, this.id).has(key); },
            deleteMeta(key) {
                const store = metaFor(kind, this.id);
                const was = store.get(key);

                store.delete(key);
                server.fireLocal('metaChange', [this, key, undefined, was]);
            },
            getMetaKeys() { return [...metaFor(kind, this.id).keys()]; },

            setSyncedMeta(key, value) {
                const store = syncedFor(kind, this.id);
                const was = store.get(key);

                store.set(key, value);
                publishSynced(kind, this.id, key, value);

                // Событие объявляется и на сервере, а не только у клиента.
                //
                // **Его здесь не было вовсе, и это стоило целого пласта работы
                // режимов.** На клиенте `syncedMetaChange` объявлялся с самого
                // начала, а на сервере — нет, хотя alt:V объявляет его на
                // обеих сторонах и подписываются на него именно на сервере: там
                // живёт всё, что реагирует на смену состояния — двери, счета,
                // принадлежность организации.
                //
                // Доводы — как у alt:V: сущность, ключ, новое значение,
                // прежнее. Сущностью идёт `this`, а не пара «род и номер»:
                // ресурс сравнивает её через `===` со своей, и слепок с теми же
                // числами не сошёлся бы никогда.
                server.fireLocal('syncedMetaChange', [this, key, value, was]);
            },
            getSyncedMeta(key) { return syncedFor(kind, this.id).get(key); },
            hasSyncedMeta(key) { return syncedFor(kind, this.id).has(key); },
            deleteSyncedMeta(key) {
                const store = syncedFor(kind, this.id);
                const was = store.get(key);

                store.delete(key);
                publishSynced(kind, this.id, key, undefined);
                server.fireLocal('syncedMetaChange', [this, key, undefined, was]);
            },
            getSyncedMetaKeys() { return [...syncedFor(kind, this.id).keys()]; },

            /// streamSyncedMeta отличается от synced тем, кому она доходит:
            /// только тем, кто сущность видит. Раздачи по видимости у oxyMP пока
            /// нет, поэтому здесь она ведёт себя как обычная synced — то есть
            /// доходит до всех.
            ///
            /// Разница в пользу режима, а не против: он получит больше, чем
            /// ожидал, но не меньше. Молчать об этом всё же нельзя — потому и
            /// сказано здесь.
            setStreamSyncedMeta(key, value) {
                const store = syncedFor(kind, this.id);
                const was = store.get(key);

                store.set(key, value);
                publishSynced(kind, this.id, key, value, true);
                server.fireLocal('streamSyncedMetaChange', [this, key, value, was]);
            },
            getStreamSyncedMeta(key) { return syncedFor(kind, this.id).get(key); },
            hasStreamSyncedMeta(key) { return syncedFor(kind, this.id).has(key); },
            deleteStreamSyncedMeta(key) {
                const store = syncedFor(kind, this.id);
                const was = store.get(key);

                store.delete(key);
                publishSynced(kind, this.id, key, undefined, true);
                server.fireLocal('streamSyncedMetaChange', [this, key, undefined, was]);
            },
            getStreamSyncedMetaKeys() { return [...syncedFor(kind, this.id).keys()]; },
        });
    }

    // --- Сущности ------------------------------------------------------------

    const Player = native.Player;
    const Vehicle = native.Vehicle;
    const WorldObject = native.Object;
    const Ped = native.Ped;

    /// Угол поворота у alt:V — вектор в радианах, у oxyMP — один угол в градусах.
    ///
    /// Разница не косметическая. Персонаж в игре поворачивается вокруг одной оси,
    /// и второй с третьей у него всегда нули — поэтому потери здесь нет. А вот
    /// единицы теряются молча: ресурс, положивший `rot.z` в натив, ждущий
    /// радианы, получит поворот в шестьдесят раз меньше нужного и будет искать
    /// причину в нативе.
    function headingToRot(heading) {
        return new shared.Vector3(0, 0, toRadians(heading));
    }

    /// Градусы игры — в радианы alt:V и обратно.
    ///
    /// Названы отдельно, потому что перевод понадобился в четырёх местах, а
    /// расписанный по месту он рано или поздно разъедется: перепутанный
    /// множитель здесь не даёт ни ошибки, ни строки в журнале — только поворот
    /// в шестьдесят раз мимо.
    function toRadians(degrees) {
        return Number(degrees) * (Math.PI / 180);
    }

    function toDegrees(radians) {
        return Number(radians) * (180 / Math.PI);
    }

    // --- Привязка сущностей --------------------------------------------------

    /// Род сущности словом — так его понимает мост.
    ///
    /// По `instanceof`, а не по спрятанному полю, и это работает даже через
    /// посредника: `alt.Vehicle` — Proxy над классом ядра, а `instanceof` идёт по
    /// цепочке прототипов, до которой посреднику дела нет.
    function kindOf(entity) {
        if (entity instanceof Player) {
            return 'player';
        }
        if (entity instanceof Vehicle) {
            return 'vehicle';
        }
        if (entity instanceof WorldObject) {
            return 'object';
        }
        if (entity instanceof Ped) {
            return 'ped';
        }

        return '';
    }

    /// Кость: у alt:V она либо номер, либо имя. Имя переводит игра — номера
    /// костей свои у каждой модели, и сервер их не знает.
    function boneOf(bone) {
        if (typeof bone === 'string') {
            return { bone: -1, boneName: bone };
        }

        // Не названная кость — минус единица: «к самой сущности, а не к кости».
        // Ноль здесь означал бы первую кость модели, а это совсем другое место.
        const index = Number(bone);
        return { bone: Number.isFinite(index) ? index : -1, boneName: '' };
    }

    /// Названа ли своя кость.
    ///
    /// Своей кости у нас нет: натива, привязывающего кость к кости, в нашей
    /// сборке игры не нашлось. Привязка от этого не отменяется — она делается по
    /// кости цели, — но промолчать нельзя: ресурс, назвавший обе, получит не то,
    /// что просил, и искать причину будет в игре.
    function ownBoneNamed(bone) {
        if (typeof bone === 'string') {
            return bone !== '';
        }

        return bone !== undefined && bone !== null && Number(bone) > 0;
    }

    /// Примешивает привязку классу сущности.
    ///
    /// Тем же приёмом, что и метаданные: род известен здесь и попадает в замыкание,
    /// а сущность о нём ничего не знает и знать не должна.
    function addAttach(target, kind) {
        Object.assign(target.prototype, {
            attachTo(entity, entityBone, ownBone, pos, rot, enableCollisions, noFixedRotation) {
                const targetKind = kindOf(entity);

                if (targetKind === '') {
                    throw new Error(`${kind}.attachTo: первым доводом нужна сущность сессии`);
                }

                if (ownBoneNamed(ownBone)) {
                    warnOnce(`${kind}.attachTo`,
                             'своя кость не передаётся — привязываем по кости цели');
                }

                const where = boneOf(entityBone);
                const turn = new shared.Vector3(rot ?? { x: 0, y: 0, z: 0 });

                return native.attachEntity(kind, this.id, {
                    targetKind,
                    target: entity.id,
                    bone: where.bone,
                    boneName: where.boneName,
                    position: new shared.Vector3(pos ?? { x: 0, y: 0, z: 0 }),

                    // Поворот у alt:V в радианах, у нас в градусах. Единицы здесь
                    // теряются молча: ресурс получил бы поворот в шестьдесят раз
                    // меньше нужного и искал бы причину в игре.
                    rotation: new shared.Vector3(turn.x * (180 / Math.PI),
                                                 turn.y * (180 / Math.PI),
                                                 turn.z * (180 / Math.PI)),

                    collision: Boolean(enableCollisions),

                    // У alt:V довод назван наоборот — `noFixedRotation`, — и это
                    // сказано в его же описании: «если false, поворот закреплён».
                    fixedRotation: !noFixedRotation,
                });
            },

            detach() {
                return native.detachEntity(kind, this.id);
            },
        });
    }

    Object.defineProperties(Player.prototype, {
        /// Позиция. Присваивание переносит игрока — так же, как в alt:V.
        pos: {
            get() { return new shared.Vector3(this.position); },
            set(value) { this.teleport(new shared.Vector3(value)); },
        },
        rot: {
            get() { return headingToRot(this.heading); },
        },
        /// Признаки состояния приходят прямо от ядра: `isDead`, `isAiming`,
        /// `isShooting`, `isInRagdoll`, `isJumping`, `isCrouching`,
        /// `isParachuting`, `isReloading`, `isInCover`, `isInMelee`,
        /// `isEnteringVehicle`, `isLeavingVehicle`, `isInWater`, `isSpawned`.
        /// Оттуда же `aimPos` и `currentWeapon`. Все они ехали в снимке с
        /// самого начала — недоставало не сведений, а дороги наружу.
        ///
        /// Четырёх признаков alt:V у нас нет, и они отказывают вслух, а не
        /// отвечают «нет». Правило то же, что и везде здесь: тишина на вопрос —
        /// это ложь, и ресурс, принявший её за правду, унесёт эту ложь дальше.
        /// Ответь мы «не крадётся» про того, кто крадётся, — режим со скрытным
        /// перемещением сломался бы молча и навсегда.
        isOnLadder: { get: absent('player.isOnLadder') },
        isOnVehicle: { get: absent('player.isOnVehicle') },
        isStealthy: { get: absent('player.isStealthy') },
        isSuperJumpEnabled: { get: absent('player.isSuperJumpEnabled') },
        /// Слой мира, в котором игрок находится. Есть у ядра и работает.
        /// Как игрока зовут. У alt:V это `name`, и оно уже есть в ядре.
        /// alt:V даёт `valid` полем — у ядра оно уже такое.
        toString: {
            value() { return `Player{ id: ${this.id}, name: ${this.name} }`; },
        },
        emit: {
            /// alt:V зовёт это `player.emit` — то же, что `alt.emitClient(player, ...)`.
            value(name, ...args) { emitClient(this, name, ...args); },
        },
        emitRaw: {
            value(name, payload) { native.emitClient(this, name, String(payload)); },
        },
        /// Вызов клиенту с ответом. Обещание — как в alt:V.
        emitRpc: {
            value(name, ...args) { return callClient(this, name, args); },
        },
        // `kick` не переопределяется: ядро уже даёт его с той же подписью, что и
        // alt:V, — `player.kick(reason)`.
        spawn: {
            value(...args) {
                // alt:V принимает и (pos), и (x, y, z), и с задержкой последним.
                const point = args.length >= 3
                    ? new shared.Vector3(args[0], args[1], args[2])
                    : new shared.Vector3(args[0]);

                this.teleport(point);
            },
        },
        /// Смывает кровь с персонажа.
        ///
        /// Не исполняется, и об этом говорится вслух — но один раз и без отказа.
        /// Отказ здесь был бы хуже: режимы зовут это в обработчике входа, следом
        /// за назначением модели, и брошенное исключение унесло бы с собой всё
        /// остальное, что этот обработчик делает, — вплоть до показа интерфейса.
        /// Ровно это и случалось.
        clearBloodDamage: {
            value() {
                warnOnce('player.clearBloodDamage',
                         'кровь с персонажа не смывается: распоряжений своему телу ' +
                         'сервер клиенту пока не шлёт');
            },
        },
        /// Одежда и аксессуары есть у ядра и работают: `setClothes`, `setProp`
        /// и `clearProp` приходят прямо оттуда.
        ///
        /// Одежда из дополнений при этом достижима — но своим, сквозным
        /// номером, а не парой «набор и место в нём». Номера дополнений идут
        /// следом за основными, и с расширением поля до двух байт вещь под
        /// номером триста надевается так же, как пятнадцатая.
        ///
        /// А вот `setDlcClothes` и `getDlcClothes` — нет, и это не лень. Пары
        /// «хеш набора и место в нём» игра обратно не отдаёт: нативов для этого
        /// нет ни в открытой базе, ни в таблице alt:V, а перевод такой пары в
        /// сквозной номер требует разбора файлов игры, которого у нас нет.
        /// Промолчать было бы хуже: ресурс решил бы, что одел человека.
        setDlcClothes: {
            value: unperformed('player.setDlcClothes',
                               'одежду из наборов зовите setClothes со сквозным номером: ' +
                               'вещи наборов идут следом за основными'),
        },
        setDlcProp: {
            value: unperformed('player.setDlcProp',
                               'аксессуары из наборов зовите setProp со сквозным номером'),
        },
        /// Вопрос, а не распоряжение: молча ответить нечем, и потому он
        /// отказывает вслух. Ответ «набор нулевой» был бы ложью про всякую
        /// вещь из дополнения, а ресурс унёс бы её дальше.
        getDlcClothes: { value: absent('player.getDlcClothes') },
        setWeather: { value: unperformed('player.setWeather', 'погода у нас общая на сессию — см. alt.setWeather') },
        setDateTime: { value: unperformed('player.setDateTime', 'часы у нас общие на сессию — см. alt.setTime') },
        /// Посадить игрока в машину ядро умеет: просьбой ему самому.
        ///
        /// У alt:V место называется вторым доводом, и минус единица означает
        /// «за руль». У нас так же — нумерация взята у самой игры.
        /// Велит персонажу играть движение.
        ///
        /// Доводы — те же и в том же порядке, что у alt:V. Уходит оно не одному
        /// хозяину персонажа, а всем, кто игрока видит: у остальных он показан
        /// куклой, и молчащая кукла осталась бы стоять столбом.
        ///
        /// Движение спорит с задачами, которыми ведутся куклы: идущему оно
        /// достанется наполовину — его тело в это время ведёт задача ходьбы.
        /// Стоящему — целиком, и это тот случай, ради которого движения и зовут.
        playAnimation: {
            value(dictionary, name, blendIn, blendOut, duration, flags, playbackRate,
                  lockX, lockY, lockZ) {
                return native.playAnimation(this.id, {
                    dictionary: String(dictionary ?? ''),
                    name: String(name ?? ''),
                    blendIn: blendIn === undefined ? 8 : Number(blendIn),
                    blendOut: blendOut === undefined ? 8 : Number(blendOut),
                    duration: duration === undefined ? -1 : Number(duration),
                    flags: Number(flags) || 0,
                    playbackRate: playbackRate === undefined ? 1 : Number(playbackRate),
                    lockX: Boolean(lockX),
                    lockY: Boolean(lockY),
                    lockZ: Boolean(lockZ),
                });
            },
        },
        /// Снимает с персонажа все задачи, включая начатое движение.
        clearTasks: {
            value() {
                return native.clearTasks(this.id);
            },
        },
    });

    Object.defineProperties(Player, {
        all: { get() { return native.players(); } },
        count: { get() { return native.players().length; } },
        getByID: {
            value(id) {
                return native.players().find((player) => player.id === id) ?? null;
            },
        },
    });

    // --- Внешность машины ----------------------------------------------------
    //
    // У ядра внешность ходит целиком: одно описание — одно сообщение клиентам.
    // У alt:V она разложена на два десятка свойств, и всякое пишется порознь.
    // Отсюда `reshape`: прочитать целиком, поправить названное, записать целиком.
    //
    // Читается она заново на каждое обращение, и это не расточительство, а то же
    // правило, что и везде: слой не хранит правду о мире. Машину могли
    // перекрасить и без нас — из другого ресурса или тем же ресурсом строкой
    // выше, — и запомненное здесь разошлось бы с настоящим в тот же миг.

    /// Места тюнинга, которые не выбираются из списка, а включаются.
    ///
    /// Турбина, дым из-под колёс, ксенон. У alt:V они ставятся тем же `setMod`,
    /// что и остальные, — нулём или единицей, — а у нас живут отдельным набором
    /// битов: игра и спрашивает о них другим нативом.
    const TOGGLE_MODS = new Set([18, 20, 22]);

    /// Стороны неона в том порядке, в каком их нумерует игра.
    const NEON_SIDES = { left: 1, right: 2, front: 4, back: 8 };

    /// Места дисков в нумерации игры: передние и задние.
    const FRONT_WHEELS = 23;
    const REAR_WHEELS = 24;

    /// Сколько у машины мест под тюнинг и «дополнений» кузова. Числа игры.
    ///
    /// Проверять их приходится здесь: место за пределами набора ядро отбросит
    /// молча — набор у него ровно такой длины, — и ресурс, промахнувшийся
    /// номером, искал бы пропавшую деталь в игре.
    const MOD_SLOTS = 49;
    const FIRST_EXTRA = 1;
    const LAST_EXTRA = 14;

    /// Внешность машины, какой её видит ядро.
    ///
    /// Машины уже нет — пустое описание, а не бросок: свойства внешности читают
    /// в перечислениях и в обработчиках, и машина, исчезнувшая между двумя
    /// строками, здесь обычное дело.
    function look(vehicle) {
        return vehicle.appearance ?? { mods: [] };
    }

    /// Правит внешность машины и отдаёт, удалось ли.
    function reshape(vehicle, change) {
        const worn = vehicle.appearance;
        if (worn === undefined) {
            return false;
        }

        change(worn);
        return vehicle.setAppearance(worn);
    }

    /// Свойство внешности: читается из описания, пишется правкой на месте.
    function looks(read, write) {
        return {
            get() { return read(look(this)); },
            set(value) { reshape(this, (worn) => write(worn, value)); },
        };
    }

    /// Цвет из трёх полей описания — и обратно.
    ///
    /// Прозрачность у alt:V в RGBA есть, а у неона и дыма её нет и в игре:
    /// принимаем и отдаём непрозрачный.
    function coloured(prefix) {
        return {
            get() {
                const worn = look(this);
                return new shared.RGBA(worn[`${prefix}Red`] ?? 0, worn[`${prefix}Green`] ?? 0,
                                       worn[`${prefix}Blue`] ?? 0, 255);
            },
            set(value) {
                reshape(this, (worn) => {
                    worn[`${prefix}Red`] = Number(value?.r) || 0;
                    worn[`${prefix}Green`] = Number(value?.g) || 0;
                    worn[`${prefix}Blue`] = Number(value?.b) || 0;
                });
            },
        };
    }

    /// Своя краска машины: три байта плюс признак «покрашена».
    ///
    /// Признак нужен затем, что чёрная краска осмысленна, а «нет краски» —
    /// отдельное состояние: игра снимает её своим вызовом. Отличить одно от
    /// другого по цвету нельзя, оттого и поле.
    ///
    /// Присваивание null снимает краску — так же, как у alt:V.
    function painted(prefix) {
        return {
            get() {
                const worn = look(this);

                if (!worn[prefix]) {
                    return null;
                }

                return new shared.RGBA(worn[`${prefix}Red`] ?? 0, worn[`${prefix}Green`] ?? 0,
                                       worn[`${prefix}Blue`] ?? 0, 255);
            },
            set(value) {
                reshape(this, (worn) => {
                    if (value === null || value === undefined) {
                        worn[prefix] = false;
                        return;
                    }

                    worn[prefix] = true;
                    worn[`${prefix}Red`] = Number(value?.r) || 0;
                    worn[`${prefix}Green`] = Number(value?.g) || 0;
                    worn[`${prefix}Blue`] = Number(value?.b) || 0;
                });
            },
        };
    }

    Object.defineProperties(Vehicle.prototype, {
        pos: {
            get() { return new shared.Vector3(this.position); },
            set(value) { this.teleport(new shared.Vector3(value)); },
        },
        /// Поворот машины.
        ///
        /// **Единицы здесь расходились молча, и это та же беда, от которой
        /// уберегает `headingToRot` у игрока.** `GET_ENTITY_ROTATION` отвечает
        /// градусами, и градусы же лежат в снимке; у alt:V `rot` — радианы.
        /// Прежде градусы отдавались как есть, и ресурс, положивший `rot.z` в
        /// натив, ждущий радианы, получал поворот в шестьдесят раз больше
        /// нужного — без ошибки и без единой строки в журнале.
        ///
        /// Присваивание разворачивает машину по-настоящему. Прежде оно молча не
        /// делало ничего, хотя дорога для него была готова: распоряжение
        /// `VehicleTeleport` несёт и точку, и поворот с самого начала, а
        /// исполняет его ведущий — машина живёт в игре у него.
        ///
        /// Разворачивается только вокруг вертикальной оси, и это не урезание:
        /// больше и не переносится. Ставить машину набок распоряжением незачем,
        /// а перевернувшуюся поднимет физика.
        rot: {
            get() {
                const turn = this.rotation;
                return new shared.Vector3(toRadians(turn.x), toRadians(turn.y),
                                          toRadians(turn.z));
            },
            set(value) {
                const turn = new shared.Vector3(value);
                this.teleport(new shared.Vector3(this.position), toDegrees(turn.z));
            },
        },
        /// Кто за рулём. У alt:V это `driver`, у ядра — `owner` (ведущий).
        ///
        /// Имена разные, потому что и смысл не совпадает целиком: ведущий у oxyMP
        /// — тот, кто считает физику машины, и это почти всегда водитель, но
        /// машина без водителя ведущего не теряет. Здесь отдаётся то же, что и
        /// `owner`, и расхождение стоит помнить.
        driver: { get() { return this.owner; } },
        toString: {
            value() { return `Vehicle{ id: ${this.id} }`; },
        },

        // Цвета из палитры игры.
        primaryColor: looks((worn) => worn.primaryColour ?? 0,
                            (worn, value) => { worn.primaryColour = Number(value) || 0; }),
        secondaryColor: looks((worn) => worn.secondaryColour ?? 0,
                              (worn, value) => { worn.secondaryColour = Number(value) || 0; }),
        pearlColor: looks((worn) => worn.pearlescentColour ?? 0,
                          (worn, value) => { worn.pearlescentColour = Number(value) || 0; }),
        wheelColor: looks((worn) => worn.wheelColour ?? 0,
                          (worn, value) => { worn.wheelColour = Number(value) || 0; }),

        tireSmokeColor: coloured('tyreSmoke'),
        neonColor: coloured('neon'),

        /// Какие полосы неона горят. У alt:V это объект из четырёх признаков.
        neon: {
            get() {
                const sides = look(this).neonSides ?? 0;
                return {
                    left: (sides & NEON_SIDES.left) !== 0,
                    right: (sides & NEON_SIDES.right) !== 0,
                    front: (sides & NEON_SIDES.front) !== 0,
                    back: (sides & NEON_SIDES.back) !== 0,
                };
            },
            set(value) {
                reshape(this, (worn) => {
                    let sides = 0;

                    for (const [name, bit] of Object.entries(NEON_SIDES)) {
                        if (value?.[name]) {
                            sides |= bit;
                        }
                    }

                    worn.neonSides = sides;
                });
            },
        },

        numberPlateText: looks((worn) => worn.plate ?? '',
                               (worn, value) => { worn.plate = String(value ?? ''); }),
        numberPlateIndex: looks((worn) => worn.plateStyle ?? 0,
                                (worn, value) => { worn.plateStyle = Number(value) || 0; }),

        livery: looks((worn) => worn.livery ?? -1,
                      (worn, value) => { worn.livery = Number(value) || 0; }),
        windowTint: looks((worn) => worn.windowTint ?? -1,
                          (worn, value) => { worn.windowTint = Number(value) || 0; }),
        dirtLevel: looks((worn) => worn.dirtLevel ?? 0,
                         (worn, value) => { worn.dirtLevel = Number(value) || 0; }),
        customTires: looks((worn) => worn.customTyres === true,
                           (worn, value) => { worn.customTyres = Boolean(value); }),

        /// Тип дисков и их вариации. У alt:V они только читаются: ставит их
        /// `setWheels`, и разделение это повторено здесь нарочно — ресурс,
        /// написанный под alt:V, зовёт именно его.
        wheelType: { get() { return look(this).wheelType ?? -1; } },
        frontWheels: { get() { return look(this).mods?.[FRONT_WHEELS] ?? -1; } },
        rearWheels: { get() { return look(this).mods?.[REAR_WHEELS] ?? -1; } },

        setWheels: {
            value(type, variation) {
                return reshape(this, (worn) => {
                    worn.wheelType = Number(type) || 0;
                    worn.mods[FRONT_WHEELS] = Number(variation) || 0;
                });
            },
        },
        setRearWheels: {
            value(variation) {
                return reshape(this, (worn) => {
                    worn.mods[REAR_WHEELS] = Number(variation) || 0;
                });
            },
        },

        /// Набор деталей. У alt:V ноль означает «тюнинг не поставить», и ресурсы
        /// проверяют это перед всяким `setMod`.
        ///
        /// У нас набор у машины есть всегда: клиент выдаёт его перед тем, как
        /// накладывать тюнинг, — иначе игра приняла бы вызовы и не сделала ничего.
        /// Поэтому единица здесь не заглушка, а правда, сказанная на языке alt:V.
        modKit: {
            get() { return 1; },
            set(value) {
                if (Number(value) > 1) {
                    warnOnce('vehicle.modKit',
                             'второго набора деталей мы не передаём — у машины всегда первый');
                }
            },
        },
        modKitsCount: { get() { return 1; } },

        getMod: {
            value(category) {
                const slot = Number(category) || 0;
                const worn = look(this);

                if (TOGGLE_MODS.has(slot)) {
                    return ((worn.toggleMods ?? 0) >>> slot) & 1;
                }

                return worn.mods?.[slot] ?? -1;
            },
        },
        setMod: {
            value(category, id) {
                const slot = Number(category) || 0;
                const chosen = Number(id) || 0;

                if (slot < 0 || slot >= MOD_SLOTS) {
                    warnOnce('vehicle.setMod',
                             `места тюнинга ${slot} у игры нет — их сорок девять, с нуля`);
                    return false;
                }

                return reshape(this, (worn) => {
                    if (!TOGGLE_MODS.has(slot)) {
                        worn.mods[slot] = chosen;
                        return;
                    }

                    worn.toggleMods = chosen === 0
                        ? (worn.toggleMods & ~(1 << slot)) >>> 0
                        : (worn.toggleMods | (1 << slot)) >>> 0;
                });
            },
        },

        /// Сколько деталей есть у этой модели в этом месте.
        ///
        /// Отказом, а не числом: ответ на это знает игра, а не сервер, — он лежит
        /// в её файлах моделей, которых у нас нет. Соврать нулём значило бы
        /// показать игроку пустое меню тюнинга и оставить его гадать, почему.
        getModsCount: { value: absent('vehicle.getModsCount') },

        getExtra: {
            value(id) {
                const extra = Number(id) || 0;
                return ((look(this).extras ?? 0) & (1 << (extra - 1))) !== 0;
            },
        },
        setExtra: {
            value(id, state) {
                const extra = Number(id) || 0;

                if (extra < FIRST_EXTRA || extra > LAST_EXTRA) {
                    warnOnce('vehicle.setExtra',
                             `дополнения кузова ${extra} у игры нет — они с первого по`
                             + ' четырнадцатое');
                    return false;
                }

                return reshape(this, (worn) => {
                    worn.extras = state
                        ? (worn.extras | (1 << (extra - 1))) >>> 0
                        : (worn.extras & ~(1 << (extra - 1))) >>> 0;
                });
            },
        },

        // Своя краска — поверх номера палитры, и снимается присваиванием null.
        customPrimaryColor: painted('customPrimary'),
        customSecondaryColor: painted('customSecondary'),

        // Того, чего в протоколе внешности нет вовсе. Вопросы отказывают вслух,
        // распоряжения говорят о себе один раз: см. absent и unperformed.
        //
        // Цвет салона, панели и раскраска крыши не попали в протокол не по
        // забывчивости: нативов, которыми их читают, нет в открытой базе имён, а
        // подставлять хеш по памяти — верный способ уронить игру. Появятся в базе
        // — появятся и здесь.
        interiorColor: {
            get: absent('vehicle.interiorColor'),
            set: unperformed('vehicle.interiorColor', 'цвет салона не передаётся'),
        },
        dashboardColor: {
            get: absent('vehicle.dashboardColor'),
            set: unperformed('vehicle.dashboardColor', 'цвет приборной панели не передаётся'),
        },
        roofLivery: {
            get: absent('vehicle.roofLivery'),
            set: unperformed('vehicle.roofLivery', 'раскраска крыши не передаётся'),
        },
        darkness: {
            get: absent('vehicle.darkness'),
            set: unperformed('vehicle.darkness', 'затемнение особых машин не передаётся'),
        },
        getAppearanceDataBase64: { value: absent('vehicle.getAppearanceDataBase64') },
        setAppearanceDataBase64: {
            value: unperformed('vehicle.setAppearanceDataBase64',
                               'внешность у нас своя по составу, чужую запись не разобрать'),
        },
    });

    Object.defineProperties(Vehicle, {
        all: { get() { return native.vehicles(); } },
        count: { get() { return native.vehicles().length; } },
        getByID: {
            value(id) {
                return native.vehicles().find((vehicle) => vehicle.id === id) ?? null;
            },
        },
    });

    // --- Прохожие ------------------------------------------------------------
    //
    // Кукла правится целиком: её состояние уходит клиентам одним сообщением.
    // Отсюда тот же приём, что и у внешности машины, — прочитать целиком,
    // поправить названное, записать целиком. Читается заново на каждое
    // обращение: слой не хранит правду о мире.

    /// Уборка куклы у ядра: своя, перекрытая ниже, зовёт её.
    const coreDestroyPed = Ped.prototype.destroy;

    /// Описание куклы, каким его понимает ядро.
    function shapeOfPed(ped) {
        return {
            model: ped.model,
            position: ped.position,
            rotation: ped.rotation,
            health: ped.health,
            maxHealth: ped.maxHealth,
            armour: ped.armour,
            weapon: ped.weapon,
            dimension: ped.dimension,
        };
    }

    /// Правит куклу и отдаёт, удалось ли. Куклы уже нет — не удалось.
    function reshapePed(ped, change) {
        if (!ped.valid) {
            return false;
        }

        const wanted = shapeOfPed(ped);
        change(wanted);

        return ped.update(wanted);
    }

    Object.defineProperties(Ped.prototype, {
        pos: {
            get() { return new shared.Vector3(this.position); },
            set(value) {
                const point = new shared.Vector3(value);
                reshapePed(this, (wanted) => { wanted.position = point; });
            },
        },
        rot: {
            get() {
                const turn = new shared.Vector3(this.rotation);
                return new shared.Vector3(turn.x * (Math.PI / 180), turn.y * (Math.PI / 180),
                                          turn.z * (Math.PI / 180));
            },
            set(value) {
                // Поворот у alt:V в радианах, у нас в градусах — как и везде.
                const turn = new shared.Vector3(value);
                const degrees = new shared.Vector3(turn.x * (180 / Math.PI),
                                                   turn.y * (180 / Math.PI),
                                                   turn.z * (180 / Math.PI));

                reshapePed(this, (wanted) => { wanted.rotation = degrees; });
            },
        },

        // Здоровье, броня и оружие здесь не переопределяются: они есть у ядра
        // под теми же именами и с сеттерами. Объяви мы их ещё и здесь, у
        // прототипа вышла бы вторая пара, которую свойство самого объекта всё
        // равно закроет собой, — и присваивание уходило бы в никуда.
        currentWeapon: {
            get() { return this.weapon ?? 0; },
            set(value) { this.weapon = Number(value) || 0; },
        },

        /// У alt:V статичная кукла — та, у которой нет сетевого владельца и
        /// которой сервер распоряжается целиком. У нас других не бывает: кукла
        /// стоит там, где её поставили, и ведущего у неё нет.
        isStaticEntity: {
            get() { return true; },
            set: unperformed('ped.isStaticEntity',
                             'куклы у нас все статичные — ведущего у них нет'),
        },

        netOwner: { get() { return null; } },

        toString: {
            value() { return `Ped{ id: ${this.id} }`; },
        },

        /// Уборка куклы забирает с собой и её метаданные.
        ///
        /// Отдельным перекрытием, потому что события `pedDestroy` у нас нет:
        /// куклу убирает один-единственный вызов, и цепляться больше не за что.
        /// У игрока и машины для этого есть события, и там метаданные забываются
        /// последним подписчиком — обработчик вправе прочесть их напоследок.
        destroy: {
            value() {
                const id = this.id;
                const gone = coreDestroyPed.call(this);

                if (gone) {
                    forget('ped', id);
                }

                return gone;
            },
        },
    });

    Object.defineProperties(Ped, {
        all: { get() { return native.peds(); } },
        count: { get() { return native.peds().length; } },
        getByID: {
            value(id) {
                return native.peds().find((ped) => ped.id === id) ?? null;
            },
        },
    });

    /// `new alt.Ped(...)` — так кукол и заводят в alt:V.
    ///
    /// Посредником над классом ядра, а не своим классом, и по той же причине,
    /// что и у машины: `instanceof alt.Ped` обязан узнавать кукол, пришедших из
    /// ядра, — а они приходят его классом.
    const ConstructiblePed = new Proxy(Ped, {
        construct(target, args) {
            const [model, position, rotation, streamingDistance, isStatic] = args;

            // Поводы разные, и ключи у них разные: warnOnce молчит о втором
            // поводе, если первый уже сказан под тем же именем, — а это два
            // разных умолчания, и знать о них нужно про оба.
            if (streamingDistance !== undefined) {
                warnOnce('alt.Ped.streamingDistance',
                         'своей дальности видимости у куклы нет — её раздаёт сервер по общей');
            }

            if (isStatic === false) {
                warnOnce('alt.Ped.isStaticEntity',
                         'нестатичных кукол у нас нет: ведущего им никто не назначает');
            }

            const hashed = typeof model === 'string' ? shared.hash(model) : model;
            const turn = rotation === undefined
                ? new shared.Vector3(0, 0, 0)
                : new shared.Vector3(rotation);

            const ped = native.createPed({
                model: hashed,
                position: new shared.Vector3(position),
                rotation: new shared.Vector3(turn.x * (180 / Math.PI), turn.y * (180 / Math.PI),
                                             turn.z * (180 / Math.PI)),
            });

            if (ped === null) {
                throw new Error(`alt.Ped: кукла модели ${hashed} не поставилась`);
            }

            return ped;
        },
    });

    addMeta(Player, 'player');
    addMeta(Vehicle, 'vehicle');
    addMeta(Ped, 'ped');

    /// Метаданные, видимые одному игроку. Есть только у игрока — у alt:V тоже.
    ///
    /// Читает их клиент модульными функциями `alt.getLocalMeta`, а не у своей
    /// сущности: так объявлено у alt:V, и своей сущности у него для этого нет —
    /// метаданные эти принадлежат не телу, а тому, кто за ним сидит.
    Object.assign(Player.prototype, {
        setLocalMeta(key, value) {
            const store = localFor(this.id);
            const было = store.get(key);

            store.set(key, value);
            publishLocal(this, key, value);

            // Объявляется и на сервере: у alt:V `localMetaChange` есть здесь, и
            // подписываются на него именно тут — там, где живёт то, что
            // реагирует на перемену.
            server.fireLocal('localMetaChange', [this, key, value, было]);
        },
        getLocalMeta(key) { return localFor(this.id).get(key); },
        hasLocalMeta(key) { return localFor(this.id).has(key); },
        deleteLocalMeta(key) {
            const store = localFor(this.id);
            const было = store.get(key);

            store.delete(key);
            publishLocal(this, key, undefined);
            server.fireLocal('localMetaChange', [this, key, undefined, было]);
        },
        getLocalMetaKeys() { return [...localFor(this.id).keys()]; },
    });

    // Привязка — всем трём родам: у alt:V она объявлена у Entity, а Entity здесь
    // нет вовсе. Общего предка у наших классов не завести: они приходят из ядра
    // порознь, и связать их одним прототипом значило бы подменить чужие классы
    // своими.
    addAttach(Player, 'player');
    addAttach(Vehicle, 'vehicle');
    addAttach(WorldObject, 'object');
    addAttach(Ped, 'ped');

    // Метаданные вышедшего игрока убираются последним подписчиком, а не первым:
    // событие объявляется до уборки нарочно (см. CLAUDE.md), и ресурс вправе
    // прочесть их в своём обработчике.
    on('playerConnect', (player) => sendSyncedSnapshot(player));

    // Уходящий уносит свои личные метаданные с собой: номер игрока сервер
    // выдаёт заново, и оставленное досталось бы следующему под тем же номером.
    on('playerDisconnect', (player) => localStore.delete(player.id));
    on('playerDisconnect', (player) => forget('player', player.id));
    on('vehicleDestroy', (vehicle) => forget('vehicle', vehicle.id));

    /// Заводит машину. Довод — как в alt:V: модель именем или хешем.
    function createVehicle(model, position, rotation) {
        const hashed = typeof model === 'string' ? shared.hash(model) : model;
        const point = new shared.Vector3(position);

        // alt:V принимает поворот вектором в радианах; ядру нужен один угол в
        // градусах. Берётся ось Z — вокруг неё машина и стоит.
        const heading = rotation === undefined
            ? 0
            : new shared.Vector3(rotation).z * (180 / Math.PI);

        return native.createVehicle(hashed, point, heading);
    }

    /// `new alt.Vehicle(...)` — так машины и заводят в alt:V.
    ///
    /// Посредником над классом ядра, а не своим классом, и это существенно:
    /// `instanceof alt.Vehicle` обязан по-прежнему узнавать машины, пришедшие из
    /// ядра, — а они объекты того самого класса. Подмени мы его своим, всякая
    /// проверка рода перестала бы сходиться, и притом молча.
    ///
    /// Доводы принимаются в обоих видах, какими их пишут: числами по одному и
    /// векторами. Поворот берётся вокруг оси Z — вокруг неё машина и стоит.
    const ConstructibleVehicle = new Proxy(Vehicle, {
        construct(_target, args) {
            const [model] = args;

            const position = args.length >= 4
                ? new shared.Vector3(args[1], args[2], args[3])
                : new shared.Vector3(args[1]);

            const rotation = args.length >= 7
                ? new shared.Vector3(args[4], args[5], args[6])
                : args[2];

            const vehicle = createVehicle(model, position, rotation);

            if (vehicle === null) {
                throw new Error(`alt.Vehicle: машина модели ${model} не завелась`);
            }

            return vehicle;
        },
    });

    // --- Кто рядом ------------------------------------------------------------

    /// Сущности сессии, отобранные родом, слоем мира и расстоянием.
    ///
    /// `position` в пустоту означает «не отбирать по расстоянию» — так работает
    /// `getEntitiesInDimension`. Слой мира в пустоту означает «любой»: у alt:V
    /// довод обязателен, но ноль там законный слой, и отличить «в нулевом» от
    /// «в любом» иначе нечем.
    function entitiesNear(position, range, dimension, allowedTypes) {
        const kinds = alt.enums.BaseObjectFilterType;

        // Не названный набор означает «все роды», как и у alt:V.
        const wanted = Number.isFinite(Number(allowedTypes)) && Number(allowedTypes) !== 0
            ? Number(allowedTypes)
            : kinds.Player | kinds.Vehicle | kinds.Ped | kinds.Object;

        const slice = Number(dimension);
        const inSlice = (entity) =>
            !Number.isFinite(slice) || entity.dimension === slice;

        const at = position === null ? null : new shared.Vector3(position);
        const reach = Number(range);
        const close = (entity) =>
            at === null || at.distanceToSquared(entity.pos) <= reach * reach;

        const found = [];

        const take = (flag, list) => {
            if ((wanted & flag) === 0) {
                return;
            }

            for (const entity of list) {
                if (inSlice(entity) && close(entity)) {
                    found.push(entity);
                }
            }
        };

        take(kinds.Player, Player.all);
        take(kinds.Vehicle, Vehicle.all);
        take(kinds.Ped, Ped.all);
        take(kinds.Object, WorldObject.all);

        return found;
    }

    /// Ближайшая сущность одного рода. Пусто — рядом никого.
    function nearestOf(kind, options) {
        const where = options?.pos;

        if (where === undefined || where === null) {
            throw new TypeError('нужен объект вида { pos, range }');
        }

        const at = new shared.Vector3(where);

        // Не названная дальность означает «где угодно»: так у alt:V, и так же
        // ведёт себя его же описание — довод там необязателен.
        const reach = Number.isFinite(Number(options?.range)) ? Number(options.range) : Infinity;

        let nearest = null;
        let nearestAt = Infinity;

        for (const entity of entitiesNear(null, 0, undefined, kind)) {
            const away = at.distanceToSquared(entity.pos);

            if (away <= reach * reach && away < nearestAt) {
                nearest = entity;
                nearestAt = away;
            }
        }

        return nearest;
    }

    Object.defineProperties(WorldObject.prototype, {
        /// Где предмет стоит и как повёрнут.
        ///
        /// Присваивание переставляет его по-настоящему и сразу у всех: ведущего
        /// у предмета нет, двигает его сервер. Прежде оно молча не делало
        /// ничего, и молчание это было дорогим — переставить ящик или рампу
        /// нужно всякому режиму, а узнать, что просьба ушла в пустоту, было не
        /// по чему.
        ///
        /// Поворот, как и у машины, переводится: в ядре градусы, у alt:V
        /// радианы.
        pos: {
            get() { return new shared.Vector3(this.position); },
            set(value) { this.place(new shared.Vector3(value), null); },
        },
        rot: {
            get() {
                const turn = this.rotation;
                return new shared.Vector3(toRadians(turn.x), toRadians(turn.y),
                                          toRadians(turn.z));
            },
            set(value) {
                const turn = new shared.Vector3(value);
                this.place(null, new shared.Vector3(toDegrees(turn.x), toDegrees(turn.y),
                                                    toDegrees(turn.z)));
            },
        },
        toString: {
            value() { return `Object{ id: ${this.id} }`; },
        },

        /// У alt:V предмет умеет ещё и качаться, гореть и быть невидимым —
        /// у нас он стоит. Так он и заведён на сервере: подвинуть его может
        /// лишь тот, кто поставил, и подвинет он его у всех разом.
        ///
        /// Молчать об этом нельзя: ресурс, потушивший невидимость, решил бы,
        /// что предмет виден.
        alpha: {
            get() { return 255; },
            set: unperformed('object.alpha', 'прозрачность предмета сервером не задаётся'),
        },
        collision: {
            get() { return true; },
            set: unperformed('object.collision', 'столкновения предмета сервером не задаются'),
        },
        isCollisionEnabled: { get() { return true; } },
        activatePhysics: { value: unperformed('object.activatePhysics',
                                       'физика предмета у нас не считается: он стоит') },
    });

    Object.defineProperties(WorldObject, {
        all: { get() { return native.objects(); } },
        count: { get() { return native.objects().length; } },
        getByID: {
            value(id) {
                return native.objects().find((object) => object.id === id) ?? null;
            },
        },
    });

    /// `new alt.Object(...)` — так предметы и ставят в alt:V.
    ///
    /// Посредником над классом ядра по той же причине, что и у машины:
    /// `instanceof alt.Object` обязан узнавать предметы, пришедшие из ядра.
    const ConstructibleObject = new Proxy(WorldObject, {
        construct(_target, args) {
            const [model] = args;
            const hashed = typeof model === 'string' ? shared.hash(model) : model;

            const position = args.length >= 4
                ? new shared.Vector3(args[1], args[2], args[3])
                : new shared.Vector3(args[1]);

            // Поворот у alt:V в градусах — в отличие от машины, где радианы.
            // Расхождение не наше: так это у него и сделано.
            const rotation = args.length >= 7
                ? new shared.Vector3(args[4], args[5], args[6])
                : (args[2] === undefined ? new shared.Vector3(0, 0, 0)
                                         : new shared.Vector3(args[2]));

            const object = native.createObject(hashed, position, rotation);

            if (object === null) {
                throw new Error(`alt.Object: предмет модели ${model} не поставился`);
            }

            return object;
        },
    });

    // --- Таймеры -------------------------------------------------------------

    /// Таймеры alt:V отличаются от родных для JS одним: они возвращают число.
    ///
    /// Не украшение. Node возвращает объект Timeout, а ресурсы alt:V складывают
    /// номера таймеров в базу, шлют их странице интерфейса и сравнивают между
    /// собой. Объект на этом месте ломает всё перечисленное молча.
    const timers = new Map();
    let nextTimer = 1;

    function keepTimer(handle) {
        const id = nextTimer++;
        timers.set(id, handle);
        return id;
    }

    function dropTimer(id) {
        const handle = timers.get(id);

        if (handle !== undefined) {
            clearTimeout(handle);
            clearInterval(handle);
            timers.delete(id);
        }
    }

    const server = {
        // Общая часть — целиком.
        ...shared,

        version: native.version,
        branch: 'release',
        get resourceName() { return native.resourceName; },
        get defaultDimension() { return 0; },
        get globalDimension() { return -2147483648; },

        Player,
        Vehicle: ConstructibleVehicle,

        on,
        once,
        off,
        emit,

        /// Объявить событие только своему ресурсу, минуя ядро.
        ///
        /// Наружу не отдаётся: это не часть API alt:V, а внутренний ход для
        /// alt_objects.js. Нужен ему затем, что зона — обычный объект JavaScript
        /// внутри одного изолята, и через ядро она пройти не может: доводы
        /// укладываются в JSON, и с той стороны от зоны остаётся безымянный
        /// слепок. Ресурс же сравнивает её через `===` со своей — так написаны
        /// все режимы, — и сравнение это не сошлось бы никогда.
        ///
        /// Слышат событие оттого только подписчики своего ресурса, и иначе быть
        /// не может: у чужого ресурса этой зоны нет вовсе.
        fireLocal: fire,
        onClient,
        offClient,
        emitClient,
        emitAllClients,
        onRpc,
        offRpc,
        emitRpc: (player, name, ...args) => callClient(player, name, args),
        emitClientRaw: (target, name, payload) => target.emitRaw(name, payload),

        log,
        logWarning,
        logError,
        logDebug: log,

        setTimeout: (handler, delay) => keepTimer(setTimeout(handler, delay)),
        setInterval: (handler, delay) => keepTimer(setInterval(handler, delay)),
        clearTimeout: dropTimer,
        clearInterval: dropTimer,
        clearTimer: dropTimer,
        nextTick: (handler) => keepTimer(setTimeout(handler, 0)),
        everyTick: (handler) => keepTimer(setInterval(handler, 1000 / native.tickRate)),
        clearNextTick: dropTimer,
        clearEveryTick: dropTimer,

        createVehicle,

        /// Мир: часы и погода. Принадлежат серверу целиком (см. CLAUDE.md).
        setWeather: (weather) => native.setWeather(String(weather)),
        setTime: (hour, minute) => native.setTime(hour, minute),

        /// Взрыв. Доводы — как у натива игры ADD_EXPLOSION и в том же порядке.
        ///
        /// Серверного взрыва нет ни у alt:V, ни у RAGE MP: там его заводит
        /// клиент нативом, у каждого свой. У нас так нельзя — взрыв это урон,
        /// звук и толчок всему вокруг, и посчитанный каждым у себя он
        /// разошёлся бы у двоих зрителей. Поэтому он серверный, как погода.
        ///
        /// Образца для имени взять неоткуда, и оно взято у самого натива:
        /// знающий натив напишет вызов верно с первого раза.
        ///
        ///     alt.addExplosion(pos, 4);                       // ракета
        ///     alt.addExplosion(pos, 7, { scale: 2, shake: 1 }) // машина, вдвое
        addExplosion: (position, kind, options) =>
            native.addExplosion(position, kind ?? 0, options),

        /// Строка в чат всем. У alt:V своего чата нет, у oxyMP есть.
        broadcast: (text) => native.broadcast(String(text)),

        // Того, чего ещё нет. Отказом, а не тишиной: см. absent().
        //
        // Метки, зоны, чекпоинты, маркеры и голосовые каналы кладёт сюда
        // alt_objects.js — он исполняется следом и заменяет их настоящими.
        Ped: ConstructiblePed,
        Object: ConstructibleObject,

        /// Сетевой предмет — тот, которым игроки могут двигать друг у друга.
        /// У нас предмет стоит: подвинуть его может лишь тот, кто поставил.
        NetworkObject: absent('alt.NetworkObject'),
        VirtualEntity: absent('alt.VirtualEntity'),
        VirtualEntityGroup: absent('alt.VirtualEntityGroup'),
        // На сервере их нет и у самого alt:V — они клиентские. Серверу же
        // доступен весь Node: `fetch`, `http`, `ws` и что угодно из npm.
        HttpClient: absent('alt.HttpClient (он клиентский; на сервере есть fetch)'),
        WebSocketClient: absent('alt.WebSocketClient (он клиентский)'),
        Resource: ScriptResource,
        restartResource: absent('alt.restartResource'),
        startResource: absent('alt.startResource'),
        stopResource: absent('alt.stopResource'),
        getServerConfig: absent('alt.getServerConfig'),
        /// Кто и что стоит рядом с точкой.
        ///
        /// Считается перебором по реестрам, а не по своему указателю: своего
        /// указателя у нас нет, а реестры сервер и так держит целиком. Игроков в
        /// сессии сотни, а не миллионы, и перебор их дешевле всякого дерева,
        /// которое пришлось бы держать в согласии с миром.
        ///
        /// `allowedTypes` — набор `alt.BaseObjectFilterType`, складываемый
        /// побитово. Не названный, он означает «все роды»: так же толкует его и
        /// alt:V.
        getEntitiesInRange: (position, range, dimension, allowedTypes) =>
            entitiesNear(position, range, dimension, allowedTypes),

        getEntitiesInDimension: (dimension, allowedTypes) =>
            entitiesNear(null, 0, dimension, allowedTypes),

        getClosestEntities: (position, range, dimension, limit, allowedTypes) => {
            const at = new shared.Vector3(position);

            // Сортируется копия, и сортируется по квадрату расстояния: корень
            // здесь не нужен никому — порядок у квадратов тот же самый.
            const found = entitiesNear(position, range, dimension, allowedTypes)
                .map((entity) => [entity, at.distanceToSquared(entity.pos)])
                .sort((left, right) => left[1] - right[1])
                .map(([entity]) => entity);

            const many = Number(limit);
            return Number.isFinite(many) && many >= 0 ? found.slice(0, many) : found;
        },

        /// Ближайший игрок и ближайшая машина.
        ///
        /// Доводом объект `{ pos, range }`, а не два числа, — так это объявлено
        /// у alt:V. Пусто — рядом никого нет.
        getClosestPlayer: (options) => nearestOf(alt.enums.BaseObjectFilterType.Player, options),
        getClosestVehicle: (options) => nearestOf(alt.enums.BaseObjectFilterType.Vehicle, options),

        /// Поднятые ресурсы: все и по имени.
        getAllResources: () => ScriptResource.all,
        hasResource: (name) => ScriptResource.exists(String(name)),

        /// Метаданные сессии, не привязанные ни к какой сущности.
        ///
        /// События у них свои — `globalSyncedMetaChange` и `globalMetaChange`, —
        /// и сущности в доводах нет вовсе: её у общих метаданных не бывает.
        /// Различать их с сущностными обязательно: режим, подписанный на оба,
        /// считал бы всякое изменение дважды.
        setSyncedMeta: (key, value) => {
            const store = syncedFor('global', 0);
            const was = store.get(key);

            store.set(key, value);
            publishSynced('global', 0, key, value);
            server.fireLocal('globalSyncedMetaChange', [key, value, was]);
        },
        getSyncedMeta: (key) => syncedFor('global', 0).get(key),
        hasSyncedMeta: (key) => syncedFor('global', 0).has(key),
        getSyncedMetaKeys: () => [...syncedFor('global', 0).keys()],
        deleteSyncedMeta: (key) => {
            const store = syncedFor('global', 0);
            const was = store.get(key);

            store.delete(key);
            publishSynced('global', 0, key, undefined);
            server.fireLocal('globalSyncedMetaChange', [key, undefined, was]);
        },

        /// Общие метаданные, не покидающие сервер.
        setMeta: (key, value) => {
            const store = metaFor('global', 0);
            const was = store.get(key);

            store.set(key, value);
            server.fireLocal('globalMetaChange', [key, value, was]);
        },
        getMeta: (key) => metaFor('global', 0).get(key),
        hasMeta: (key) => metaFor('global', 0).has(key),
        getMetaKeys: () => [...metaFor('global', 0).keys()],
        deleteMeta: (key) => {
            const store = metaFor('global', 0);
            const was = store.get(key);

            store.delete(key);
            server.fireLocal('globalMetaChange', [key, undefined, was]);
        },
    };

    alt.server = server;
})(globalThis.__oxympAlt);
