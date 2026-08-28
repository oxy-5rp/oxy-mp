// Клиентская часть API alt:V.
//
// Собирается поверх `__oxympAlt.native` — тонкого мостика в клиент, — и поверх
// общей части (`alt_shared.js`), которая здесь та же самая, что и на сервере.
// Не похожая, а буквально тот же файл: `alt-shared` потому и зовётся общей, что
// разойдись в ней `Vector3` или `hash` хоть в мелочи, ресурс, считающий
// расстояние на клиенте и на сервере, получал бы два разных ответа.
//
// Правило то же, что и на сервере: молчаливых заглушек нет. Чего ещё нет —
// отказывает вслух.

'use strict';

(function build(alt) {
    const native = alt.native;
    const shared = alt.shared;

    function absent(what) {
        return function () {
            throw new Error(`${what}: в oxyMP этого ещё нет`);
        };
    }

    /// О чём уже говорили. По поводу, а не по вызову.
    const warned = new Set();

    function warnOnce(what, why) {
        if (warned.has(what)) {
            return;
        }

        warned.add(what);
        native.logWarning(`${what}: ${why}`);
    }

    /// Распоряжение, которого мы не умеем исполнить.
    ///
    /// Говорит один раз и возвращается, а не бросает. Разница с `absent` не в
    /// громкости, а в цене: распоряжение стоит посреди чужого обработчика — в
    /// такте, в обработчике клавиши, — и брошенное отсюда исключение уносит с
    /// собой всё, что шло следом. Вопросов это не касается: у вопроса без ответа
    /// тишина — ложь, и для них `absent` остаётся.
    ///
    /// answer — что вернуть вместо ответа. Пустая строка и false выбраны так,
    /// чтобы зовущий увидел «не вышло», а не принял ложь за правду.
    function unperformed(what, why, answer) {
        return function () {
            warnOnce(what, why);
            return answer;
        };
    }

    // --- События -------------------------------------------------------------

    /// Подписки ресурса, по имени.
    ///
    /// Свой список поверх мостика — ровно как на сервере. Мостик умеет одно,
    /// «позвать подписанных на имя»; alt:V обещает `off` и `once`.
    const listeners = new Map();
    const bridged = new Set();

    function listenersFor(name) {
        let found = listeners.get(name);

        if (found === undefined) {
            found = [];
            listeners.set(name, found);
        }

        return found;
    }

    /// Разбирает нагрузку события в набор доводов.
    ///
    /// JSON, а не MValue, и это согласовано с сервером: `alt_server.js`
    /// укладывает доводы ровно так же. Формат один на обе стороны — иначе не
    /// сошлось бы ничего.
    function decodeArgs(payload) {
        if (typeof payload !== 'string' || payload.length === 0) {
            return [];
        }

        try {
            const parsed = JSON.parse(payload);
            return Array.isArray(parsed) ? parsed : [parsed];
        } catch {
            // Разобрать не вышло — отдаём как есть: другая сторона могла послать
            // простую строку, и терять её из-за того, что она не JSON, незачем.
            return [payload];
        }
    }

    function encodeArgs(args) {
        return JSON.stringify(args);
    }

    function fire(name, args) {
        // Копия списка нарочно: обработчик волен отписаться прямо отсюда.
        const called = listenersFor(name).slice();

        for (const entry of called) {
            if (entry.once) {
                off(name, entry.handler);
            }

            try {
                entry.handler(...args);
            } catch (failure) {
                // Упавший обработчик не уносит ни остальных, ни игру.
                logError(`ошибка в обработчике «${name}»:`, failure?.stack ?? failure);
            }
        }
    }

    /// Ставит мостик в клиент, если его ещё нет.
    ///
    /// `bridgeName` — то, под каким именем событие приходит снизу; `name` — то,
    /// под каким его слушает ресурс. Они расходятся у событий сервера и окон:
    /// клиент помечает их приставкой, чтобы своё имя ресурса не столкнулось с
    /// чужим.
    function bridge(name, bridgeName, unpack) {
        const key = bridgeName ?? name;

        if (bridged.has(key)) {
            return;
        }

        bridged.add(key);

        native.on(key, (payload) => fire(name, unpack ? unpack(payload) : decodeArgs(payload)));
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

    /// Событие внутри клиента. Сеть не задействована.
    function emit(name, ...args) {
        fire(name, args);
    }

    // --- Разговор с сервером -------------------------------------------------

    function onServer(name, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.onServer ждёт имя события и обработчик');
        }

        const local = `!server:${name}`;

        listenersFor(local).push({ handler, once: false });
        bridge(local, `server:${name}`);
    }

    function offServer(name, handler) {
        off(`!server:${name}`, handler);
    }

    function emitServer(name, ...args) {
        native.emitServer(String(name), encodeArgs(args));
    }

    function emitServerRaw(name, payload) {
        native.emitServer(String(name), String(payload));
    }

    // --- Вызовы с ответом (RPC) ----------------------------------------------
    //
    // Зеркало серверного (script-js/js/alt_server.js): те же служебные имена, те
    // же правила. Разъедутся — вызовы перестанут доходить, и без единой жалобы:
    // событие просто никто не ждёт.

    /// Сколько ждать ответа, прежде чем считать вызов пропавшим.
    const kRpcTimeout = 5000;

    const kRpcCall = '__oxymp:rpc:call';
    const kRpcAnswer = '__oxymp:rpc:answer';

    /// Обработчики вызовов по имени. По одному: ответ может быть только один.
    const answerers = new Map();

    /// Свои вызовы, ожидающие ответа.
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

    /// Отвечает на вызов, пришедший с сервера.
    function answerRpc(id, name, args) {
        const handler = answerers.get(name);

        const reply = (ok, value) =>
            native.emitServer(kRpcAnswer, encodeArgs([id, ok, ok ? value : String(value)]));

        if (handler === undefined) {
            reply(false, `на вызов «${name}» никто не отвечает`);
            return;
        }

        // Обработчик волен вернуть и обещание, и готовое значение.
        Promise.resolve()
            .then(() => handler(...args))
            .then((value) => reply(true, value === undefined ? null : value),
                  (failure) => {
                      logError(`ошибка в обработчике вызова «${name}»:`, failure);
                      reply(false, failure?.message ?? failure);
                  });
    }

    /// Делает вызов серверу и ждёт ответа.
    function emitRpc(name, ...args) {
        return new Promise((resolve, reject) => {
            const id = nextCall++;

            const timer = setTimeout(() => {
                pending.delete(id);
                reject(new Error(`вызов «${name}» остался без ответа за ${kRpcTimeout} мс`));
            }, kRpcTimeout);

            pending.set(id, { resolve, reject, timer });

            native.emitServer(kRpcCall, encodeArgs([id, name, args]));
        });
    }

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

    bridged.add(`server:${kRpcCall}`);
    native.on(`server:${kRpcCall}`, (payload) => {
        const [id, name, args] = decodeArgs(payload);
        answerRpc(id, name, Array.isArray(args) ? args : []);
    });

    bridged.add(`server:${kRpcAnswer}`);
    native.on(`server:${kRpcAnswer}`, (payload) => {
        const [id, ok, value] = decodeArgs(payload);
        takeAnswer(id, ok, value);
    });

    // --- Журнал --------------------------------------------------------------

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

    const log = (...args) => native.log(...args.map(render));
    const logWarning = (...args) => native.logWarning(...args.map(render));
    const logError = (...args) => native.logError(...args.map(render));

    // --- Метаданные, присланные сервером -------------------------------------

    /// Что сервер о ком рассказал, по роду и номеру.
    ///
    /// Только на чтение: `syncedMeta` принадлежит серверу, и клиент, поменявший
    /// её у себя, обманул бы сам себя — до следующей рассылки, которая вернула
    /// бы серверное значение.
    const synced = new Map();

    /// То же служебное имя, что и на сервере. Разъедутся — метаданные
    /// перестанут доходить, и без единой жалобы: событие просто никто не ждёт.
    const kSyncedMetaEvent = '__oxymp:meta';

    /// То же имя, что и на сервере, и по той же причине: разъедутся — личные
    /// метаданные перестанут доходить, и без единой жалобы.
    const kLocalMetaEvent = '__oxymp:localmeta';

    function syncedFor(kind, id) {
        const key = `${kind}:${id}`;
        let found = synced.get(key);

        if (found === undefined) {
            found = new Map();
            synced.set(key, found);
        }

        return found;
    }

    /// Сущность сессии по роду и номеру, какими их называет сервер.
    ///
    /// Берётся у `alt.entities` на каждый вызов, а не через `entities` ниже по
    /// файлу: тот объявлен позже, и обработчик, стоящий выше, полагался бы на
    /// порядок исполнения внутри файла.
    ///
    /// Пусто — сущности у клиента нет, и причин тому две, обе законные: род,
    /// которого клиент по номеру сессии не знает (предмет, кукла), либо игрок, о
    /// котором сервер ещё не объявил. Метаданные при этом всё равно
    /// запоминаются: `getSyncedMeta` отдаст их, когда сущность появится.
    function entityOf(kind, id) {
        if (kind === 'player') {
            return alt.entities.playerById(id);
        }
        if (kind === 'vehicle') {
            return alt.entities.vehicleById(id);
        }

        return null;
    }

    native.on(`server:${kSyncedMetaEvent}`, (payload) => {
        const [kind, id, key, value, streamed] = decodeArgs(payload);

        // Прежнее значение снимается до записи: alt:V отдаёт его четвёртым
        // доводом, и после присваивания взять его будет уже неоткуда.
        const previous = syncedFor(kind, id).get(key);
        const fresh = value === null ? undefined : value;

        if (value === null) {
            syncedFor(kind, id).delete(key);
        } else {
            syncedFor(kind, id).set(key, value);
        }

        // Ресурсам сообщается так же, как в alt:V: изменение — это событие, а не
        // только новое значение. Режим, рисующий имя над головой, перерисовывает
        // его по нему, а не опросом каждый кадр.
        //
        // Доводы сверены с `@altv/types-client`, а не вспомянуты:
        // `syncedMetaChange(entity, key, value, oldValue)`, а у общих —
        // `globalSyncedMetaChange(key, value, oldValue)` без сущности вовсе.
        // Прежде отсюда уходили род и номер вместо сущности, и обработчик,
        // написанный под alt:V, получал число там, где ждал игрока.
        if (kind === 'global') {
            fire('globalSyncedMetaChange', [key, fresh, previous]);
            return;
        }

        const entity = entityOf(kind, id);
        if (entity === null) {
            return;
        }

        fire(streamed === true ? 'streamSyncedMetaChange' : 'syncedMetaChange',
             [entity, key, fresh, previous]);
    });

    bridged.add(`server:${kSyncedMetaEvent}`);

    // --- Метаданные, назначенные лично нам ------------------------------------
    //
    // Отдельно от synced и складом попроще: они принадлежат не сущности, а тому,
    // кто за ней сидит, и у alt:V читаются модульными функциями, а не у объекта.
    const localMeta = new Map();

    native.on(`server:${kLocalMetaEvent}`, (payload) => {
        const [key, value] = decodeArgs(payload);
        const previous = localMeta.get(key);

        if (value === null) {
            localMeta.delete(key);
        } else {
            localMeta.set(key, value);
        }

        fire('localMetaChange', [key, value === null ? undefined : value, previous]);
    });

    bridged.add(`server:${kLocalMetaEvent}`);

    // --- Свой номер в сессии --------------------------------------------------

    // Слой сущностей читает метаданные и свой номер через эти окошки: он
    // исполняется раньше и о них ещё не знает.
    alt.readSyncedMeta = (kind, id, key) => syncedFor(kind, id).get(key);
    alt.readSyncedMetaKeys = (kind, id) => [...syncedFor(kind, id).keys()];

    /// Кем нас числит сервер. −1 — ещё не принял.
    ///
    /// Спрашивается у клиента на каждое обращение, а не запоминается. Так надо:
    /// ресурс читает его первыми же строками, ещё до первого события, а при
    /// переподключении сервер выдаёт другой — запомненный однажды устарел бы
    /// молча.
    alt.selfId = () => native.selfId();

    // --- Окно интерфейса -----------------------------------------------------

    /// Страница поверх кадра игры.
    ///
    /// То самое, ради чего клиентская машина и заводилась: игровой режим рисует
    /// свой интерфейс не нативами игры, а обычной веб-страницей, и говорит с ней
    /// событиями.
    ///
    /// Адрес вида `http://resource/client/ui/index.html` — так его пишет alt:V,
    /// и разбирает его клиент: `resource` здесь не имя узла в сети, а признак
    /// того, что файл нужно взять из раздачи сервера.
    class WebView {
        #id = 0;
        #handlers = new Map();

        constructor(url, isOverlay) {
            this.url = String(url);

            this.#id = native.createWebView(this.url);

            if (this.#id === 0) {
                throw new Error(`окно «${this.url}» не завелось`);
            }

            // alt:V вторым доводом принимает и признак наложения, и точку с
            // размером. Признак — единственное, что здесь пока значимо.
            this.isOverlay = isOverlay === true;

            this.#visible = true;
            this.#focused = false;

            WebView.all.push(this);
        }

        static all = [];

        get id() { return this.#id; }

        get valid() { return this.#id !== 0; }

        #visible = true;
        #focused = false;

        get isVisible() { return this.#visible; }

        set isVisible(value) {
            this.#visible = Boolean(value);
            native.setWebViewVisible(this.#id, this.#visible);
        }

        get focused() { return this.#focused; }

        set focused(value) {
            this.#focused = Boolean(value);
            native.setWebViewFocused(this.#id, this.#focused);
        }

        /// Событие странице.
        emit(name, ...args) {
            native.emitWebView(this.#id, String(name), encodeArgs(args));
        }

        emitRaw(name, payload) {
            native.emitWebView(this.#id, String(name), String(payload));
        }

        /// Подписка на событие от страницы.
        on(name, handler) {
            if (typeof handler !== 'function') {
                throw new TypeError('webView.on ждёт имя события и обработчик');
            }

            // Номер окна входит в имя: клиент помечает им событие, чтобы два
            // окна с одинаковым именем события не перепутались.
            const local = `!view:${this.#id}:${name}`;

            listenersFor(local).push({ handler, once: false });
            bridge(local, `view:${this.#id}:${name}`);

            this.#handlers.set(handler, local);
        }

        off(name, handler) {
            off(`!view:${this.#id}:${name}`, handler);
            this.#handlers.delete(handler);
        }

        destroy() {
            if (this.#id === 0) {
                return;
            }

            native.destroyWebView(this.#id);

            const at = WebView.all.indexOf(this);
            if (at >= 0) {
                WebView.all.splice(at, 1);
            }

            this.#id = 0;
        }

        // Того, чего ещё нет.
        focus() { this.focused = true; }
        unfocus() { this.focused = false; }
        setZIndex() { throw new Error('webView.setZIndex: в oxyMP этого ещё нет'); }
        setExtraHeader() { throw new Error('webView.setExtraHeader: в oxyMP этого ещё нет'); }
    }

    // --- Клавиши -------------------------------------------------------------

    /// Нажатые прямо сейчас, по коду.
    const held = new Set();

    // Клавиши мостит этот файл, а не общий bridge(), и потому их имена
    // объявляются уже занятыми.
    //
    // Без этого `alt.on('keydown', ...)` поставил бы второй мостик на то же имя:
    // один здешний, с проверкой удержания, другой общий — без неё. Обработчик
    // ресурса звался бы дважды на одно нажатие, и счётчик, увеличиваемый по
    // клавише, шёл бы через два.
    bridged.add('keydown');
    bridged.add('keyup');

    native.on('keydown', (payload) => {
        const key = Number(payload);

        // Повтор от удержания в alt:V событием не считается: `keydown` там
        // случается один раз на нажатие, а «держит ли» спрашивают через
        // isKeyDown. Без этого меню по клавише открывалось бы и закрывалось
        // тридцать раз в секунду.
        if (held.has(key)) {
            return;
        }

        held.add(key);
        fire('keydown', [key]);
    });

    native.on('keyup', (payload) => {
        const key = Number(payload);

        held.delete(key);
        fire('keyup', [key]);
    });

    const isKeyDown = (key) => held.has(Number(key));

    // --- Таймеры -------------------------------------------------------------

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

    /// Кадровые обработчики, по выданному номеру.
    ///
    /// Отдельно от таймеров, потому что снимаются иначе: таймер отменяют
    /// clearTimeout, а этот нужно убрать из списка подписчиков `render`. Общий
    /// склад привёл бы к тому, что clearEveryTick «срабатывал» бы, ничего не
    /// сняв, — а обработчик продолжал бы рисовать каждый кадр.
    const frameHandlers = new Map();

    function everyTick(handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.everyTick ждёт обработчик');
        }

        const id = nextTimer++;

        listenersFor('render').push({ handler, once: false });
        bridge('render');

        frameHandlers.set(id, handler);
        return id;
    }

    function clearEveryTick(id) {
        const handler = frameHandlers.get(id);

        if (handler === undefined) {
            return;
        }

        off('render', handler);
        frameHandlers.delete(id);
    }

    // --- Сборка модуля -------------------------------------------------------

    const entities = alt.entities;
    const net = alt.net;
    const objects = alt.objects;
    const extras = alt.extras;
    const locals = alt.locals;

    /// Свои выстрелы: `playerWeaponShoot`.
    ///
    /// У alt:V это событие приходит из перехвата внутри игры; у нас перехвата
    /// нет, и выстрел виден только по убыли патронов в обойме. Отсюда и то, чего
    /// это наблюдение **не** увидит, и молчать об этом нельзя:
    ///
    /// — удар кулаком и всякое оружие без патронов: убывать там нечему;
    /// — выстрел при бесконечных патронах: обойма не худеет.
    ///
    /// Зато очередь видна вся: за кадр обойма худеет на столько, сколько пуль
    /// вылетело, и событие объявляется столько же раз — так же, как у alt:V,
    /// где оно приходит на каждую пулю, а не на нажатие спуска.
    ///
    /// Считается это только пока кто-то слушает: три натива в кадр — недорого,
    /// но платить за них молча, когда событие никому не нужно, незачем.
    let ownWeapon = 0;
    let ownClip = -1;

    function watchOwnShots() {
        if (listenersFor('playerWeaponShoot').length === 0) {
            // Забываем и обойму: подписавшийся посреди боя не должен получить
            // залп за всё время, что его не слушали.
            ownClip = -1;
            return;
        }

        const ped = alt.natives.playerPedId();
        const weapon = alt.natives.getSelectedPedWeapon(ped);

        // Ответ приходит вторым местом: сам натив отвечает «есть ли обойма».
        const [, clip] = alt.natives.getAmmoInClip(ped, weapon, 0);

        if (weapon !== ownWeapon) {
            // Смена оружия обойму обнуляет не выстрелом, а сменой: считать
            // разницу между обоймами разных стволов бессмысленно.
            ownWeapon = weapon;
            ownClip = clip;
            return;
        }

        const выстрелов = ownClip < 0 ? 0 : ownClip - clip;

        ownClip = clip;

        if (выстрелов <= 0) {
            // Обойма выросла — это перезарядка, а не выстрел наоборот.
            return;
        }

        const total = alt.natives.getAmmoInPedWeapon(ped, weapon);

        for (let i = 0; i < выстрелов; ++i) {
            fire('playerWeaponShoot', [weapon, total, clip]);
        }
    }

    // Кадровая работа слоя: маркеры, курсор, запрет управления.
    //
    // Подписка своя, а не через everyTick: она принадлежит самому слою и не
    // должна сниматься вместе с обработчиками ресурса.
    bridged.add('render');
    native.on('render', () => {
        try {
            alt.drawFrame();
        } catch (failure) {
            logError('ошибка при отрисовке кадра:', failure?.stack ?? failure);
        }

        // Сверка тел — там же, где кадровая работа слоя, и по той же причине:
        // тело сущности заводит игра, когда ей вздумается, и узнать об этом
        // можно только сверкой. Своей подпиской, а не через everyTick: она
        // принадлежит слою и не должна сниматься вместе с обработчиками
        // ресурса.
        try {
            alt.entities.watchBodies();
        } catch (failure) {
            logError('ошибка при сверке появившихся сущностей:', failure?.stack ?? failure);
        }

        watchOwnShots();

        fire('render', []);
    });

    const client = {
        ...shared,

        isClient: true,
        isServer: false,

        get resourceName() { return native.resourceName; },

        // Метаданные, назначенные лично нам сервером. Модульными функциями, а
        // не у сущности: так объявлено у alt:V — принадлежат они не телу, а
        // тому, кто за ним сидит.
        getLocalMeta: (key) => localMeta.get(key),
        hasLocalMeta: (key) => localMeta.has(key),
        getLocalMetaKeys: () => [...localMeta.keys()],

        on,
        once,
        off,
        emit,
        onServer,
        offServer,
        emitServer,
        emitServerRaw,

        /// Объявить событие подписчикам своего ресурса, не выходя наружу.
        ///
        /// Тем и отличается от `emit`, что не уходит ни серверу, ни соседям:
        /// событием этим слой рассказывает ресурсу о том, что заметил сам, —
        /// появившееся тело, ушедшую сущность. Доводом идёт живой объект, а не
        /// слепок: ресурс сравнивает сущности через `===` со своими, и слепок с
        /// теми же числами не сошёлся бы никогда.
        ///
        /// Пара к `server.fireLocal` на сервере, и заведён по тому же поводу.
        fireLocal: (name, args) => fire(name, args ?? []),

        onRpc,
        offRpc,
        emitRpc,

        /// Так этот вызов зовут в части ресурсов — то же самое под другим именем.
        emitRpcServer: emitRpc,

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
        clearNextTick: dropTimer,

        /// everyTick — каждый кадр, а не по времени.
        ///
        /// Отличие от setInterval здесь существенное, в отличие от сервера: кадр
        /// игры плавает, и режим, рисующий что-нибудь поверх него, обязан делать
        /// это ровно раз в кадр, иначе нарисованное мигает.
        everyTick,
        clearEveryTick,

        WebView,
        isKeyDown,

        // Сущности: то, что стоит в мире. Собраны поверх нативов, а их номера
        // в сессии — поверх переводчика, который держит клиент.
        WorldObject: entities.WorldObject,
        /// `Entity.getByScriptID` отбора не делает: у alt:V он отвечает любой
        /// сущностью, какая по этому номеру нашлась.
        Entity: Object.defineProperties(entities.Entity, {
            getByScriptID: { value: entities.fromScriptID, configurable: true },
        }),
        Ped: Object.defineProperties(entities.Ped, {
            getByScriptID: {
                value: (handle) => {
                    const found = entities.fromScriptID(handle);

                    return found instanceof entities.Ped ? found : null;
                },
                configurable: true,
            },
        }),
        LocalPlayer: entities.LocalPlayer,

        /// Игроки сессии — все, о ком сказал сервер.
        ///
        /// `all` и `streamedIn` разведены так же, как в alt:V, и разница между
        /// ними существенная. `all` — это все игроки сессии, включая тех, до
        /// кого отсюда километр: у них есть номер, имя и метаданные, и список
        /// игроков рисуется по ним. `streamedIn` — только те, кому здесь нашлось
        /// тело; всё, что рисуется над головой или считает расстояние, обязано
        /// брать этот список, а не первый.
        ///
        /// Себя `all` отдаёт тем же объектом, что и `local`: режимы сравнивают
        /// найденного в списке с `alt.Player.local` через `===`.
        Player: Object.defineProperties(entities.Player, {
            local: { get: () => entities.local, configurable: true },
            all: { get: () => entities.players(), configurable: true },
            streamedIn: { get: () => entities.players(true), configurable: true },
            count: { get: () => entities.players().length, configurable: true },
            getByID: { value: (id) => entities.playerById(id), configurable: true },
            getByRemoteID: { value: (id) => entities.playerById(id), configurable: true },

            /// По дескриптору игры — то же, что `alt.fromScriptID`, но с
            /// отбором по роду: у alt:V `Player.getByScriptID` на машине
            /// отвечает null, а не машиной. Отдай мы её, режим позвал бы у
            /// машины `.name` и получил бы невнятную ошибку вместо честного
            /// «такого игрока нет».
            getByScriptID: {
                value: (handle) => {
                    const found = entities.fromScriptID(handle);

                    return found instanceof entities.Player ? found : null;
                },
                configurable: true,
            },
        }),

        /// Машины сессии — тех же правил.
        ///
        /// Здесь `all` и `streamedIn` расходятся ещё заметнее, чем у игроков:
        /// машины сервер держит все разом, а заводит их клиент по мере
        /// подгрузки моделей, и в первые секунды после входа тела нет почти ни у
        /// одной.
        Vehicle: Object.defineProperties(entities.Vehicle, {
            all: { get: () => entities.vehicles(), configurable: true },
            streamedIn: { get: () => entities.vehicles(true), configurable: true },
            count: { get: () => entities.vehicles().length, configurable: true },
            getByID: { value: (id) => entities.vehicleById(id), configurable: true },
            getByRemoteID: { value: (id) => entities.vehicleById(id), configurable: true },
            getByScriptID: {
                value: (handle) => {
                    const found = entities.fromScriptID(handle);

                    return found instanceof entities.Vehicle ? found : null;
                },
                configurable: true,
            },
        }),

        /// Сущность по дескриптору игры: нативы отдают именно их.
        fromScriptID: entities.fromScriptID,

        // Метки и маркеры. Эти видны по-настоящему: рисует их сама игра.
        Blip: objects.Blip,
        PointBlip: objects.PointBlip,
        RadiusBlip: objects.RadiusBlip,
        Marker: objects.Marker,

        showCursor: objects.showCursor,
        get isCursorVisible() { return objects.cursorVisible; },

        get gameControlsEnabled() { return objects.gameControlsEnabled; },
        toggleGameControls: objects.toggleGameControls,

        // Статистика, курсор, признаки персонажа, местные предметы, хранилище.
        /// Размер кадра игры. У alt:V он на самом alt, а не только среди нативов.
        getScreenResolution: () => {
            // Первое место списка — возврат натива; у `void` там пусто.
            const [, ширина, высота] = alt.natives.getScreenResolution(0, 0);
            return new shared.Vector2(ширина, высота);
        },

        setStat: extras.setStat,
        getStat: extras.getStat,
        getCursorPos: extras.getCursorPos,
        setCursorPos: extras.setCursorPos,
        setConfigFlag: extras.setConfigFlag,
        getConfigFlag: extras.getConfigFlag,
        LocalObject: extras.LocalObject,
        LocalStorage: extras.LocalStorage,
        Utils: extras.Utils,

        /// Метаданные сессии, присланные сервером.
        getSyncedMeta: (key) => syncedFor('global', 0).get(key),
        hasSyncedMeta: (key) => syncedFor('global', 0).has(key),
        getSyncedMetaKeys: () => [...syncedFor('global', 0).keys()],

        // Местные сущности: те, что клиент заводит сам и видит только сам.
        // Сервер о них не знает, и потому им не нужно ни сети, ни синхронизации.
        LocalVehicle: locals.LocalVehicle,
        LocalPed: locals.LocalPed,
        WeaponObject: locals.WeaponObject,
        Checkpoint: locals.Checkpoint,

        // Зоны считаются здесь и только про своего игрока — так же, как у alt:V.
        // Те, о которых должен знать сервер, заводятся на сервере: они там есть.
        Colshape: locals.Colshape,
        ColshapeCylinder: locals.ColshapeCylinder,
        ColshapeCuboid: locals.ColshapeCuboid,
        ColshapeSphere: locals.ColshapeSphere,
        ColshapeCircle: locals.ColshapeCircle,
        ColshapePolygon: locals.ColshapePolygon,

        // Сеть ресурса. Ходит в неё ресурс сервера, а не игрок: своей волей он
        // отсюда ничего не вызовет — ни меню, ни команд, ни клавиш у клиента
        // нет. Так же устроено и у alt:V.
        HttpClient: net.HttpClient,
        WebSocketClient: net.WebSocketClient,
        WebSocketReadyState: net.WebSocketReadyState,

        // Того, чего ещё нет. Отказом, а не тишиной.
        VirtualEntity: absent('alt.VirtualEntity'),
        Voice: absent('alt.Voice'),
        Audio: absent('alt.Audio'),
        RmlDocument: absent('alt.RmlDocument'),
        Discord: absent('alt.Discord'),
        // Распоряжения, которых мы не умеем исполнить: говорят один раз и не
        // бросают. Стоят они посреди чужих обработчиков, и брошенное отсюда
        // исключение унесло бы с собой всё, что идёт следом.
        loadModel: unperformed('alt.loadModel',
                               'модель подгружается нативом requestModel'),
        requestIpl: unperformed('alt.requestIpl',
                                'куски мира по слову ресурса не подгружаются'),
        setWeatherCycle: unperformed('alt.setWeatherCycle',
                                     'круг погоды ведёт сервер, а не ресурс'),

        /// Заморозка камеры. Своего натива у игры для этого нет: alt:V делает
        /// это внутри своего ядра, куда нам хода нет.
        setCamFrozen: unperformed('alt.setCamFrozen',
                                  'камера по слову ресурса не замирает'),

        /// Снимок кадра. Возвращает обещание — как в alt:V, — но пустое: отдать
        /// картинку нам неоткуда, а бросить нельзя, зовут это из обработчика.
        takeScreenshotGameOnly: () => {
            warnOnce('alt.takeScreenshotGameOnly', 'снимок кадра не снимается');
            return Promise.resolve('');
        },

        takeScreenshot: () => {
            warnOnce('alt.takeScreenshot', 'снимок кадра не снимается');
            return Promise.resolve('');
        },

        /// Обращение к разметке радара. false — «не начали»: так зовущий и
        /// узнает, что дальше звать нечего.
        beginScaleformMovieMethodMinimap: unperformed(
            'alt.beginScaleformMovieMethodMinimap',
            'разметка радара ресурсу недоступна', false),
    };

    alt.client = client;
})(globalThis.__oxympAlt);
