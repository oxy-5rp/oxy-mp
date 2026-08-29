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

    /// Место, откуда прилетела ошибка, — по первой строке её стека.
    ///
    /// У alt:V `resourceError` называет файл и строку отдельными доводами, а у
    /// брошенного `Error` они есть только внутри `stack`. Не разобралось —
    /// доводы остаются пустыми: по неверному месту ошибку ищут дольше, чем
    /// вовсе без места.
    function whereItBroke(failure) {
        const stack = typeof failure?.stack === 'string' ? failure.stack : '';
        const found = stack.match(/\((.*):(\d+):\d+\)/) ?? stack.match(/at (.*):(\d+):\d+/);

        return found === null ? { file: '', line: 0 }
                              : { file: found[1], line: Number(found[2]) };
    }

    /// Объявляет ресурсу его же ошибку. У alt:V это `resourceError` на обеих
    /// сторонах, и здесь оно нужнее: клиентский журнал лежит у игрока на диске,
    /// а не у того, кто режим писал.
    ///
    /// Ошибку внутри обработчика ошибки объявлять некому: она ушла бы тому же
    /// обработчику и по кругу.
    function tellAboutError(name, failure) {
        if (name === 'resourceError') {
            return;
        }

        const беда = failure instanceof Error ? failure : new Error(String(failure));
        const { file, line } = whereItBroke(беда);

        fire('resourceError', [беда, file, line, беда.stack ?? '']);
    }

    /// Одна и та же поломка, повторённая тысячу раз, — это не тысяча сведений, а
    /// одно, засыпавшее собой все остальные.
    ///
    /// Обработчик кадрового события падает шестьдесят раз в секунду. Живой чужой
    /// режим дал так пять тысяч одинаковых строк за минуту, и в журнале не
    /// осталось видно ничего другого — а журнал уходит наружу, и по нему
    /// разбирают чужие поломки.
    ///
    /// Поэтому повторы считаются, а печатаются по нарастающей: первые три, потом
    /// каждая десятая, сотая, тысячная. Так и о поломке сказано, и что она
    /// продолжается — видно, и журнал остаётся читаемым.
    const поломки = new Map();

    function пожаловатьсяНаПоломку(что, беда) {
        const ключ = `${что}|${беда?.message ?? беда}`;
        const было = (поломки.get(ключ) ?? 0) + 1;

        поломки.set(ключ, было);

        const порог = было <= 3 || было % 10 === 0 && было < 100
            || было % 100 === 0 && было < 1000 || было % 1000 === 0;

        if (!порог) {
            return;
        }

        const сколько = было === 1 ? '' : ` (повтор ${было})`;

        logError(`${что}${сколько}:`, беда?.stack ?? беда);
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
                пожаловатьсяНаПоломку(`ошибка в обработчике «${name}»`, failure);
                tellAboutError(name, failure);
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

    /// События, которые у alt:V есть, а у нас не объявляются ни разу.
    ///
    /// Подписка на такое — самая тихая беда из возможных: ошибки нет, обработчик
    /// стоит, а дорога к нему мёртвая. Снаружи это выглядит как «режим не
    /// работает», и виноватым кажется режим.
    ///
    /// Причина у каждого своя и названа. Большинство alt:V берёт перехватами
    /// внутри игры — попадание пули, урон, столкновение, смена задачи, — а у нас
    /// таких перехватов нет; остальным нужна синхронизация или голосовая связь,
    /// которых нет как хозяйства.
    const неОбъявляемые = new Map([
        ['consoleCommand', 'консоль по F8 у нас только на чтение, команд в неё не вводят'],
        ['entityHitEntity', 'о столкновениях сущностей игра нам не сообщает'],
        ['playerAnimationChange', 'состояние движения по сети не едет'],
        ['playerBulletHit', 'о попадании пули игра нам не сообщает: alt:V берёт это перехватом'],
        ['playerDimensionChange', 'свой слой мира клиент не отслеживает'],
        ['playerStartTalking', 'голосовой связи в oxyMP нет'],
        ['playerStopTalking', 'голосовой связи в oxyMP нет'],
        ['taskChange', 'о смене задачи игра нам не сообщает'],
        ['voiceConnection', 'голосовой связи в oxyMP нет'],
        ['weaponDamage', 'урон считает сервер; клиент только свидетельствует о попадании'],
        ['worldObjectPositionChange', 'о переносе предмета сервер объявляет его состоянием'],
    ]);

    /// Жалоба при подписке, а не молчание.
    ///
    /// Узнать иначе, что событие не приходит, можно только по тому, что режим не
    /// работает, — а это самый дорогой способ узнавать. Проверка одна на обе
    /// подписки: `on` и `once` разойтись здесь не должны.
    function предупредитьОМёртвом(кто, name) {
        const почему = неОбъявляемые.get(String(name));

        if (почему !== undefined) {
            warnOnce(`${кто}('${name}')`, `${почему}; обработчик не позовут ни разу`);
        }
    }

    function on(name, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.on ждёт имя события и обработчик');
        }

        предупредитьОМёртвом('alt.on', name);

        listenersFor(name).push({ handler, once: false });
        bridge(name);
    }

    function once(name, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('alt.once ждёт имя события и обработчик');
        }

        предупредитьОМёртвом('alt.once', name);

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
    /// Свои общие метаданные клиента. Один склад на весь клиент, а не на
    /// ресурс: у alt:V `alt.setMeta` видят все его ресурсы, и разделять их
    /// значило бы отнять единственное, ради чего эти метаданные и заводят.
    const ownGlobalMeta = new Map();

    /// На что сейчас наведена подгрузка мира. null — на игрока, как обычно.
    ///
    /// Помнится здесь потому, что у игры об этом не спросить: `IS_ENTITY_FOCUS`
    /// отвечает про одну названную сущность, а «наведено ли вообще» она не
    /// понимает вовсе.
    let focusEntity = null;

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

    /// Смена помещения: то же событие у сервера, и доехать оно может только
    /// отсюда — у сервера нет ни персонажа, ни натива, чтобы спросить.
    const kInteriorEvent = '__oxymp:interior';

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
        setZIndex(_index) { throw new Error('webView.setZIndex: в oxyMP этого ещё нет'); }

        setExtraHeader(_name, _value) {
            throw new Error('webView.setExtraHeader: заголовки запросам страницы у нас не '
                            + 'подставляются');
        }

        setCookie(_url, _name, _value) {
            throw new Error('webView.setCookie: своим хранилищем печений страница у нас не '
                            + 'распоряжается');
        }

        setZoomLevel(_level) {
            throw new Error('webView.setZoomLevel: масштаб страницы у нас не задаётся');
        }

        /// Готова ли страница. У alt:V это признак его собственного слоя; у нас
        /// о готовности говорит событие `load`, и признака рядом нет.
        get isReady() {
            throw new Error('webView.isReady: о готовности страницы говорит событие load');
        }

        /// Ускорение видеокартой. Chromium в кадре игры рисует программно, и
        /// иначе он рисовать не может: кадр отдаётся буфером.
        get gpuAccelerationActive() { return false; }

        /// Куда уходит звук страницы. У alt:V звук можно развести по своим
        /// выходам; у нас звукового слоя нет вовсе.
        addOutput(_output) {
            throw new Error('webView.addOutput: своего звукового слоя в oxyMP нет');
        }

        removeOutput(_output) {
            throw new Error('webView.removeOutput: своего звукового слоя в oxyMP нет');
        }

        getOutputs() {
            throw new Error('webView.getOutputs: своего звукового слоя в oxyMP нет');
        }
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

    /// Свои выстрелы и смена оружия: `playerWeaponShoot` и `playerWeaponChange`.
    ///
    /// Оба видны из одного наблюдения за обоймой, и потому считаются вместе:
    /// заводить ради второго свой обход кадра значило бы платить за один и тот
    /// же вопрос дважды.
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
        if (listenersFor('playerWeaponShoot').length === 0 &&
            listenersFor('playerWeaponChange').length === 0) {
            // Забываем и обойму: подписавшийся посреди боя не должен получить
            // залп за всё время, что его не слушали.
            ownClip = -1;
            return;
        }

        const ped = alt.natives.playerPedId();

        // Беззнаковым: натив отдаёт хеш знаковым числом, а `alt.hash` считает
        // беззнаковый, и режим сравнивает одно с другим через `===`. Молчит это
        // полностью — оба числа законны.
        const weapon = alt.natives.getSelectedPedWeapon(ped) >>> 0;

        // Ответ приходит вторым местом: сам натив отвечает «есть ли обойма».
        //
        // **И этот ответ обязателен к проверке.** `GET_AMMO_IN_CLIP` отвечает
        // «нет» всякий раз, когда оружия нет **в руках** — а выбранным оно при
        // этом числится: убравший ствол в кобуру человек всё ещё «выбрал»
        // пистолет, но `IS_PED_ARMED` про него отвечает «безоружен». Измерено
        // живой игрой: выданный пистолет числится выбранным, патронов у него
        // шестьдесят, а обойма не читается вовсе.
        //
        // Не проверь мы ответ — обойма читалась бы нулём, и убравший ствол
        // человек «выстреливал» бы разом всю обойму: разница между прошлым
        // числом и нулём объявилась бы залпом.
        const [ясно, clip] = alt.natives.getAmmoInClip(ped, weapon, 0);

        if (ясно !== true) {
            // Оружия в руках нет — считать нечего и не с чем сравнивать в
            // следующий раз.
            ownClip = -1;
            return;
        }

        if (weapon !== ownWeapon) {
            // Смена оружия обойму обнуляет не выстрелом, а сменой: считать
            // разницу между обоймами разных стволов бессмысленно.
            const был = ownWeapon;

            ownWeapon = weapon;
            ownClip = clip;

            // Заодно объявляем смену: у alt:V это `playerWeaponChange(было,
            // стало)`. Своего источника у неё нет — она видна из того же
            // наблюдения за обоймой, и заводить ради неё второй обход кадра
            // значило бы платить за один и тот же вопрос дважды.
            //
            // Первое чтение сменой не считается: до него `ownWeapon` нулевой, а
            // безоружный персонаж — не то же самое, что сменивший оружие.
            if (был !== 0) {
                fire('playerWeaponChange', [был, weapon]);
            }

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

    /// Что вокруг игрока переменилось: окно, интерьер, посадка в машину.
    ///
    /// Всё это alt:V объявляет своими событиями, и всё это выводится наблюдением
    /// за ответами игры: своего источника ни у одного из них нет. Считается
    /// только пока кто-то слушает — иначе за них платили бы нативами каждый
    /// кадр без нужды.
    ///
    /// Первое чтение переменой не считается: до него сравнивать не с чем, и
    /// объявленная «перемена» из ничего в нынешнее была бы ложью.
    let ownInterior = null;
    let ownEntering = null;
    let ownLeaving = null;
    let ownFocused = null;
    let ownWidth = 0;
    let ownHeight = 0;

    function watchOwnState() {
        const слушают = (имя) => listenersFor(имя).length !== 0;

        if (слушают('windowResolutionChange')) {
            const [, ширина, высота] = alt.natives.getActualScreenResolution(0, 0);

            if (ширина !== ownWidth || высота !== ownHeight) {
                const было = new shared.Vector2(ownWidth, ownHeight);

                ownWidth = ширина;
                ownHeight = высота;

                if (было.x !== 0 || было.y !== 0) {
                    fire('windowResolutionChange', [было, new shared.Vector2(ширина, высота)]);
                }
            }
        }

        if (слушают('windowFocusChange')) {
            // Меню паузы у игры открывается само, когда окно теряет внимание, —
            // и это единственный признак потери внимания, который игра отдаёт
            // изнутри себя. Спрашивать окно у Windows отсюда нельзя: слой на
            // JavaScript живёт в кадре игры и о её окне ничего не знает.
            const внимание = alt.natives.isPauseMenuActive() !== true;

            if (внимание !== ownFocused) {
                const было = ownFocused;

                ownFocused = внимание;

                if (было !== null) {
                    fire('windowFocusChange', [внимание]);
                }
            }
        }

        const тело = alt.natives.playerPedId();

        // Помещение считается всегда, а не только когда его слушают здесь: то же
        // событие есть и у сервера, и узнать оттуда, слушает ли его кто-нибудь,
        // нечем. Цена — один натив в кадр; уезжает же наружу только сама смена,
        // то есть раз на вход в здание.
        {
            const внутри = alt.natives.getInteriorFromEntity(тело);

            if (внутри !== ownInterior) {
                const было = ownInterior;

                ownInterior = внутри;

                if (было !== null) {
                    fire('playerInteriorChange', [entities.local, было, внутри]);
                    emitServerRaw(kInteriorEvent, encodeArgs([было, внутри]));
                }
            }
        }

        if (слушают('startLeavingVehicle')) {
            // Своего вопроса «вылезает ли» у игры нет, и он выводится из трёх
            // её ответов: «в машине, считая залезающего» — да, «в машине» —
            // нет, «залезает» — нет. То же правило и в снимке игрока
            // (`client/src/game/ped_activity.cpp`), и разойтись им нельзя: одно
            // вылезание, объявленное двумя способами по-разному, — это две
            // разные правды об одном.
            //
            // Проверять это задачей `TASK_LEAVE_VEHICLE` бессмысленно: замерено
            // покадрово — при выходе по задаче оба ответа держатся истинными до
            // последнего кадра и разом становятся ложными, промежутка не
            // возникает. Правило верно для выхода, который затеял человек.
            const вылезает = alt.natives.isPedInAnyVehicle(тело, true) === true &&
                             alt.natives.isPedInAnyVehicle(тело, false) !== true &&
                             alt.natives.isPedGettingIntoAVehicle(тело) !== true;

            if (вылезает !== ownLeaving) {
                ownLeaving = вылезает;

                if (вылезает) {
                    const откуда = alt.natives.getVehiclePedIsIn(тело, true);

                    fire('startLeavingVehicle',
                         [entities.fromScriptID(откуда), -1, entities.local]);
                }
            }
        }

        if (слушают('startEnteringVehicle')) {
            // Машина, в которую человек полез, а не та, в которой он сидит:
            // событие объявляется в начале посадки, а к её концу оно уже не
            // новость.
            //
            // Спрашивается двумя нативами, и это не перестраховка.
            // `GET_VEHICLE_PED_IS_TRYING_TO_ENTER` отвечает только про попытку
            // сесть на занятое место — проверено живой игрой: при обычной
            // посадке он молчит. Задача входа при этом видна вторым вопросом, а
            // машину под ней называет `GET_VEHICLE_PED_IS_IN` со «считать
            // залезающего».
            const лезет = alt.natives.isPedGettingIntoAVehicle(тело) === true;
            const куда = лезет ? alt.natives.getVehiclePedIsIn(тело, true)
                               : alt.natives.getVehiclePedIsTryingToEnter(тело);

            if (куда !== ownEntering) {
                ownEntering = куда;

                if (куда !== 0) {
                    fire('startEnteringVehicle',
                         [entities.fromScriptID(куда), -1, entities.local]);
                }
            }
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
        watchOwnState();

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
        /// Прохожие, заведённые сервером.
        ///
        /// `all` и `streamedIn` разведены так же, как у игроков и машин: первое
        /// — все, о ком сказал сервер, второе — те, у кого здесь есть тело.
        /// Случайные прохожие самой игры сюда не попадают: у них нет номера
        /// сессии, и сервер о них не знает.
        Ped: Object.defineProperties(entities.Ped, {
            all: { get: () => entities.peds(), configurable: true },
            streamedIn: { get: () => entities.peds(true), configurable: true },
            count: { get: () => entities.peds().length, configurable: true },

            getByID: { value: (id) => entities.pedById(id), configurable: true },
            getByRemoteID: { value: (id) => entities.pedById(id), configurable: true },

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
        TextLabel: objects.TextLabel,

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
        /// Помощники alt:V. Две рисовалки текста добавляются здесь: им нужен
        /// `everyTick`, а он живёт в этом файле, не в `alt_client_extras.js`.
        Utils: Object.assign(extras.Utils, {
            drawText2d(text, pos2d, font, scale, color, outline, dropShadow, textAlign) {
                return everyTick(() => extras.Utils.drawText2dThisFrame(
                    text, pos2d, font, scale, color, outline, dropShadow, textAlign));
            },

            drawText3d(text, pos3d, font, scale, color, outline, dropShadow, textAlign) {
                return everyTick(() => extras.Utils.drawText3dThisFrame(
                    text, pos3d, font, scale, color, outline, dropShadow, textAlign));
            },
        }),

        /// Метаданные сессии, присланные сервером.
        getSyncedMeta: (key) => syncedFor('global', 0).get(key),
        hasSyncedMeta: (key) => syncedFor('global', 0).has(key),
        getSyncedMetaKeys: () => [...syncedFor('global', 0).keys()],

        /// Свои общие метаданные: те, что клиент кладёт себе сам.
        ///
        /// Никуда не уезжают и ниоткуда не приезжают — это склад одного
        /// клиента. У alt:V так же: `alt.setMeta` на клиенте местный, а сетевые
        /// — это `syncedMeta`, которые кладёт сервер.
        ///
        /// Нужны они затем же, зачем и `LocalStorage`: сложить что-нибудь между
        /// ресурсами одного клиента. Отличие в том, что эти не переживают
        /// перезапуск игры, и потому их не приходится писать на диск.
        setMeta(key, value) {
            shared._eachMetaPair(key, value, (name, own) => {
                const было = ownGlobalMeta.get(name);

                ownGlobalMeta.set(name, own);
                fire('globalMetaChange', [name, own, было]);
            });
        },

        getMeta: (key) => ownGlobalMeta.get(key),
        hasMeta: (key) => ownGlobalMeta.has(key),
        getMetaKeys: () => [...ownGlobalMeta.keys()],

        deleteMeta(key) {
            const было = ownGlobalMeta.get(key);

            ownGlobalMeta.delete(key);
            fire('globalMetaChange', [key, undefined, было]);
        },

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

        /// Куда игре смотреть при подгрузке мира.
        ///
        /// Обычно игра грузит мир вокруг игрока; этим его можно увести в другое
        /// место — например к машине, за которой следит камера, или к точке,
        /// куда игрок сейчас поедет. Без этого всё за пределами обычной
        /// дальности остаётся незагруженным, и режим, показывающий что-нибудь
        /// издали, показывает пустоту.
        ///
        /// Точку игре задать нечем: натив `SET_FOCUS_POS_AND_VEL` в объявлениях
        /// alt:V отсутствует, и в нашей таблице его нет. Сущностью — можно, и
        /// это как раз то, ради чего наведение и заводят.
        ///
        /// Состояние помнится здесь, а не спрашивается у игры: `IS_ENTITY_FOCUS`
        /// отвечает лишь про одну названную сущность, а вопрос «наведено ли
        /// вообще» она не понимает.
        FocusData: {
            get isFocusOverriden() { return focusEntity !== null; },
            get focusOverrideEntity() { return focusEntity; },

            get focusOverridePos() {
                throw new Error('alt.FocusData.focusOverridePos: точку игре задать нечем — '
                                + 'натива на неё нет ни у нас, ни в объявлениях alt:V');
            },

            get focusOverrideOffset() {
                throw new Error('alt.FocusData.focusOverrideOffset: смещение принимает '
                                + 'только наведение на точку, а его у нас нет');
            },

            /// Наводит на сущность. Точкой — отказ вслух: смолчав, мы оставили
            /// бы режим уверенным, что мир вокруг неё грузится.
            overrideFocus(target) {
                if (target === null || target === undefined
                    || typeof target.scriptID !== 'number') {
                    throw new Error('alt.FocusData.overrideFocus: наводить умеем только на '
                                    + 'сущность — натива на точку нет');
                }

                focusEntity = target;
                natives.setFocusEntity(target.scriptID);
            },

            clearFocusOverride() {
                focusEntity = null;
                natives.clearFocus();
            },
        },

        /// Отладочное рисование в мире.
        ///
        /// Шар у игры свой (`DRAW_DEBUG_SPHERE`), и он же лежит под этим именем
        /// у alt:V. Число граней его не спрашивает — сколько нарисовать, игра
        /// решает сама по расстоянию; довод принимается и не читается, о чём
        /// сказано вслух один раз, а не молча пропущено.
        Rendering: {
            drawSphere(center, radius, color, segments) {
                const точка = new shared.Vector3(center);
                const цвет = color === undefined ? shared.RGBA.white : new shared.RGBA(color);

                if (segments !== undefined) {
                    warnOnce('alt.Rendering.drawSphere',
                             'число граней игра не принимает: она решает его сама');
                }

                alt.natives.drawDebugSphere(точка.x, точка.y, точка.z, Number(radius) || 1,
                                        цвет.r, цвет.g, цвет.b, цвет.a);
            },

            setDepthtesting: unperformed(
                'alt.Rendering.setDepthtesting',
                'глубиной отладочного рисования игра не распоряжается: у alt:V это '
                + 'настройка его собственного вывода, а рисуем мы игрой'),
        },

        // Того, чего ещё нет. Отказом, а не тишиной.
        VirtualEntity: absent('alt.VirtualEntity'),
        Voice: absent('alt.Voice'),
        Audio: absent('alt.Audio'),
        AudioCategory: absent('alt.AudioCategory'),
        AudioFilter: absent('alt.AudioFilter'),
        AudioOutput: absent('alt.AudioOutput'),
        RmlDocument: absent('alt.RmlDocument'),
        RmlElement: absent('alt.RmlElement'),
        Discord: absent('alt.Discord'),

        // Три подсистемы, которых нет не по недосмотру: alt:V читает их прямо из
        // памяти игры — управляемость машины, разметку помещений, числа оружия,
        // — а смещения там свои у каждой сборки. Пока чтения нет, честнее
        // отказать по имени, чем оставить `undefined`: тогда ресурс падает с
        // «Cannot read properties of undefined», и виноватым выглядит он сам.
        HandlingData: absent('alt.HandlingData'),
        MemoryBuffer: absent('alt.MemoryBuffer'),
        Worker: absent('alt.Worker'),
        WeaponData: absent('alt.WeaponData'),
        Interior: absent('alt.Interior'),
        InteriorPortal: absent('alt.InteriorPortal'),
        InteriorRoom: absent('alt.InteriorRoom'),
        MapZoomData: absent('alt.MapZoomData'),
        Profiler: absent('alt.Profiler'),
        // Распоряжения, которых мы не умеем исполнить: говорят один раз и не
        // бросают. Стоят они посреди чужих обработчиков, и брошенное отсюда
        // исключение унесло бы с собой всё, что идёт следом.
        loadModel: unperformed('alt.loadModel',
                               'модель подгружается нативом requestModel'),
        /// Подгружает кусок мира по имени.
        ///
        /// Натив у игры есть, и отказ здесь стоял по недосмотру: без него
        /// режимы со своими помещениями не показывали их вовсе — интерьеры
        /// GTA V почти все лежат отдельными IPL и без просьбы не грузятся.
        ///
        /// Ответа игра не даёт: `REQUEST_IPL` ничего не возвращает, а
        /// подгрузка занимает кадры. Спросить, вышло ли, можно `isIplActive`
        /// — тем же путём, каким это делает и alt:V.
        requestIpl: (name) => alt.natives.requestIpl(String(name)),
        removeIpl: (name) => alt.natives.removeIpl(String(name)),
        isIplActive: (name) => alt.natives.isIplActive(String(name)) === true,
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
