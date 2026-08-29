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

    /// Дальность раздачи, спрошенная один раз.
    ///
    /// Раз, а не на каждый вызов: `isEntityInStreamRange` зовут в обходе по
    /// сущностям, и переход через границу на каждую из них стоил бы дороже
    /// самой проверки. Настройка эта за время работы сервера не меняется — она
    /// читается из файла при запуске.
    let знаемДальность = null;

    function streamingDistance() {
        if (знаемДальность === null) {
            знаемДальность = native.serverConfig().streamingDistance;
        }

        return знаемДальность;
    }

    /// Сущность по роду и номеру. Роды — числа протокола (`shared::EntityKind`).
    ///
    /// Игрок сюда не попадает: раздачу игроков сервер списком не ведёт, и рода
    /// «игрок» в этом ответе не бывает вовсе.
    function byKind(kind, id) {
        switch (kind) {
        case 2: return Vehicle.getByID(id);
        case 3: return WorldObject.getByID(id);
        case 4: return Ped.getByID(id);
        default: return null;
        }
    }

    /// Чем игра метит «наложения на лице нет».
    ///
    /// Двести пятьдесят пять, а не ноль: ноль — это первое наложение из списка,
    /// и снять им ничего нельзя. Так же считает и alt:V.
    const kNoOverlay = 255;

    /// Чем справочник метит «набора тюнинга у модели нет». То же число, что и
    /// у ядра (`script::kNoModKit`).
    const kNoModKit = 0xFFFF;

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
    /// `answer` — что вернуть. У alt:V многие распоряжения отвечают признаком
    /// «получилось», и `undefined` вместо него — та же ложь, что и заглушка:
    /// оно ложно, но ложно случайно, а не потому, что мы это сказали. Сверка
    /// машинная: `tools/altvparity/refusal_kind.py`.
    function unperformed(what, why, answer) {
        return function () {
            warnOnce(what, why);
            return answer;
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

        /// То же самое под именем alt:V. Оба имени живые: `get` пришло из RAGE
        /// MP, а режимы под alt:V зовут `getByName`.
        static getByName(name) {
            return ScriptResource.get(name);
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

    /// Место, откуда прилетела ошибка, — по первой строке её стека.
    ///
    /// У alt:V `resourceError` называет файл и строку отдельными доводами, а у
    /// брошенного `Error` они есть только внутри `stack`. Разбирается первая
    /// строка вида `at что-то (файл:строка:столбец)`; не разобралась — доводы
    /// остаются пустыми, и это лучше выдуманных: по неверному месту ошибку
    /// ищут дольше, чем вовсе без места.
    function whereItBroke(failure) {
        const stack = typeof failure?.stack === 'string' ? failure.stack : '';
        const found = stack.match(/\((.*):(\d+):\d+\)/) ?? stack.match(/at (.*):(\d+):\d+/);

        return found === null ? { file: '', line: 0 }
                              : { file: found[1], line: Number(found[2]) };
    }

    /// Объявляет ресурсу его же ошибку. У alt:V это `resourceError`, и вешают
    /// на него отправку в свой сбор ошибок: журнала сервера режиму мало —
    /// читает его хозяин сервера, а не режим.
    ///
    /// Ошибку внутри обработчика ошибки объявлять некому: она ушла бы тому же
    /// обработчику и по кругу. Такая пишется только в журнал — строкой выше.
    function tellAboutError(name, failure) {
        if (name === 'resourceError') {
            return;
        }

        const беда = failure instanceof Error ? failure : new Error(String(failure));
        const { file, line } = whereItBroke(беда);

        fire('resourceError', [беда, file, line, беда.stack ?? '']);
    }

    /// Зовёт подписчиков имени. Возвращает false, если хоть один отменил событие.
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
                пожаловатьсяНаПоломку(`ошибка в обработчике «${name}»`, failure);
                tellAboutError(name, failure);
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

    /// Достраивает описание модели машины до того вида, в каком его ждёт alt:V.
    ///
    /// Ядро отдаёт числа — маски дополнений и номера наборов тюнинга, — а alt:V
    /// объявляет `availableModkits` списком признаков и `hasExtra` вопросом.
    /// Разбор битов живёт здесь, а не в ядре, и не в обоих: два разбора одного и
    /// того же со временем расходятся, и разошедшиеся молчат.
    function dressVehicleModel(info) {
        if (info === null || info === undefined) {
            return null;
        }

        // Список признаков по номеру набора, как у alt:V. Длина — до
        // наибольшего из двух названных: списка всех наборов сервера у режима
        // нет, а `availableModkits[n]` он спрашивает по номеру.
        const наборы = [];

        for (const набор of [info.modKit, info.secondModKit]) {
            if (набор !== kNoModKit) {
                наборы[набор] = true;
            }
        }

        for (let at = 0; at < наборы.length; at += 1) {
            if (наборы[at] === undefined) {
                наборы[at] = false;
            }
        }

        info.availableModkits = наборы;

        // Дополнения кузова нумеруются с первого, а биты — с нулевого.
        info.hasExtra = (id) => ((info.extras >>> (Number(id) - 1)) & 1) !== 0;
        info.hasDefaultExtra = (id) => ((info.defaultExtras >>> (Number(id) - 1)) & 1) !== 0;

        return info;
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

    /// События, которые у alt:V есть, а у нас не объявляются ни разу.
    ///
    /// Подписка на такое — самая тихая беда из возможных: ошибки нет, обработчик
    /// стоит, а дорога к нему мёртвая. Снаружи это выглядит как «режим не
    /// работает», и виноватым кажется режим.
    ///
    /// Причина у каждого своя и названа: одним нужна синхронизация, которой у нас
    /// нет (сцены, задачи, снаряды), другим — очередь подключений или голосовая
    /// связь, которых нет как хозяйства.
    const неОбъявляемые = new Map([
        ['clientRequestObject', 'клиент не просит сервер о предметах — их ставит сервер сам'],
        ['clientDeleteObject', 'клиент не просит сервер о предметах — их убирает сервер сам'],
        ['connectionQueueAdd', 'очереди подключений у нас нет'],
        ['connectionQueueRemove', 'очереди подключений у нас нет'],
        ['givePedScriptedTask', 'задачи прохожих по сети не едут'],
        ['playerAnimationChange', 'состояние движения по сети не едет'],
        ['playerRequestControl', 'прав на сущность клиент не просит: ведущего назначает сервер'],
        ['requestSyncedScene', 'согласованных сцен у нас нет'],
        ['startSyncedScene', 'согласованных сцен у нас нет'],
        ['stopSyncedScene', 'согласованных сцен у нас нет'],
        ['updateSyncedScene', 'согласованных сцен у нас нет'],
        ['startFire', 'о начатом огне клиент не сообщает'],
        ['startProjectile', 'о выпущенном снаряде клиент не сообщает'],
        ['voiceConnection', 'голосовой связи в oxyMP нет'],
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
        // Не знаем такого вызова — **молчим**, а не отвечаем отказом.
        //
        // Отвечать нельзя, и это стоило целой поломки у живого чужого режима.
        // Вызов приходит **каждому** ресурсу: слой поднят внутри каждого, и
        // подписка на служебное имя у каждого своя. Отвечающий же — один, а
        // остальные о таком имени не знают. Ответь они отказом — первый пришедший
        // отказ и станет ответом, а настоящий ответ опоздает и будет отброшен.
        //
        // Снаружи это выглядело так: у режима с четырьмя ресурсами всякий вызов
        // отвечал «на вызов X никто не отвечает», хотя обработчик был и работал.
        //
        // Молчание безопасно: у зовущего есть свой срок, и не ответивший никем
        // вызов кончится им.
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

    /// Смена помещения приезжает от клиента: у сервера нет ни персонажа, ни
    /// натива, чтобы спросить, — а событие такое у alt:V есть и на его стороне.
    onClient('__oxymp:interior', (player, было, стало) =>
        fire('playerInteriorChange', [player, было, стало]));

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
            /// Принимает и пару, и объект целиком — обе формы alt:V.
            setMeta(key, value) {
                shared._eachMetaPair(key, value, (name, own) => {
                    const store = metaFor(kind, this.id);
                    const was = store.get(name);

                    store.set(name, own);
                    server.fireLocal('metaChange', [this, name, own, was]);
                });
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
              shared._eachMetaPair(key, value, (name, own) => {
                const store = syncedFor(kind, this.id);
                const was = store.get(name);

                store.set(name, own);
                publishSynced(kind, this.id, name, own);

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
                server.fireLocal('syncedMetaChange', [this, name, own, was]);
              });
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
                shared._eachMetaPair(key, value, (name, own) => {
                    const store = syncedFor(kind, this.id);
                    const was = store.get(name);

                    store.set(name, own);
                    publishSynced(kind, this.id, name, own, true);
                    server.fireLocal('streamSyncedMetaChange', [this, name, own, was]);
                });
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

    /// Кость: у alt:V она либо номер, либо имя.
    ///
    /// Имя переводит игра, а не сервер, и с появлением справочника моделей это
    /// стало решением, а не вынужденностью: номера костей по моделям у сервера
    /// теперь есть (`getPedModelInfoByHash().bones`). Переводить ими всё равно
    /// нельзя — справочник собран с чужой сборки игры, а номера костей
    /// принадлежат той сборке, что запущена у игрока. Разойдись они хоть на
    /// одну модель, привязка встала бы не к той кости и не сказала бы об этом
    /// ни слова.
    ///
    /// Справочник для этого и не нужен: имя доезжает до клиента как есть, и
    /// переводит его сама игра — то есть единственный, кто знает наверняка.
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
        // Чего у игрока нет: облачный вход alt:V, состояние, которого нет в
        // снимке, и признак запрета менять ведущего. Здесь, а не в блоке
        // методов рядом: `Object.assign` копирует геттер, **вызывая** его, и
        // весь слой перестаёт подниматься.
        authToken: { get: absent('player.authToken') },
        cloudAuthResult: { get: absent('player.cloudAuthResult') },
        cloudID: { get: absent('player.cloudID') },
        discordID: { get: absent('player.discordID') },
        hwid3: { get: absent('player.hwid3') },
        hwidExHash: { get: absent('player.hwidExHash') },
        currentAnimationDict: { get: absent('player.currentAnimationDict') },
        currentAnimationName: { get: absent('player.currentAnimationName') },
        currentInterior: { get: absent('player.currentInterior') },
        entityAimingAt: { get: absent('player.entityAimingAt') },
        entityAimOffset: { get: absent('player.entityAimOffset') },
        flashlightActive: { get: absent('player.flashlightActive') },
        headRot: { get: absent('player.headRot') },
        lastDamagedBodyPart: { get: absent('player.lastDamagedBodyPart') },
        netOwnershipDisabled: { get: absent('player.netOwnershipDisabled') },

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
        /// `isOnVehicle` и `isStealthy` приходят от ядра наравне с остальными.
        ///
        /// Второе отвечает тем же признаком, что и `isCrouching`, и это не
        /// подмена: своего приседания у игрока в GTA V нет — крадущийся и
        /// пригнувшийся там одно и то же, и `GET_PED_STEALTH_MOVEMENT`
        /// отвечает на оба вопроса. У alt:V это два свойства, и оба отвечают
        /// одним признаком игры.
        ///
        /// Двух других у нас нет, и они отказывают вслух, а не отвечают «нет».
        /// Правило то же, что и везде здесь: тишина на вопрос — это ложь, и
        /// ресурс, принявший её за правду, унесёт эту ложь дальше.
        ///
        /// `isOnLadder` — натива у игры нет вовсе: лестница у неё задача, а
        /// номер задачи пришлось бы угадывать, и угаданный не тот отвечал бы
        /// правдоподобно. `isSuperJumpEnabled` игра только выставляет, но не
        /// возвращает: натив у неё «на этот кадр», и вопроса к нему нет.
        isOnLadder: { get: absent('player.isOnLadder') },
        isSuperJumpEnabled: { get: absent('player.isSuperJumpEnabled') },
        /// Что этому игроку сейчас раздаётся: сущность и расстояние до неё.
        ///
        /// Спрашивается у сервера, а не у клиента: решает, кому что отдавать,
        /// сервер, и его ответ и есть правда — клиент узнаёт о ней позже.
        ///
        /// Игроков здесь нет, и это не пробел: их сервер раздаёт всем, кто в
        /// дальности, но списка «кому какого отдали» не ведёт — снимки идут
        /// связкой, а не по одному. Выдумать такой список значило бы соврать про
        /// то, чего сервер не считает.
        streamedEntities: {
            get() {
                const плоско = this.streamedEntitiesRaw();
                const мой = this.pos;
                const found = [];

                for (let i = 0; i + 1 < плоско.length; i += 2) {
                    const entity = byKind(плоско[i], плоско[i + 1]);

                    if (entity === null) {
                        continue;
                    }

                    const их = entity.pos;
                    const dx = мой.x - их.x;
                    const dy = мой.y - их.y;
                    const dz = мой.z - их.z;

                    found.push({ entity, distance: Math.sqrt(dx * dx + dy * dy + dz * dz) });
                }

                    return found;
            },
        },

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
        /// Уходит всем, кто игрока видит, а не одному хозяину: у остальных он
        /// показан куклой, и умытым бы он у них не стал.
        clearBloodDamage: {
            value() {
                return native.clearBlood(this.id);
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
                               'вещи наборов идут следом за основными', false),
        },
        setDlcProp: {
            value: unperformed('player.setDlcProp',
                               'аксессуары из наборов зовите setProp со сквозным номером',
                               false),
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
        /// Произносит реплику голосом персонажа.
        ///
        /// Уходит всем, кто игрока видит, и по той же причине, что движение: у
        /// остальных он показан куклой, и молчащая кукла осталась бы немой.
        ///
        /// Третий довод — голос. Пустой означает «своим»; названный уводит вызов
        /// на другой натив у клиента, где голос идёт отдельным доводом.
        playAmbientSpeech: {
            value(speechName, speechParam, speechDictionary) {
                return native.playSpeech(this.id, String(speechName ?? ''),
                                         String(speechParam ?? ''),
                                         String(speechDictionary ?? ''));
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
    /// Какого выпуска записи прочностей и повреждений. Поднимать при всякой
    /// правке состава — по той же причине, что и у внешности.
    const kHealthFormat = 1;
    const kDamageFormat = 1;

    /// Разбирает свою запись из base64 и проверяет, что она наша.
    ///
    /// Один разбор на все записи, а не по одному у каждой: три копии проверки
    /// «наша ли это запись» со временем разойдутся, и разошедшаяся примет чужое
    /// молча.
    function readOwnRecord(who, data, format) {
        let запись;

        try {
            запись = JSON.parse(Buffer.from(String(data), 'base64').toString('utf8'));
        } catch (беда) {
            throw new Error(`${who}: запись не разбирается — ${беда.message}`);
        }

        if (запись?.oxymp !== format) {
            throw new Error(`${who}: запись не наша или от другой сборки `
                            + `(ждали ${format}, в записи ${запись?.oxymp})`);
        }

        return запись;
    }

    /// Какого выпуска запись внешности. Поднимать при всякой правке состава:
    /// запись прежнего выпуска тогда отказывает вслух вместо того, чтобы лечь
    /// на машину не теми полями.
    const kAppearanceFormat = 1;

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
        // --- Чего у машины нет, и почему ------------------------------------
        //
        // Ниже четыре семьи имён, которых у нас нет. Отказ у всех громкий, а не
        // тишина: свойство, отвечающее `undefined`, ресурс примет за правду и
        // унесёт её дальше. Причина у каждой семьи своя, и она названа.
        //
        // Поезда. Их у нас нет как рода: рельсы, составы и сцепка — отдельное
        // хозяйство игры, которого сервер не ведёт вовсе, и всякое число отсюда
        // было бы выдумкой.
        isMissionTrain: { get: absent('vehicle.isMissionTrain') },
        isTrainCaboose: { get: absent('vehicle.isTrainCaboose') },
        isTrainEngine: { get: absent('vehicle.isTrainEngine') },
        trainCarriageConfigIndex: { get: absent('vehicle.trainCarriageConfigIndex') },
        trainConfigIndex: { get: absent('vehicle.trainConfigIndex') },
        trainCruiseSpeed: { get: absent('vehicle.trainCruiseSpeed') },
        trainDirection: { get: absent('vehicle.trainDirection') },
        trainDistanceFromEngine: { get: absent('vehicle.trainDistanceFromEngine') },
        trainEngineId: { get: absent('vehicle.trainEngineId') },
        trainForceDoorsOpen: { get: absent('vehicle.trainForceDoorsOpen') },
        trainLinkedToBackwardId: { get: absent('vehicle.trainLinkedToBackwardId') },
        trainLinkedToForwardId: { get: absent('vehicle.trainLinkedToForwardId') },
        trainPassengerCarriages: { get: absent('vehicle.trainPassengerCarriages') },
        trainRenderDerailed: { get: absent('vehicle.trainRenderDerailed') },
        trainTrackId: { get: absent('vehicle.trainTrackId') },
        trainUnk1: { get: absent('vehicle.trainUnk1') },
        trainUnk2: { get: absent('vehicle.trainUnk2') },
        trainUnk3: { get: absent('vehicle.trainUnk3') },
        setTrainEngineId: { value: unperformed('vehicle.setTrainEngineId', 'поездов у нас нет как рода') },
        setTrainLinkedToBackwardId: { value: unperformed('vehicle.setTrainLinkedToBackwardId', 'поездов у нас нет как рода') },
        setTrainLinkedToForwardId: { value: unperformed('vehicle.setTrainLinkedToForwardId', 'поездов у нас нет как рода') },

        // Колёса поштучно. alt:V читает и правит их прямо в памяти машины у
        // ведущего; у нас по сети едет состояние машины целиком, а колесо в него
        // не входит.
        doesWheelHasTire: { value: absent('vehicle.doesWheelHasTire') },
        getWheelCamber: { value: absent('vehicle.getWheelCamber') },
        getWheelHealth: { value: absent('vehicle.getWheelHealth') },
        getWheelHeight: { value: absent('vehicle.getWheelHeight') },
        getWheelRimRadius: { value: absent('vehicle.getWheelRimRadius') },
        getWheelTrackWidth: { value: absent('vehicle.getWheelTrackWidth') },
        getWheelTyreRadius: { value: absent('vehicle.getWheelTyreRadius') },
        getWheelTyreWidth: { value: absent('vehicle.getWheelTyreWidth') },
        isWheelBurst: { value: absent('vehicle.isWheelBurst') },
        isWheelDetached: { value: absent('vehicle.isWheelDetached') },
        isWheelOnFire: { value: absent('vehicle.isWheelOnFire') },
        setWheelBurst: { value: unperformed('vehicle.setWheelBurst', 'колёса поштучно по сети не едут') },
        setWheelCamber: { value: unperformed('vehicle.setWheelCamber', 'колёса поштучно по сети не едут') },
        setWheelDetached: { value: unperformed('vehicle.setWheelDetached', 'колёса поштучно по сети не едут') },
        setWheelFixed: { value: unperformed('vehicle.setWheelFixed', 'колёса поштучно по сети не едут') },
        setWheelHasTire: { value: unperformed('vehicle.setWheelHasTire', 'колёса поштучно по сети не едут') },
        setWheelOnFire: { value: unperformed('vehicle.setWheelOnFire', 'колёса поштучно по сети не едут') },
        setWheelHealth: { value: unperformed('vehicle.setWheelHealth', 'колёса поштучно по сети не едут') },
        setWheelHeight: { value: unperformed('vehicle.setWheelHeight', 'колёса поштучно по сети не едут') },
        setWheelRimRadius: { value: unperformed('vehicle.setWheelRimRadius', 'колёса поштучно по сети не едут') },
        setWheelTrackWidth: { value: unperformed('vehicle.setWheelTrackWidth', 'колёса поштучно по сети не едут') },
        setWheelTyreRadius: { value: unperformed('vehicle.setWheelTyreRadius', 'колёса поштучно по сети не едут') },
        setWheelTyreWidth: { value: unperformed('vehicle.setWheelTyreWidth', 'колёса поштучно по сети не едут') },

        // Разбор повреждений по частям: бронестёкла, бамперы, пулевые отверстия,
        // разбитые фары. У alt:V это его собственный слепок, который он
        // рассылает; у нас по сети едет прочность кузова, двигателя и бака — и
        // больше ничего.
        getArmoredWindowHealth: { value: absent('vehicle.getArmoredWindowHealth') },
        getArmoredWindowShootCount: { value: absent('vehicle.getArmoredWindowShootCount') },
        getBumperDamageLevel: { value: absent('vehicle.getBumperDamageLevel') },
        getPartBulletHoles: { value: absent('vehicle.getPartBulletHoles') },
        getPartDamageLevel: { value: absent('vehicle.getPartDamageLevel') },
        isLightDamaged: { value: absent('vehicle.isLightDamaged') },
        isSpecialLightDamaged: { value: absent('vehicle.isSpecialLightDamaged') },
        isWindowDamaged: { value: absent('vehicle.isWindowDamaged') },
        setArmoredWindowHealth: { value: unperformed('vehicle.setArmoredWindowHealth', 'разбор повреждений по частям по сети не едет') },
        setArmoredWindowShootCount: { value: unperformed('vehicle.setArmoredWindowShootCount', 'разбор повреждений по частям по сети не едет') },
        setBumperDamageLevel: { value: unperformed('vehicle.setBumperDamageLevel', 'разбор повреждений по частям по сети не едет') },
        setLightDamaged: { value: unperformed('vehicle.setLightDamaged', 'разбор повреждений по частям по сети не едет') },
        setPartBulletHoles: { value: unperformed('vehicle.setPartBulletHoles', 'разбор повреждений по частям по сети не едет') },
        setPartDamageLevel: { value: unperformed('vehicle.setPartDamageLevel', 'разбор повреждений по частям по сети не едет') },
        setSpecialLightDamaged: { value: unperformed('vehicle.setSpecialLightDamaged', 'разбор повреждений по частям по сети не едет') },
        setWindowDamaged: { value: unperformed('vehicle.setWindowDamaged', 'разбор повреждений по частям по сети не едет') },

        // Разное, и причины разные: часть alt:V читает из памяти машины (разгон,
        // торможение), часть принадлежит дополнениям GTA Online, которых сервер
        // не ведёт (ракетное топливо, огнемёт, противоракеты), часть — его
        // собственные затеи (значок на борту, отложенный взрыв).
        accelerationLevel: { get: absent('vehicle.accelerationLevel') },
        activeRadioStation: { get: absent('vehicle.activeRadioStation') },
        attached: { get: absent('vehicle.attached') },
        boatAnchorActive: { get: absent('vehicle.boatAnchorActive') },
        bodyAdditionalHealth: { get: absent('vehicle.bodyAdditionalHealth') },
        brakeLevel: { get: absent('vehicle.brakeLevel') },
        counterMeasureCount: { get: absent('vehicle.counterMeasureCount') },
        driftModeEnabled: { get: absent('vehicle.driftModeEnabled') },
        flamethrowerActive: { get: absent('vehicle.flamethrowerActive') },
        hasTimedExplosion: { get: absent('vehicle.hasTimedExplosion') },
        headlightColor: { get: absent('vehicle.headlightColor') },
        hybridExtraActive: { get: absent('vehicle.hybridExtraActive') },
        hybridExtraState: { get: absent('vehicle.hybridExtraState') },
        lightsMultiplier: { get: absent('vehicle.lightsMultiplier') },
        manualEngineControl: { get: absent('vehicle.manualEngineControl') },
        repairsCount: { get: absent('vehicle.repairsCount') },
        rocketRefuelSpeed: { get: absent('vehicle.rocketRefuelSpeed') },
        scriptMaxSpeed: { get: absent('vehicle.scriptMaxSpeed') },
        timedExplosionCulprit: { get: absent('vehicle.timedExplosionCulprit') },
        timedExplosionTime: { get: absent('vehicle.timedExplosionTime') },
        getWeaponCapacity: { value: absent('vehicle.getWeaponCapacity') },
        setBadge: { value: unperformed('vehicle.setBadge', 'у сервера этого нет') },
        setSearchLightTo: { value: unperformed('vehicle.setSearchLightTo', 'у сервера этого нет') },
        setTimedExplosion: { value: unperformed('vehicle.setTimedExplosion', 'у сервера этого нет') },
        setWeaponCapacity: { value: unperformed('vehicle.setWeaponCapacity', 'у сервера этого нет') },

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
        /// Поворот кватернионом.
        ///
        /// Считается из угла поворота, а не хранится: в снимке машины лежит один
        /// угол вокруг вертикали — крена и тангажа сервер не знает вовсе, и
        /// хранить ради них четыре числа значило бы хранить три нуля.
        ///
        /// Только чтение: присвоенный кватернион пришлось бы разбирать обратно в
        /// один угол, потеряв два остальных, — то есть принять больше, чем мы
        /// умеем исполнить, и промолчать об этом. Поворот ставится через `rot`.
        quaternion: {
            get() {
                // Поворот вокруг вертикали: половина угла в синусе и косинусе.
                const половина = toRadians(this.heading) / 2;

                return new shared.Quaternion(0, 0, Math.sin(половина), Math.cos(половина));
            },
            set(_value) {
                warnOnce('vehicle.quaternion',
                         'поворот задаётся через rot: крена и тангажа сервер не знает');
            },
        },

        /// Закрыта ли складная крыша.
        ///
        /// Выводится из `roofState`, а не хранится вторым числом: два источника
        /// правды об одном разошлись бы молча. Ноль у игры — «поднята», то есть
        /// закрыта; всё остальное — нет.
        ///
        /// Присваивание — распоряжение крыше: `true` поднять, `false` опустить.
        roofClosed: {
            get() { return (this.roofState ?? 0) === 0; },
            set(_value) {
                warnOnce('vehicle.roofClosed',
                         'крышу сервер пока не двигает — она приходит снимком от ведущего');
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
        /// Знает это не игра, а справочник рядом с сервером: `gamedata.bin`,
        /// собираемый `tools/gamedata` из файлов alt:V. Так же отвечает и он
        /// сам — из своего `vehmods.bin`.
        ///
        /// Нет справочника — отказ вслух, а не ноль. Ноль означает «в этом
        /// месте деталей у модели нет», то есть пустое меню тюнинга, и отдать
        /// его вместо «не знаю» значило бы сказать про всякую машину, что
        /// тюнинговать её нечем.
        ///
        /// Самого метода здесь нет: его ставит ядро (`bindings.cpp`,
        /// `vehicleModsCount`). Объяви мы его и здесь — здешний победил бы, и
        /// ядерный стал бы мёртвым кодом.

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
        /// Вся внешность одной строкой — чтобы положить её в базу и достать
        /// обратно. Ровно для этого её и зовут: сохранить тюнинг машины при
        /// выходе игрока и вернуть при входе.
        ///
        /// Запись **наша по составу, а не alt:V**, и подделываться под его
        /// раскладку мы не стали: у него внутри слепок его собственной
        /// структуры, поле в поле, и совпасть с ней можно только случайно. Зато
        /// запись помечена своим именем и своим числом — и чужая, попавшая сюда
        /// из базы, оставшейся от alt:V, отказывает вслух, а не ложится на
        /// машину чем попало. Молчаливо принятая чужая запись дала бы машину со
        /// случайным цветом и случайными деталями, и виноватой выглядела бы
        /// машина, а не запись.
        /// Прочности одной строкой — чтобы положить их в базу и достать обратно.
        ///
        /// Всё то же, что и у внешности: запись наша по составу, помечена своим
        /// именем и числом выпуска, и чужая, оставшаяся от alt:V, отказывает
        /// вслух. Молча принятая, она дала бы машину со случайной прочностью, и
        /// виноватой выглядела бы машина, а не запись.
        getHealthDataBase64: {
            value() {
                return Buffer.from(JSON.stringify({
                    oxymp: kHealthFormat,
                    bodyHealth: this.bodyHealth,
                    engineHealth: this.engineHealth,
                    petrolTankHealth: this.petrolTankHealth,
                }), 'utf8').toString('base64');
            },
        },
        /// Прочность машине сервер не назначает: она живёт в игре у ведущего, и
        /// её собственные свойства отказывают на присваивание по той же причине.
        /// Запись поэтому читается, проверяется, что она наша, — и всё.
        ///
        /// Отказ здесь громче обычного и нарочно: `setHealthDataBase64` зовут
        /// именно затем, чтобы вернуть машине сохранённую прочность, и молчание
        /// в ответ было бы худшим из возможного — режим решил бы, что вернул.
        setHealthDataBase64: {
            value(data) {
                // Разбираем всё равно: чужая запись обязана отказать первой.
                // Иначе режим, кормящий нас чужими записями, узнает о них
                // только тогда, когда прочность когда-нибудь заработает.
                readOwnRecord('vehicle.setHealthDataBase64', data, kHealthFormat);

                warnOnce('vehicle.setHealthDataBase64',
                         'прочность назначает ведущий, а не сервер — запись прочитана, '
                         + 'но не наложена; починить машину целиком умеет repair()');

                return false;
            },
        },

        /// Повреждения кузова и стёкол одной строкой.
        ///
        /// Состав у нас беднее, чем у alt:V: вмятин по частям кузова, дырок от
        /// пуль и здоровья бронестёкол сервер не знает — их считает игра у того,
        /// кто в машине едет, и по сети они не ходят. В записи поэтому лежит то,
        /// что мы знаем: стёкла и двери.
        ///
        /// Сказать об этом нужно вслух: режим, сохранивший запись и не нашедший
        /// в ней вмятин, обязан узнать почему.
        getDamageStatusBase64: {
            value() {
                warnOnce('vehicle.getDamageStatusBase64',
                         'вмятин, дырок и бронестёкол сервер не знает — в записи только '
                         + 'стёкла и двери');

                // Стёкла и двери собираются перебором: маской наружу они не
                // объявлены, а объявлять её вторым способом сказать одно и то же
                // значило бы завести второй источник правды.
                const стёкла = [];
                const двери = [];

                for (let окно = 0; окно < 4; окно += 1) {
                    стёкла.push(this.isWindowOpened(окно) === true);
                }

                for (let дверь = 0; дверь < 6; дверь += 1) {
                    двери.push(this.getDoorState(дверь));
                }

                return Buffer.from(JSON.stringify({
                    oxymp: kDamageFormat,
                    windows: стёкла,
                    doors: двери,
                }), 'utf8').toString('base64');
            },
        },
        setDamageStatusBase64: {
            value(data) {
                const запись = readOwnRecord('vehicle.setDamageStatusBase64', data, kDamageFormat);

                if (Array.isArray(запись.windows)) {
                    запись.windows.forEach((открыто, окно) =>
                        this.setWindowOpened(окно, открыто === true));
                }

                if (Array.isArray(запись.doors)) {
                    запись.doors.forEach((степень, дверь) =>
                        this.setDoorState(дверь, Number(степень) || 0));
                }

                return true;
            },
        },

        /// Состояние игры и скриптовые данные машины у alt:V — слепки его
        /// собственных структур, и своего состава у них нет вовсе. Подделать их
        /// нечем, а отдать пустую строку значило бы сказать «у машины ничего
        /// нет», и режим сохранил бы эту пустоту в базу как правду.
        getGamestateDataBase64: { value: absent('vehicle.getGamestateDataBase64') },
        setGamestateDataBase64: {
            value: unperformed('vehicle.setGamestateDataBase64',
                               'состояние игры у машины — слепок структур alt:V, своего '
                               + 'состава у него нет'),
        },
        getScriptDataBase64: { value: absent('vehicle.getScriptDataBase64') },
        setScriptDataBase64: {
            value: unperformed('vehicle.setScriptDataBase64',
                               'скриптовые данные машины — слепок структур alt:V'),
        },

        getAppearanceDataBase64: {
            value() {
                const worn = look(this);

                return Buffer.from(JSON.stringify({ oxymp: kAppearanceFormat, worn }),
                                   'utf8').toString('base64');
            },
        },
        setAppearanceDataBase64: {
            value(data) {
                let record;

                try {
                    record = JSON.parse(Buffer.from(String(data), 'base64').toString('utf8'));
                } catch (беда) {
                    throw new Error('vehicle.setAppearanceDataBase64: запись не разбирается — '
                                    + беда.message);
                }

                if (record?.oxymp !== kAppearanceFormat) {
                    throw new Error('vehicle.setAppearanceDataBase64: запись не наша или от '
                                    + `другой сборки (ждали ${kAppearanceFormat}, `
                                    + `в записи ${record?.oxymp})`);
                }

                // Целиком, а не полями: чего в записи нет, то и не должно
                // остаться от прежней внешности — иначе восстановленная машина
                // унесла бы с собой деталь, которой у сохранённой не было.
                return reshape(this, (worn) => Object.assign(worn, record.worn));
            },
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
        /// Патроны по роду боеприпаса, а не по стволу.
        ///
        /// У игры и у alt:V патроны общие для всех стволов одного рода: у двух
        /// пистолетов обойма одна на двоих. У нас же снаряжение хранится по
        /// стволам, и род боеприпаса приходится доставать из справочника
        /// (`getWeaponModelInfoByHash`) — оттуда же, откуда его берёт и сама
        /// игра.
        ///
        /// Отсюда и то, чего этот ответ не умеет: разойдись у двух стволов
        /// одного рода счёт патронов, отвечен будет первый. В игре так не
        /// бывает, а у нас может — если режим положил патроны каждому стволу
        /// отдельно.
        getAmmo(ammoHash) {
            const род = Number(ammoHash) >>> 0;

            for (const слот of this.weapons) {
                const сведения = alt.getWeaponModelInfoByHash(слот.hash);

                if (сведения !== null && сведения.ammoTypeHash === род) {
                    return слот.ammo;
                }
            }

            return 0;
        },

        /// Кладёт патроны всем стволам этого рода разом — так же, как это видит
        /// игра.
        setAmmo(ammoHash, amount) {
            const род = Number(ammoHash) >>> 0;
            const сколько = Number(amount) || 0;
            let положено = false;

            for (const слот of this.weapons) {
                const сведения = alt.getWeaponModelInfoByHash(слот.hash);

                if (сведения !== null && сведения.ammoTypeHash === род) {
                    this.setWeaponAmmo(слот.hash, сколько);
                    положено = true;
                }
            }

            return положено;
        },

        /// Пределы, особый род боеприпаса и его признаки. Всё это alt:V держит у
        /// себя и рассылает клиентам; у нас такой синхронизации нет вовсе, и
        /// молчаливое согласие означало бы обещание, которого мы не сдержим.
        getAmmoMax: absent('player.getAmmoMax'),
        setAmmoMax: unperformed('player.setAmmoMax',
                                'предел патронов по роду боеприпаса по сети не едет'),
        getAmmoMax50: absent('player.getAmmoMax50'),
        setAmmoMax50: unperformed('player.setAmmoMax50',
                                  'предел патронов по роду боеприпаса по сети не едет'),
        getAmmoMax100: absent('player.getAmmoMax100'),
        setAmmoMax100: unperformed('player.setAmmoMax100',
                                   'предел патронов по роду боеприпаса по сети не едет'),
        getAmmoSpecialType: absent('player.getAmmoSpecialType'),
        setAmmoSpecialType: unperformed('player.setAmmoSpecialType',
                                        'особый род боеприпаса по сети не едет'),
        getAmmoFlags: absent('player.getAmmoFlags'),
        setAmmoFlags: unperformed('player.setAmmoFlags',
                                  'признаки боеприпаса по сети не едут'),

        /// Облачный вход alt:V. Своей службы входа у нас нет вовсе, и придумать
        /// эти числа значило бы соврать: режим, пускающий по облачному номеру,
        /// пустил бы кого угодно.
        ///
        /// Пустой строкой их отдать тоже нельзя — по той же причине, по которой
        /// `socialID` отдаётся пустым, а не нулём: ноль читается как настоящий
        /// номер. Здесь же и строки не годится: вопрос без ответа обязан
        /// отказать вслух.

        /// Состояние игрока, которого нет в снимке. alt:V рассылает это своей
        /// синхронизацией; у нас по сети едет другое — точка прицела, а не цель,
        /// и направление взгляда, а не поворот головы.

        /// Кровь и грязь на персонаже. У alt:V это двоичный слепок, который
        /// клиент умеет читать и накладывать; у нас его нет ни в снимке, ни в
        /// объявлении внешности.
        getBloodDamageBase64: absent('player.getBloodDamageBase64'),
        setBloodDamageBase64: unperformed('player.setBloodDamageBase64',
                                          'кровь на персонаже по сети не едет'),

        /// Цвета лица из палитры. Читать их у игры нечем — см. правило о том,
        /// что у неё на лицо и цвета только запись, — а слать вслепую значило бы
        /// затирать назначенное.
        getHeadBlendPaletteColor: absent('player.getHeadBlendPaletteColor'),
        setHeadBlendPaletteColor: unperformed('player.setHeadBlendPaletteColor',
                                              'цвета лица из палитры по сети не едут', false),
        removeHeadBlendPaletteColor: unperformed('player.removeHeadBlendPaletteColor',
                                                 'цвета лица из палитры по сети не едут'),

        /// Вещь дополнения парой «набор и место». Пары этой у нас нет и взяться
        /// ей неоткуда — см. `setDlcClothes`; номер вещи здесь сквозной.
        getDlcProp: absent('player.getDlcProp'),

        /// Убрать персонажа из мира, оставив игрока на сервере. У нас персонаж и
        /// игрок — одно: сервер ведёт его положение и рассылает снимки, пока
        /// игрок здесь.
        despawn: unperformed('player.despawn',
                             'персонаж и игрок у нас одно, и убрать одно без другого нельзя'),

        /// Запрет менять ведущего и рассылка имён. Первое у нас относится к
        /// машинам (`vehicle.setNetOwner`), второе не заводилось вовсе: имена
        /// уезжают всем и всегда.
        sendNames: unperformed('player.sendNames', 'имена уезжают всем и всегда'),

        setLocalMeta(key, value) {
            shared._eachMetaPair(key, value, (name, own) => {
                const store = localFor(this.id);
                const было = store.get(name);

                store.set(name, own);
                publishLocal(this, name, own);

                // Объявляется и на сервере: у alt:V `localMetaChange` есть
                // здесь, и подписываются на него именно тут — там, где живёт
                // то, что реагирует на перемену.
                server.fireLocal('localMetaChange', [this, name, own, было]);
            });
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

        /// Снять наложение с лица: бровь, щетину, румяна и прочее.
        ///
        /// «Снять» у игры отдельного натива не имеет: наложение снимается
        /// назначением пустого — номер 255, непрозрачность ноль. Так же делает и
        /// alt:V, и потому это не подмена, а тот же приём.
        removeHeadOverlay(overlayID) {
            return this.setHeadOverlay(Number(overlayID) || 0, kNoOverlay, 0);
        },

        /// Вернуть черте лица её обычный вид. Ноль — середина шкалы от −1 до 1,
        /// то есть «как у всех», а не «нет черты».
        removeFaceFeature(index) {
            return this.setFaceFeature(Number(index) || 0, 0);
        },

        /// Забыть родителей и смешение целиком.
        ///
        /// Нулевые родители с нулевым смешением — это и есть «лица не
        /// назначали»: именно так выглядит персонаж, которому его не задавали.
        removeHeadBlendData() {
            this.setHeadBlendData(0, 0, 0, 0, 0, 0, 0, 0, 0);
        },

        /// Рядом ли эта сущность с игроком — то есть дошла ли она до него по
        /// раздаче.
        ///
        /// Считается расстоянием и дальностью раздачи сервера, а не спросом у
        /// клиента: сервер сам решает, кому что отдавать, и ответ у него точнее.
        /// Своя машина у alt:V всегда «рядом», и здесь так же — она едет вместе
        /// с игроком.
        isEntityInStreamRange(entity) {
            if (entity === null || entity === undefined || entity.valid !== true) {
                return false;
            }

            if (this.vehicle !== null && entity.id === this.vehicle.id) {
                return true;
            }

            const дальность = streamingDistance();
            const мой = this.pos;
            const их = entity.pos;

            const dx = мой.x - их.x;
            const dy = мой.y - их.y;
            const dz = мой.z - их.z;

            return dx * dx + dy * dy + dz * dz <= дальность * дальность;
        },
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
        /// Дальность показа, наложение на землю и вариант текстуры. Всё это
        /// делает игра у того, кто предмет видит; у нас предмет заводится
        /// сервером и рассылается положением, а этих трёх чисел в рассылке нет.
        lodDistance: { get: absent('object.lodDistance') },
        textureVariation: { get: absent('object.textureVariation') },
        placeOnGroundProperly: {
            value: unperformed('object.placeOnGroundProperly',
                               'наложить предмет на землю может только тот, кто его видит'),
        },

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
        /// Поднять, остановить или перезапустить ресурс по имени.
        ///
        /// **Исполняется не сейчас, а перед ближайшим тактом.** Просьба
        /// приходит изнутри изолята одного из ресурсов, а остановка разбирает
        /// изолят целиком — вместе со стеком, по которому в просьбу пришли.
        /// Ресурс, останавливающий сам себя, тем и опасен, что это как раз то,
        /// чего просит панель управления.
        ///
        /// Ответ поэтому означает «просьба принята», а не «сделано»: false —
        /// ресурса с таким именем нет в каталоге вовсе. У alt:V эти три ничего
        /// не возвращают; ответ здесь в пользу режима, а не против — соврать
        /// про несуществующий ресурс мы всё равно не можем.
        startResource: (name) => native.startResource(String(name)),
        stopResource: (name) => native.stopResource(String(name)),
        restartResource: (name) => native.restartResource(String(name)),
        /// Чем сервер объявил себя при запуске.
        ///
        /// Состав — то из `IServerConfig` alt:V, что у нас есть на самом деле.
        /// Чего нет, того здесь нет и в помине: пустая строка вместо адреса
        /// сайта выглядела бы как «сайта нет», а его просто никто не спрашивал.
        ///
        /// Пароля здесь нет и не будет — только `passworded`. Режим, положивший
        /// его в свой журнал или отправивший в своё окно, раздал бы его игрокам.
        /// Настройки сервера, какими их прочли из `server.cfg`.
        ///
        /// Полей у alt:V вчетверо больше, и недостающие здесь **не выдумываются**:
        /// облачный вход, очередь подключений, внешний адрес, границы карты,
        /// рабочие потоки сущностей — всего этого у нас нет как хозяйства, и
        /// подставить туда умолчания alt:V значило бы объявить о возможностях,
        /// которых нет. Отсутствующее поле — правда; выдуманное число — ложь.
        getServerConfig: () => native.serverConfig(),

        /// Что известно о модели машины, человека или оружия.
        ///
        /// Знает это не игра у клиента и не реестры сервера, а справочник рядом
        /// с сервером: это свойства самой модели. Нет справочника — вопрос
        /// отказывает вслух и говорит, чего не хватает; есть, а модели в нём
        /// нет — ответ `null`, и это законный ответ, который режимы и проверяют.
        ///
        /// Принимается и хеш числом, и имя строкой: режимы пишут и так, и так.
        getVehicleModelInfoByHash: (hash) => dressVehicleModel(native.vehicleModelInfo(hash)),
        getPedModelInfoByHash: (hash) => native.pedModelInfo(hash),
        getWeaponModelInfoByHash: (hash) => native.weaponModelInfo(hash),
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
            shared._eachMetaPair(key, value, (name, own) => {
                const store = syncedFor('global', 0);
                const was = store.get(name);

                store.set(name, own);
                publishSynced('global', 0, name, own);
                server.fireLocal('globalSyncedMetaChange', [name, own, was]);
            });
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
            shared._eachMetaPair(key, value, (name, own) => {
                const store = metaFor('global', 0);
                const was = store.get(name);

                store.set(name, own);
                server.fireLocal('globalMetaChange', [name, own, was]);
            });
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
