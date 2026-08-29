// Остальное клиентского API alt:V: статистика, курсор, местные объекты, хранилище.
//
// Собрано отдельным файлом не по смыслу, а по происхождению: это то, что
// понадобилось живым режимам сверх основы. Каждый кусок здесь — ответ на
// конкретный вызов, встреченный в чужом бандле, и потому лучше держать их
// вместе, чем растаскивать по трём файлам.
//
// Правило прежнее: что можно исполнить через игру — исполняется; чего нельзя —
// отказывает вслух.
//
// Файл исполняется после alt_client_objects.js и до alt_client.js.

'use strict';

(function build(alt) {
    const shared = alt.shared;
    const natives = alt.natives;
    const entities = alt.entities;

    // --- Статистика игрока -----------------------------------------------------

    /// Ставит игровую статистику.
    ///
    /// Тип угадывается по значению, а не берётся из таблицы имён. У alt:V такая
    /// таблица есть, и она на несколько тысяч строк; держать её вторую копию
    /// здесь значило бы обязаться сверять их вечно. Ошибиться же угадыванием
    /// трудно: логическое значение и число игра различает сама.
    function setStat(name, value) {
        const key = String(name);

        if (typeof value === 'boolean') {
            return natives.statSetBool(shared.hash(key), value, true) === true;
        }

        if (Number.isInteger(value)) {
            return natives.statSetInt(shared.hash(key), value, true) === true;
        }

        return natives.statSetFloat(shared.hash(key), Number(value) || 0, true) === true;
    }

    function getStat(name) {
        // Выходной довод: игра пишет ответ по указателю.
        const [, значение] = natives.statGetInt(shared.hash(String(name)), 0, -1);
        return значение;
    }

    // --- Курсор ---------------------------------------------------------------

    /// Коды осей курсора среди действий управления.
    ///
    /// Игра не отдаёт положения курсора отдельным нативом: она отдаёт его двумя
    /// «действиями» — теми же, которыми читаются стики и мышь. Значение приходит
    /// долей от нуля до единицы, и в точки экрана его переводим мы.
    const kCursorX = 239;
    const kCursorY = 240;

    function screenSize() {
        // Первое место списка — возврат самого натива; у `void` там пусто.
        const [, ширина, высота] = natives.getScreenResolution(0, 0);
        return new shared.Vector2(ширина, высота);
    }

    function getCursorPos(normalized) {
        // 0 — обычное управление игрока.
        const x = natives.getControlNormal(0, kCursorX);
        const y = natives.getControlNormal(0, kCursorY);

        if (normalized === true) {
            return new shared.Vector2(x, y);
        }

        const размер = screenSize();
        return new shared.Vector2(x * размер.x, y * размер.y);
    }

    function setCursorPos(position) {
        const точка = new shared.Vector2(position);
        const размер = screenSize();

        // Натив ждёт долю, а не точки: приводим обратно.
        natives._setCursorLocation(размер.x === 0 ? 0 : точка.x / размер.x,
                                   размер.y === 0 ? 0 : точка.y / размер.y);
    }

    // --- Признаки персонажа ----------------------------------------------------

    function setConfigFlag(flag, value) {
        const персонаж = natives.playerPedId();

        if (персонаж !== 0) {
            natives.setPedConfigFlag(персонаж, Number(flag) || 0, Boolean(value));
        }
    }

    function getConfigFlag(flag) {
        const персонаж = natives.playerPedId();

        return персонаж !== 0 &&
               natives.getPedConfigFlag(персонаж, Number(flag) || 0, true) === true;
    }

    // --- Местные объекты -------------------------------------------------------

    /// Предмет, заведённый клиентом и видимый только ему.
    ///
    /// «Местный» — то есть не сетевой: сервер о нём не знает, остальные игроки
    /// его не видят. Ровно так же он устроен и в alt:V, и нужен для того же —
    /// для украшений, которые незачем гонять по сети.
    class LocalObject extends entities.Entity {
        constructor(model, pos, rot, noOffset, dynamic) {
            const хеш = typeof model === 'string' ? shared.hash(model) : Number(model) || 0;
            const точка = new shared.Vector3(pos);

            // Модель обязана быть загружена до создания: игра молча вернёт ноль,
            // если её нет в памяти, и предмет не появится без единой жалобы.
            //
            // Заказ здесь синхронный только по видимости: подгрузка занимает
            // кадры, и заказавший в этом же кадре её не дождётся. Отказ поэтому
            // говорит, что делать, — так же, как у местных машин и кукол.
            if (natives.hasModelLoaded(хеш) !== true) {
                natives.requestModel(хеш);

                if (natives.hasModelLoaded(хеш) !== true) {
                    throw new Error(`LocalObject: модель ${хеш} ещё не загружена — закажите её `
                                    + 'через alt.Utils.requestModel и дождитесь');
                }
            }

            const дескриптор = natives.createObject(хеш, точка.x, точка.y, точка.z,
                                                    false, dynamic !== false,
                                                    noOffset === true);

            if (дескриптор === 0) {
                throw new Error(`предмет модели ${хеш} не завёлся: игра отказала`);
            }

            // Заказ отпускается сразу: заведённый предмет игра держит и без
            // него, а заказ держит модель в памяти сам по себе.
            natives.setModelAsNoLongerNeeded(хеш);

            super(дескриптор);

            if (rot !== undefined) {
                const поворот = new shared.Vector3(rot).toDegrees();
                natives.setEntityRotation(дескриптор, поворот.x, поворот.y, поворот.z, 2, true);
            }

            LocalObject.all.push(this);
        }

        static all = [];

        static get count() { return LocalObject.all.length; }

        /// Предметы самого мира — те, что расставила игра, а не мы.
        ///
        /// Отказ, а не пустой список: пустой означал бы «в мире нет ни одного
        /// предмета», и режим, ищущий среди них дверь или мусорку, решил бы,
        /// что их нет, вместо того чтобы узнать, что мы их не считаем.
        static get allWorld() {
            throw new Error('LocalObject.allWorld: предметы самого мира мы не перечисляем — '
                            + 'их находят нативом getClosestObjectOfType');
        }

        /// Заведён ли этот предмет самим миром. Всегда «нет»: заводим их здесь
        /// мы, и это не заглушка, а правда о них.
        get isWorldObject() { return false; }

        get alpha() {
            return this.valid ? natives.getEntityAlpha(this.scriptID) : 0;
        }

        set alpha(value) {
            if (this.valid) {
                // Последний довод — «постепенно», и он выключен: у alt:V
                // прозрачность ставится разом.
                natives.setEntityAlpha(this.scriptID, Number(value) || 0, false);
            }
        }

        resetAlpha() {
            if (this.valid) {
                natives.resetEntityAlpha(this.scriptID);
            }
        }

        get lodDistance() {
            return this.valid ? natives.getEntityLodDist(this.scriptID) : 0;
        }

        set lodDistance(value) {
            if (this.valid) {
                natives.setEntityLodDist(this.scriptID, Number(value) || 0);
            }
        }

        /// Действует ли на предмет тяжесть.
        ///
        /// Только запись: вопроса у игры нет — есть один натив, и тот
        /// распоряжение. Отказ вслух, а не «да»: предмет, которому тяжесть
        /// отключили, ответил бы «падает», и режим, качающий её туда-сюда,
        /// сбился бы на первом же чтении.
        get hasGravity() {
            throw new Error('object.hasGravity: у игры об этом не спросить — натив только ставит');
        }

        set hasGravity(value) {
            if (this.valid) {
                natives.setEntityHasGravity(this.scriptID, value === true);
            }
        }

        /// Столкновения. Та же беда, что и с тяжестью: натив только ставит.
        get isCollisionEnabled() {
            throw new Error('object.isCollisionEnabled: у игры об этом не спросить — '
                            + 'натив только ставит');
        }

        toggleCollision(toggle, keepPhysics) {
            if (this.valid) {
                natives.setEntityCollision(this.scriptID, toggle === true, keepPhysics === true);
            }
        }

        /// Заморожен ли предмет на месте. Снова только запись.
        get positionFrozen() {
            throw new Error('object.positionFrozen: у игры об этом не спросить — '
                            + 'натив только ставит');
        }

        set positionFrozen(value) {
            if (this.valid) {
                natives.freezeEntityPosition(this.scriptID, value === true);
            }
        }

        /// Какой из вариантов раскраски модели надет. Опять только запись.
        get textureVariation() {
            throw new Error('object.textureVariation: у игры об этом не спросить — '
                            + 'натив только ставит');
        }

        set textureVariation(value) {
            if (this.valid) {
                natives.setObjectTextureVariant(this.scriptID, Number(value) || 0);
            }
        }

        /// Ставит предмет на землю под ним.
        placeOnGroundProperly() {
            if (this.valid) {
                natives.placeObjectOnGroundProperly(this.scriptID);
            }
        }

        activatePhysics() {
            if (this.valid) {
                natives.activatePhysics(this.scriptID);
            }
        }

        /// Привязывает предмет к сущности. Доводы — как у alt:V.
        ///
        /// Первым принимается и сущность, и голый дескриптор: у alt:V объявлены
        /// обе формы, и режимы пользуются обеими.
        attachToEntity(entity, boneIndex, offset, rot, useSoftPinning, collision, fixedRot) {
            if (!this.valid) {
                return;
            }

            const кому = typeof entity === 'object' && entity !== null
                ? entity.scriptID : Number(entity) || 0;

            if (кому === 0) {
                throw new Error('object.attachToEntity: первым доводом нужна сущность или её '
                                + 'дескриптор');
            }

            const смещение = new shared.Vector3(offset ?? shared.Vector3.zero);
            const поворот = new shared.Vector3(rot ?? shared.Vector3.zero).toDegrees();

            // Последние доводы игры: «мягкое крепление», «столкновения»,
            // «неподвижный поворот», ось поворота и «привязка к физике». Три
            // первых называет режим, остальные — умолчания самой игры.
            natives.attachEntityToEntity(
                this.scriptID, кому, Number(boneIndex) || 0,
                смещение.x, смещение.y, смещение.z,
                поворот.x, поворот.y, поворот.z,
                useSoftPinning !== false, collision === true, fixedRot !== false,
                false, 2, true);
        }

        detach(dynamic) {
            if (this.valid) {
                // Доводы игры: «применить скорость» и «собрать столкновения».
                // Второй здесь всегда да — отцепленный предмет обязан снова
                // сталкиваться с миром.
                natives.detachEntity(this.scriptID, dynamic !== false, true);
            }
        }

        /// Дождаться появления предмета.
        ///
        /// У alt:V это обещание: там предмет может появиться позже, потому что
        /// его подгружает стриминг. Здесь он появляется в конструкторе или не
        /// появляется вовсе, и обещание исполняется сразу — не заглушкой, а по
        /// существу: ждать нечего.
        waitForSpawn() {
            return Promise.resolve();
        }

        destroy() {
            const дескриптор = this.scriptID;

            if (дескриптор !== 0) {
                // Указатель на дескриптор: натив обнуляет его сам.
                natives.deleteObject(дескриптор);
            }

            const at = LocalObject.all.indexOf(this);
            if (at >= 0) {
                LocalObject.all.splice(at, 1);
            }
        }
    }

    // --- Хранилище -------------------------------------------------------------

    /// Хранилище настроек ресурса, переживающее перезапуск.
    ///
    /// У alt:V оно лежит файлом рядом с клиентом. Здесь — так же, в кеше, по
    /// файлу на ресурс: два режима не должны видеть настроек друг друга.
    ///
    /// Пишется по просьбе (`save`), а не на каждое изменение: режим меняет
    /// десятки ключей подряд, и запись файла на каждый стоила бы кадра.
    class LocalStorage {
        #values = new Map();
        #path = '';

        constructor() {
            const fs = require('fs');
            const path = require('path');

            this.#path = path.join(alt.native.resourcePath, 'localstorage.json');

            try {
                const text = fs.readFileSync(this.#path, 'utf8');

                for (const [key, value] of Object.entries(JSON.parse(text))) {
                    this.#values.set(key, value);
                }
            } catch {
                // Файла нет или он испорчен — начинаем с пустого. Жаловаться
                // незачем: первый запуск ресурса выглядит ровно так же.
            }
        }

        static #only = null;

        /// Хранилище одно на ресурс — так же, как в alt:V.
        static get(...args) {
            if (args.length > 0) {
                // alt:V зовёт это и как `LocalStorage.get(ключ)`, и как
                // `LocalStorage.get()`. Первое — чтение у общего хранилища.
                return LocalStorage.instance.get(args[0]);
            }

            return LocalStorage.instance;
        }

        static get instance() {
            if (LocalStorage.#only === null) {
                LocalStorage.#only = new LocalStorage();
            }

            return LocalStorage.#only;
        }

        get(key) { return this.#values.get(String(key)); }
        has(key) { return this.#values.has(String(key)); }
        set(key, value) { this.#values.set(String(key), value); }
        delete(key) { this.#values.delete(String(key)); }
        deleteAll() { this.#values.clear(); }

        save() {
            try {
                require('fs').writeFileSync(
                    this.#path, JSON.stringify(Object.fromEntries(this.#values)), 'utf8');
            } catch (failure) {
                alt.client.logError('хранилище не сохранилось:', failure);
            }
        }
    }

    // --- Утилиты --------------------------------------------------------------

    /// То, что alt:V складывает в `alt.Utils`.
    const Utils = {
        /// Ждёт заданное время. Обещанием — как в alt:V.
        wait(ms) {
            return new Promise((resolve) => setTimeout(resolve, Number(ms) || 0));
        },

        /// Ждёт, пока условие станет истинным.
        ///
        /// Предел по времени обязателен: условие, которое не сбудется никогда,
        /// иначе оставило бы обещание висеть до конца сессии.
        waitFor(condition, timeout = 2000) {
            const started = Date.now();

            return new Promise((resolve, reject) => {
                const проверить = () => {
                    if (condition()) {
                        resolve();
                        return;
                    }

                    if (Date.now() - started >= timeout) {
                        reject(new Error(`условие не сбылось за ${timeout} мс`));
                        return;
                    }

                    setTimeout(проверить, 0);
                };

                проверить();
            });
        },

        /// Просит игру подгрузить модель и дожидается её.
        async requestModel(model, timeout = 5000) {
            const хеш = typeof model === 'string' ? shared.hash(model) : Number(model) || 0;

            if (natives.hasModelLoaded(хеш) === true) {
                return хеш;
            }

            natives.requestModel(хеш);

            await Utils.waitFor(() => natives.hasModelLoaded(хеш) === true, timeout);
            return хеш;
        },
    };

    alt.extras = {
        setStat,
        getStat,
        getCursorPos,
        setCursorPos,
        setConfigFlag,
        getConfigFlag,
        LocalObject,
        LocalStorage,
        Utils,
    };
})(globalThis.__oxympAlt);
