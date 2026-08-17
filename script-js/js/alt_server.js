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
            native.on(name, (...args) => fire(name, args));
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

    /// Событие внутри сервера. Сеть не задействована.
    ///
    /// Только внутри этого ресурса, и это отличие от alt:V, о котором нужно
    /// знать: там `alt.emit` слышат все ресурсы сразу. Здесь у каждого ресурса
    /// свой изолят и свой список подписок, и события между ресурсами ещё не
    /// ходят — см. docs/scripting.md, раздел незакрытого.
    function emit(name, ...args) {
        fire(name, args);
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
    }

    /// Примешивает набор meta-действий классу сущности.
    function addMeta(target, kind) {
        Object.defineProperties(target.prototype, {
            setMeta: {
                value(key, value) { metaFor(kind, this.id).set(key, value); },
            },
            getMeta: {
                value(key) { return metaFor(kind, this.id).get(key); },
            },
            hasMeta: {
                value(key) { return metaFor(kind, this.id).has(key); },
            },
            deleteMeta: {
                value(key) { metaFor(kind, this.id).delete(key); },
            },
            getMetaKeys: {
                value() { return [...metaFor(kind, this.id).keys()]; },
            },
        });
    }

    // --- Сущности ------------------------------------------------------------

    const Player = native.Player;
    const Vehicle = native.Vehicle;

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
        /// alt:V зовёт это `model`; у нас модель персонажа ведёт клиент.
        model: {
            get() { return 0; },
            set: absent('player.model'),
        },
        dimension: {
            get() { return 0; },
            set: absent('player.dimension'),
        },
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
        setSyncedMeta: { value: absent('player.setSyncedMeta') },
        getSyncedMeta: { value: absent('player.getSyncedMeta') },
        setStreamSyncedMeta: { value: absent('player.setStreamSyncedMeta') },
        setClothes: { value: absent('player.setClothes') },
        setDlcClothes: { value: absent('player.setDlcClothes') },
        setProp: { value: absent('player.setProp') },
        clearProp: { value: absent('player.clearProp') },
        setWeather: { value: absent('player.setWeather') },
        setDateTime: { value: absent('player.setDateTime') },
        removeWeapon: { value: absent('player.removeWeapon') },
        setIntoVehicle: { value: absent('player.setIntoVehicle') },
        addWeaponComponent: { value: absent('player.addWeaponComponent') },
        playAnimation: { value: absent('player.playAnimation') },
        attachTo: { value: absent('player.attachTo') },
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
            set: absent('vehicle.pos'),
        },
        rot: {
            get() { return new shared.Vector3(this.rotation); },
            set: absent('vehicle.rot'),
        },
        /// Кто за рулём. У alt:V это `driver`, у ядра — `owner` (ведущий).
        ///
        /// Имена разные, потому что и смысл не совпадает целиком: ведущий у oxyMP
        /// — тот, кто считает физику машины, и это почти всегда водитель, но
        /// машина без водителя ведущего не теряет. Здесь отдаётся то же, что и
        /// `owner`, и расхождение стоит помнить.
        driver: { get() { return this.owner; } },
        dimension: {
            get() { return 0; },
            set: absent('vehicle.dimension'),
        },
        toString: {
            value() { return `Vehicle{ id: ${this.id} }`; },
        },
        setSyncedMeta: { value: absent('vehicle.setSyncedMeta') },
        getSyncedMeta: { value: absent('vehicle.getSyncedMeta') },
        setMod: { value: absent('vehicle.setMod') },
        repair: { value: absent('vehicle.repair') },
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

        Player,
        Vehicle,

        on,
        once,
        off,
        emit,
        onClient,
        offClient,
        emitClient,
        emitAllClients,
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
        Blip: absent('alt.Blip'),
        PointBlip: absent('alt.PointBlip'),
        Checkpoint: absent('alt.Checkpoint'),
        Colshape: absent('alt.Colshape'),
        ColshapeSphere: absent('alt.ColshapeSphere'),
        ColshapeCylinder: absent('alt.ColshapeCylinder'),
        ColshapeCuboid: absent('alt.ColshapeCuboid'),
        ColshapePolygon: absent('alt.ColshapePolygon'),
        Ped: absent('alt.Ped'),
        Object: absent('alt.Object'),
        NetworkObject: absent('alt.NetworkObject'),
        VirtualEntity: absent('alt.VirtualEntity'),
        VirtualEntityGroup: absent('alt.VirtualEntityGroup'),
        VoiceChannel: absent('alt.VoiceChannel'),
        Marker: absent('alt.Marker'),
        HttpClient: absent('alt.HttpClient'),
        WebSocketClient: absent('alt.WebSocketClient'),
        Resource: absent('alt.Resource'),
        restartResource: absent('alt.restartResource'),
        startResource: absent('alt.startResource'),
        stopResource: absent('alt.stopResource'),
        getServerConfig: absent('alt.getServerConfig'),
        setSyncedMeta: absent('alt.setSyncedMeta'),
        getSyncedMeta: absent('alt.getSyncedMeta'),
    };

    alt.server = server;
})(globalThis.__oxympAlt);
