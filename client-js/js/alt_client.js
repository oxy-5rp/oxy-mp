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

    const client = {
        ...shared,

        isClient: true,
        isServer: false,

        get resourceName() { return native.resourceName; },

        on,
        once,
        off,
        emit,
        onServer,
        offServer,
        emitServer,
        emitServerRaw,

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

        // Того, чего ещё нет. Отказом, а не тишиной.
        Player: absent('alt.Player'),
        Vehicle: absent('alt.Vehicle'),
        Entity: absent('alt.Entity'),
        LocalPlayer: absent('alt.LocalPlayer'),
        LocalVehicle: absent('alt.LocalVehicle'),
        LocalPed: absent('alt.LocalPed'),
        LocalObject: absent('alt.LocalObject'),
        WorldObject: absent('alt.WorldObject'),
        Blip: absent('alt.Blip'),
        PointBlip: absent('alt.PointBlip'),
        Marker: absent('alt.Marker'),
        Checkpoint: absent('alt.Checkpoint'),
        Colshape: absent('alt.Colshape'),
        VirtualEntity: absent('alt.VirtualEntity'),
        Voice: absent('alt.Voice'),
        Audio: absent('alt.Audio'),
        HttpClient: absent('alt.HttpClient'),
        WebSocketClient: absent('alt.WebSocketClient'),
        RmlDocument: absent('alt.RmlDocument'),
        LocalStorage: absent('alt.LocalStorage'),
        Discord: absent('alt.Discord'),
        showCursor: absent('alt.showCursor'),
        getCursorPos: absent('alt.getCursorPos'),
        setCursorPos: absent('alt.setCursorPos'),
        gameControlsEnabled: absent('alt.gameControlsEnabled'),
        toggleGameControls: absent('alt.toggleGameControls'),
        loadModel: absent('alt.loadModel'),
        requestIpl: absent('alt.requestIpl'),
        setWeatherCycle: absent('alt.setWeatherCycle'),
    };

    alt.client = client;
})(globalThis.__oxympAlt);
