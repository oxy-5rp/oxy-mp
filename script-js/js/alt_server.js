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
            // Одним мостиком приходят двое: события самой сессии — их ядро
            // зовёт с готовыми доводами — и события, объявленные ресурсами, у
            // которых довод один, строкой. Различаются они по числу доводов, и
            // спутать их нельзя: у объявленного ресурсом довод ровно один и он
            // всегда строка, а у событий сессии первым идёт сущность.
            native.on(name, (...args) => {
                if (args.length === 1 && typeof args[0] === 'string') {
                    fire(name, decodeLocal(args[0]));
                    return;
                }

                fire(name, args);
            });
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

    /// Рассылает изменение всем, кто его увидит.
    ///
    /// Всем, а не одному владельцу: `syncedMeta` на то и synced, что её видит
    /// каждый. Клиент, знающий чужое имя над головой, узнаёт его отсюда.
    function publishSynced(kind, id, key, value) {
        const payload = encodeArgs([kind, id, key, value === undefined ? null : value]);

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
                native.emitClient(player, kSyncedMetaEvent, encodeArgs([kind, id, key, value]));
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
            setMeta(key, value) { metaFor(kind, this.id).set(key, value); },
            getMeta(key) { return metaFor(kind, this.id).get(key); },
            hasMeta(key) { return metaFor(kind, this.id).has(key); },
            deleteMeta(key) { metaFor(kind, this.id).delete(key); },
            getMetaKeys() { return [...metaFor(kind, this.id).keys()]; },

            setSyncedMeta(key, value) {
                syncedFor(kind, this.id).set(key, value);
                publishSynced(kind, this.id, key, value);
            },
            getSyncedMeta(key) { return syncedFor(kind, this.id).get(key); },
            hasSyncedMeta(key) { return syncedFor(kind, this.id).has(key); },
            deleteSyncedMeta(key) {
                syncedFor(kind, this.id).delete(key);
                publishSynced(kind, this.id, key, undefined);
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
                syncedFor(kind, this.id).set(key, value);
                publishSynced(kind, this.id, key, value);
            },
            getStreamSyncedMeta(key) { return syncedFor(kind, this.id).get(key); },
        });
    }

    // --- Сущности ------------------------------------------------------------

    const Player = native.Player;
    const Vehicle = native.Vehicle;
    const WorldObject = native.Object;

    /// Угол поворота у alt:V — вектор в радианах, у oxyMP — один угол в градусах.
    ///
    /// Разница не косметическая. Персонаж в игре поворачивается вокруг одной оси,
    /// и второй с третьей у него всегда нули — поэтому потери здесь нет. А вот
    /// единицы теряются молча: ресурс, положивший `rot.z` в натив, ждущий
    /// радианы, получит поворот в шестьдесят раз меньше нужного и будет искать
    /// причину в нативе.
    function headingToRot(heading) {
        return new shared.Vector3(0, 0, heading * (Math.PI / 180));
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
        /// Слой мира, в котором игрок находится. Есть у ядра и работает.
        /// Как игрока зовут. У alt:V это `name`, и оно уже есть в ядре.
        ip: { get: absent('player.ip') },
        ping: { get: absent('player.ping') },
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
        /// А вот `setDlcClothes` — нет, и разница не в лени. У alt:V она берёт
        /// вещь из набора DLC, названного хешем, и вещи эти лежат в других
        /// файлах игры; наш протокол внешности такого поля не знает вовсе.
        /// Промолчать было бы хуже: ресурс решил бы, что одел человека.
        setDlcClothes: {
            value: unperformed('player.setDlcClothes',
                               'одежда из наборов DLC протоколом внешности не описана'),
        },
        setWeather: { value: unperformed('player.setWeather', 'погода у нас общая на сессию — см. alt.setWeather') },
        setDateTime: { value: unperformed('player.setDateTime', 'часы у нас общие на сессию — см. alt.setTime') },
        removeWeapon: { value: unperformed('player.removeWeapon', 'отобрать одно оружие нельзя, можно всё — clearWeapons') },
        /// Посадить игрока в машину ядро умеет: просьбой ему самому.
        ///
        /// У alt:V место называется вторым доводом, и минус единица означает
        /// «за руль». У нас так же — нумерация взята у самой игры.
        addWeaponComponent: { value: unperformed('player.addWeaponComponent', 'обвесы оружия не передаются') },
        playAnimation: { value: unperformed('player.playAnimation', 'движения по слову сервера не проигрываются') },
        attachTo: { value: unperformed('player.attachTo', 'привязка сущностей друг к другу не передаётся') },
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

    Object.defineProperties(Vehicle.prototype, {
        pos: {
            get() { return new shared.Vector3(this.position); },
            set(value) { this.teleport(new shared.Vector3(value)); },
        },
        rot: {
            get() { return new shared.Vector3(this.rotation); },
            set: unperformed('vehicle.rot', 'повернуть машину сервер пока не умеет'),
        },
        /// Кто за рулём. У alt:V это `driver`, у ядра — `owner` (ведущий).
        ///
        /// Имена разные, потому что и смысл не совпадает целиком: ведущий у oxyMP
        /// — тот, кто считает физику машины, и это почти всегда водитель, но
        /// машина без водителя ведущего не теряет. Здесь отдаётся то же, что и
        /// `owner`, и расхождение стоит помнить.
        driver: { get() { return this.owner; } },
        /// Слой мира, в котором машина стоит. Есть у ядра и работает.
        toString: {
            value() { return `Vehicle{ id: ${this.id} }`; },
        },
        setMod: { value: unperformed('vehicle.setMod', 'обвесы машины сервером не меняются') },

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

    addMeta(Player, 'player');
    addMeta(Vehicle, 'vehicle');

    // Метаданные вышедшего игрока убираются последним подписчиком, а не первым:
    // событие объявляется до уборки нарочно (см. CLAUDE.md), и ресурс вправе
    // прочесть их в своём обработчике.
    on('playerConnect', (player) => sendSyncedSnapshot(player));
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

    Object.defineProperties(WorldObject.prototype, {
        pos: {
            get() { return new shared.Vector3(this.position); },
            set: unperformed('object.pos', 'переставить предмет сервер пока не умеет'),
        },
        rot: {
            get() { return new shared.Vector3(this.rotation); },
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

        /// Строка в чат всем. У alt:V своего чата нет, у oxyMP есть.
        broadcast: (text) => native.broadcast(String(text)),

        // Того, чего ещё нет. Отказом, а не тишиной: см. absent().
        //
        // Метки, зоны, чекпоинты, маркеры и голосовые каналы кладёт сюда
        // alt_objects.js — он исполняется следом и заменяет их настоящими.
        Ped: absent('alt.Ped'),
        Object: ConstructibleObject,

        /// Сетевой предмет — тот, которым игроки могут двигать друг у друга.
        /// У нас предмет стоит: подвинуть его может лишь тот, кто поставил.
        NetworkObject: absent('alt.NetworkObject'),
        VirtualEntity: absent('alt.VirtualEntity'),
        VirtualEntityGroup: absent('alt.VirtualEntityGroup'),
        HttpClient: absent('alt.HttpClient'),
        WebSocketClient: absent('alt.WebSocketClient'),
        Resource: absent('alt.Resource'),
        restartResource: absent('alt.restartResource'),
        startResource: absent('alt.startResource'),
        stopResource: absent('alt.stopResource'),
        getServerConfig: absent('alt.getServerConfig'),
        /// Метаданные сессии, не привязанные ни к какой сущности.
        setSyncedMeta: (key, value) => {
            syncedFor('global', 0).set(key, value);
            publishSynced('global', 0, key, value);
        },
        getSyncedMeta: (key) => syncedFor('global', 0).get(key),
        deleteSyncedMeta: (key) => {
            syncedFor('global', 0).delete(key);
            publishSynced('global', 0, key, undefined);
        },
    };

    alt.server = server;
})(globalThis.__oxympAlt);
