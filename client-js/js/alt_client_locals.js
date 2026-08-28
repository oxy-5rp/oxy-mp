// Местные сущности и зоны: то, что живёт только у этого игрока.
//
// «Местная» — значит не сетевая: сервер о ней не знает, остальные игроки её не
// видят. У alt:V это отдельная семья классов (`LocalVehicle`, `LocalPed`,
// `WeaponObject`, `Checkpoint`, `Colshape*`), и нужна она затем же, зачем и
// там: украшения, примерочные, подсказки под ногами — всё, что бессмысленно
// гонять по сети.
//
// Отсюда главное свойство этого файла: **сервер здесь не участвует вовсе**. Всё
// делается нативами игры, и потому всё это можно было сделать сразу, не дожидаясь
// ни синхронизации, ни новых сообщений протокола.
//
// Номер у местной сущности свой, а не серверный. Он выдаётся здесь, счётчиком, и
// нужен ровно для `getByID` — так же, как в alt:V. Путать его с номером сессии
// нельзя: они из разных миров и совпадут случайно.
//
// Файл исполняется после alt_client_extras.js и до alt_client.js.

'use strict';

(function build(alt) {
    const shared = alt.shared;
    const natives = alt.natives;
    const entities = alt.entities;

    /// О чём уже предупреждали. По одному разу на повод, а не на вызов: то, что
    /// зовут каждый кадр, залило бы журнал одинаковыми строками.
    const warned = new Set();

    function warnOnce(what, why) {
        if (warned.has(what)) {
            return;
        }

        warned.add(what);
        alt.client.logWarning(`${what}: ${why}`);
    }

    /// Номера местных сущностей. Общий счётчик на все роды — как в alt:V, где
    /// номер выдаёт один реестр объектов на всё.
    let nextId = 1;

    /// Модель по имени или хешу.
    function modelHash(model) {
        return typeof model === 'string' ? shared.hash(model) : Number(model) || 0;
    }

    /// Просит игру подгрузить модель и говорит, дождалась ли.
    ///
    /// Ждать по-настоящему здесь нельзя: конструктор alt:V синхронный, а
    /// подгрузка занимает кадры. Поэтому модель заказывается, и если её ещё нет
    /// — сущность не заводится и говорит об этом. Ресурсы alt:V к этому готовы:
    /// они грузят модель заранее через `alt.Utils.requestModel`.
    function demandModel(хеш) {
        if (natives.hasModelLoaded(хеш) === true) {
            return true;
        }

        natives.requestModel(хеш);
        return natives.hasModelLoaded(хеш) === true;
    }

    // --- Основа местных сущностей ---------------------------------------------

    /// Общее у всего, что клиент завёл сам.
    ///
    /// Реестр здесь общий и статический, а не по классу, и это не экономия:
    /// `getByID` у alt:V ищет среди всех местных сущностей сразу, а классу
    /// достаётся только проверка рода. Разложи мы их по классам — номер,
    /// выданный одним, нашёлся бы и у другого.
    const locals = new Map();

    class LocalEntity extends entities.Entity {
        #id = 0;

        constructor(handle) {
            super(handle);

            this.#id = nextId++;
            locals.set(this.#id, this);

            // Местная сущность — тоже объект слоя, и об её рождении говорится
            // так же, как о рождении метки (`alt_client_objects.js`). Двумя
            // местами, а не одним, потому что основания у них разные: у метки
            // это `BaseObject`, у зоны и у местного тела — `Entity`. Молчать
            // здесь было бы хуже, чем не объявлять вовсе: обработчик, ведущий
            // учёт объектов, недосчитался бы половины и не узнал бы об этом.
            alt.client.emit('baseObjectCreate', this);
        }

        /// Номер местной сущности. Свой, клиентский: серверу его слать нельзя.
        get id() {
            return this.#id;
        }

        get isRemote() {
            return false;
        }

        /// Убирает сущность из мира и из реестра.
        ///
        /// Идемпотентно: alt:V позволяет звать `destroy` дважды, и режимы этим
        /// пользуются — уборка часто идёт и по событию, и по таймеру.
        destroy() {
            const дескриптор = this.scriptID;

            if (дескриптор !== 0) {
                // Ссылкой, а не значением: натив обнуляет дескриптор у
                // вызывающего, и передать копию — значит оставить игре чужую.
                natives.deleteEntity(дескриптор);
            }

            if (locals.delete(this.#id)) {
                // Только если она в реестре и была: `destroy` зовут дважды, и
                // второе объявление о смерти было бы враньём.
                alt.client.emit('baseObjectRemove', this);
            }
        }

        /// Сущность по её местному номеру, если она нужного рода.
        static byId(id, kind) {
            const found = locals.get(Number(id));
            return found instanceof kind ? found : null;
        }

        /// Все заведённые сущности этого рода.
        static allOf(kind) {
            return [...locals.values()].filter((что) => что instanceof kind);
        }
    }

    // --- Машина, которую видит только хозяин ----------------------------------

    /// Подпись у alt:V шестидоводная, но живые режимы зовут её и четырьмя.
    ///
    /// Так и написано в бандле VRUSSIA: `new LocalVehicle('zion', 0, pos, rot)`.
    /// Недостающие два — «пользоваться ли подгрузкой» и «с какого расстояния» —
    /// у alt:V имеют разумные значения по умолчанию, и здесь они те же.
    class LocalVehicle extends LocalEntity {
        constructor(model, dimension, pos, rot, useStreaming, streamingDistance) {
            const хеш = modelHash(model);

            if (!demandModel(хеш)) {
                throw new Error(`LocalVehicle: модель ${хеш} ещё не загружена — ` +
                                'закажите её через alt.Utils.requestModel и дождитесь');
            }

            const точка = new shared.Vector3(pos);
            const поворот = rot === undefined ? shared.Vector3.zero
                                              : new shared.Vector3(rot).toDegrees();

            // Последние два довода: сетевая машина и «принадлежит скрипту». Обе
            // лжи здесь: сети у нас своей нет, а хозяином машина обязана считать
            // нас — иначе игра уберёт её при первой уборке мира.
            const дескриптор = natives.createVehicle(хеш, точка.x, точка.y, точка.z,
                                                     поворот.z, false, false);

            if (дескриптор === 0) {
                throw new Error(`LocalVehicle: игра отказала модели ${хеш}`);
            }

            super(дескриптор);

            natives.setEntityAsMissionEntity(дескриптор, true, true);

            if (rot !== undefined) {
                natives.setEntityRotation(дескриптор, поворот.x, поворот.y, поворот.z, 2, true);
            }

            if (dimension !== undefined && Number(dimension) !== 0) {
                warnOnce('LocalVehicle.dimension',
                         'измерений в oxyMP нет — машина заведена в общем мире');
            }

            if (useStreaming === true || streamingDistance !== undefined) {
                warnOnce('LocalVehicle.streaming',
                         'подгрузка по расстоянию не выполняется — машина стоит всегда');
            }
        }

        static getByID(id) { return LocalEntity.byId(id, LocalVehicle); }
        static get all() { return LocalEntity.allOf(LocalVehicle); }
        static get count() { return LocalVehicle.all.length; }

        get driver() {
            const дескриптор = this.scriptID;

            if (дескриптор === 0) {
                return null;
            }

            const кто = natives.getPedInVehicleSeat(дескриптор, -1, false);
            return кто === 0 ? null : new entities.Ped(кто);
        }

        toString() { return `LocalVehicle{ id: ${this.id} }`; }
    }

    // --- Персонаж, которого видит только хозяин --------------------------------

    /// Тип персонажа для CREATE_PED. Четвёрка — обычный горожанин; тот же, каким
    /// клиент показывает чужих игроков.
    const kCivilianType = 4;

    class LocalPed extends LocalEntity {
        constructor(model, dimension, pos, rot, useStreaming, streamingDistance) {
            const хеш = modelHash(model);

            if (!demandModel(хеш)) {
                throw new Error(`LocalPed: модель ${хеш} ещё не загружена — ` +
                                'закажите её через alt.Utils.requestModel и дождитесь');
            }

            const точка = new shared.Vector3(pos);
            const поворот = rot === undefined ? shared.Vector3.zero
                                              : new shared.Vector3(rot).toDegrees();

            const дескриптор = natives.createPed(kCivilianType, хеш, точка.x, точка.y, точка.z,
                                                 поворот.z, false, false);

            if (дескриптор === 0) {
                throw new Error(`LocalPed: игра отказала модели ${хеш}`);
            }

            super(дескриптор);

            natives.setEntityAsMissionEntity(дескриптор, true, true);

            if (dimension !== undefined && Number(dimension) !== 0) {
                warnOnce('LocalPed.dimension',
                         'измерений в oxyMP нет — персонаж заведён в общем мире');
            }

            if (useStreaming === true || streamingDistance !== undefined) {
                warnOnce('LocalPed.streaming',
                         'подгрузка по расстоянию не выполняется — персонаж стоит всегда');
            }
        }

        static getByID(id) { return LocalEntity.byId(id, LocalPed); }
        static get all() { return LocalEntity.allOf(LocalPed); }
        static get count() { return LocalPed.all.length; }

        toString() { return `LocalPed{ id: ${this.id} }`; }
    }

    // --- Оружие, лежащее в мире ------------------------------------------------

    /// Предмет-оружие: то, что лежит на земле или которое дают персонажу в руки.
    ///
    /// У игры это отдельный натив, а не обычный объект: у оружия свои прицепы —
    /// обвесы, магазин, вид от модели, — и обычный `CREATE_OBJECT` их не заводит.
    class WeaponObject extends LocalEntity {
        constructor(weapon, pos, rot, useStreaming, streamingDistance, ammoCount, model) {
            const хеш = modelHash(weapon);
            const точка = new shared.Vector3(pos);
            const поворот = rot === undefined ? shared.Vector3.zero
                                              : new shared.Vector3(rot).toDegrees();

            // Доводы игры: хеш, патроны, точка, «показать сразу», размер, вид.
            // Последний — «какой из вариантов модели»; ноль означает обычный.
            const дескриптор = natives.createWeaponObject(
                хеш, ammoCount === undefined ? 100 : Number(ammoCount) || 0,
                точка.x, точка.y, точка.z, true, 1.0, model === undefined ? 0 : model);

            if (дескриптор === 0) {
                throw new Error(`WeaponObject: игра отказала оружию ${хеш}`);
            }

            super(дескриптор);

            if (rot !== undefined) {
                natives.setEntityRotation(дескриптор, поворот.x, поворот.y, поворот.z, 2, true);
            }

            if (useStreaming === true || streamingDistance !== undefined) {
                warnOnce('WeaponObject.streaming',
                         'подгрузка по расстоянию не выполняется — оружие лежит всегда');
            }
        }

        static getByID(id) { return LocalEntity.byId(id, WeaponObject); }
        static get all() { return LocalEntity.allOf(WeaponObject); }
        static get count() { return WeaponObject.all.length; }

        /// Отдаёт оружие персонажу в руки.
        giveTo(ped) {
            const кому = typeof ped === 'object' && ped !== null ? ped.scriptID : Number(ped) || 0;

            if (кому !== 0 && this.scriptID !== 0) {
                natives.giveWeaponObjectToPed(this.scriptID, кому);
            }
        }

        toString() { return `WeaponObject{ id: ${this.id} }`; }
    }

    // --- Отметка под ногами ----------------------------------------------------

    /// Цвет, приведённый к четырём числам. Принимает и RGBA, и обычный объект.
    function colorOf(value, fallbackAlpha) {
        if (value === undefined || value === null) {
            return { r: 255, g: 255, b: 255, a: fallbackAlpha };
        }

        return {
            r: Number(value.r) || 0,
            g: Number(value.g) || 0,
            b: Number(value.b) || 0,
            a: value.a === undefined ? fallbackAlpha : Number(value.a) || 0,
        };
    }

    /// Отметка на земле — та самая, к которой игра рисует столб света.
    ///
    /// У alt:V она сущность со своим номером и своими событиями входа. Событий
    /// здесь нет: за вход отвечает зона (`Colshape*`), а отметка только рисует.
    /// Так же они разделены и в игре — это два разных натива, и режимы обычно
    /// заводят их парой.
    class Checkpoint extends LocalEntity {
        #radius = 0;
        #height = 0;

        /// Какой это чекпоинт. Помнится, а не спрашивается: у игры на вид
        /// чекпоинта только запись — обратного натива нет.
        #type = 0;

        constructor(type, pos, nextPos, radius, height, color, iconColor, streamingDistance) {
            const точка = new shared.Vector3(pos);
            const следующая = nextPos === undefined ? точка : new shared.Vector3(nextPos);

            const цвет = colorOf(color, 128);
            const шириной = Number(radius) || 1;

            const дескриптор = natives.createCheckpoint(
                Number(type) || 0,
                точка.x, точка.y, точка.z,
                следующая.x, следующая.y, следующая.z,
                шириной, цвет.r, цвет.g, цвет.b, цвет.a, 0);

            if (дескриптор === 0) {
                throw new Error('Checkpoint: игра отказала');
            }

            super(дескриптор);

            this.#type = Number(type) || 0;
            this.#radius = шириной;
            this.#height = Number(height) || 0;

            if (this.#height > 0) {
                natives.setCheckpointCylinderHeight(дескриптор, this.#height, this.#height,
                                                    шириной);
            }

            if (iconColor !== undefined) {
                const значок = colorOf(iconColor, 255);
                natives.setCheckpointRgba(дескриптор, значок.r, значок.g, значок.b, значок.a);
            }

            if (streamingDistance !== undefined) {
                warnOnce('Checkpoint.streaming',
                         'подгрузка по расстоянию не выполняется — отметка видна всегда');
            }
        }

        static getByID(id) { return LocalEntity.byId(id, Checkpoint); }
        static get all() { return LocalEntity.allOf(Checkpoint); }
        static get count() { return Checkpoint.all.length; }
        get checkpointType() { return this.#type; }

        get radius() { return this.#radius; }
        get height() { return this.#height; }

        /// Убирается своим нативом, а не общим: отметка игре не сущность.
        destroy() {
            const дескриптор = this.scriptID;

            if (дескриптор !== 0) {
                natives.deleteCheckpoint(дескриптор);
            }

            if (locals.delete(this.id)) {
                alt.client.emit('baseObjectRemove', this);
            }
        }

        toString() { return `Checkpoint{ id: ${this.id} }`; }
    }

    // --- Зоны ------------------------------------------------------------------

    /// Зоны, за которыми следим. Отдельно от прочих местных сущностей: их
    /// проверяют каждый кадр, и перебирать ради этого весь реестр незачем.
    const shapes = new Set();

    /// Зона: место в мире, о входе в которое и выходе из которого сообщают.
    ///
    /// Считается здесь, у клиента, и только про своего игрока. Это не упрощение,
    /// а то же, что делает alt:V: его клиентские зоны следят за местным игроком,
    /// а зоны, о которых должен знать сервер, заводятся на сервере (там они уже
    /// есть — см. `script/`).
    /// Объявляет вход и выход двумя именами.
    ///
    /// Нынешний alt:V объявляет `entityEnterColshape` и `entityLeaveColshape`;
    /// `enterColshape` и `leaveColshape` остались от прежних его сборок и от
    /// RAGE MP. Серверная половина объявляет оба с тех пор, как выяснилось, что
    /// зоны считались правильно и оповещали пустоту; клиентская объявляла одно
    /// старое имя ещё долго после этого — та же беда, только на другой стороне.
    ///
    /// Имена написаны здесь целиком, а не собраны из кусков, и это нарочно:
    /// паритет считается сверкой имён с объявлениями alt:V, а спрятанное в
    /// шаблонной строке имя числится отсутствующим.
    function announce(shape, entity, entered) {
        if (entered) {
            alt.client.emit('entityEnterColshape', shape, entity);
            alt.client.emit('enterColshape', shape, entity);
            return;
        }

        alt.client.emit('entityLeaveColshape', shape, entity);
        alt.client.emit('leaveColshape', shape, entity);
    }

    class Colshape extends LocalEntity {
        #inside = false;

        /// Какая это зона. Числами alt:V (`shared.ColShapeType`): наследник
        /// называет свой род сам, потому что общего вопроса «какая ты» у зоны
        /// нет — есть только её собственный класс.
        #type = 0;

        get colshapeType() { return this.#type; }

        /// Считать ли только игроков. У нас зона и так сверяется с одним
        /// игроком — своим, — и признак этот пока ничего не меняет; хранится он
        /// затем, чтобы `zone.playersOnly` отвечал тем, что в него положили, а
        /// не пустотой.
        playersOnly = false;

        /// Заводится без тела: зона — это правило, а не предмет в мире.
        constructor(type) {
            super(0);

            this.#type = Number(type) || 0;
            shapes.add(this);
        }

        /// Внутри ли зоны названная сущность.
        ///
        /// Вопрос про неё, а не признак «внутри ли хоть кто-то»: так объявлено у
        /// alt:V (`isEntityIn(entity)`), и так же считает наша серверная
        /// половина. Признаком это здесь и было — и `zone.isEntityIn(машина)`
        /// падало бы на «true не функция», потому что читалось свойство, а не
        /// звался метод.
        isEntityIn(entity) {
            if (entity === null || entity === undefined || entity.valid !== true) {
                return false;
            }

            return this.isPointIn(entity.pos);
        }

        /// Внутри ли точка. Переопределяется каждой формой.
        isPointIn(_point) { return false; }

        /// Сверяет положение игрока с зоной и объявляет переход.
        ///
        /// Только переход, а не состояние: событие «внутри» каждый кадр было бы
        /// не событием, а опросом, и режим, открывающий по нему окно, открывал
        /// бы его тридцать раз в секунду.
        check(where, player) {
            const внутри = this.isPointIn(where);

            if (внутри === this.#inside) {
                return;
            }

            this.#inside = внутри;
            announce(this, player, внутри);
        }

        destroy() {
            shapes.delete(this);

            if (locals.delete(this.id)) {
                alt.client.emit('baseObjectRemove', this);
            }
        }
    }

    /// Столб: круг на плоскости плюс высота.
    class ColshapeCylinder extends Colshape {
        #x = 0; #y = 0; #z = 0; #radius = 0; #height = 0;

        constructor(x, y, z, radius, height) {
            super(shared.ColShapeType.Cylinder);

            this.#x = Number(x) || 0;
            this.#y = Number(y) || 0;
            this.#z = Number(z) || 0;
            this.#radius = Number(radius) || 0;
            this.#height = Number(height) || 0;
        }

        get pos() { return new shared.Vector3(this.#x, this.#y, this.#z); }
        get radius() { return this.#radius; }
        get height() { return this.#height; }

        isPointIn(point) {
            // Высота проверяется отдельно от круга: столб — это не шар, и
            // игрок этажом выше в него не попадает.
            if (point.z < this.#z || point.z > this.#z + this.#height) {
                return false;
            }

            const dx = point.x - this.#x;
            const dy = point.y - this.#y;

            return dx * dx + dy * dy <= this.#radius * this.#radius;
        }

        toString() { return `ColshapeCylinder{ id: ${this.id} }`; }
    }

    /// Ящик: две противоположные точки.
    class ColshapeCuboid extends Colshape {
        #from = null;
        #to = null;

        constructor(x1, y1, z1, x2, y2, z2) {
            super(shared.ColShapeType.Cuboid);

            // Углы приводятся к «меньший — больший»: режим вправе назвать их в
            // любом порядке, и ящик от этого не должен становиться пустым.
            this.#from = new shared.Vector3(Math.min(x1, x2), Math.min(y1, y2), Math.min(z1, z2));
            this.#to = new shared.Vector3(Math.max(x1, x2), Math.max(y1, y2), Math.max(z1, z2));
        }

        get pos() {
            return new shared.Vector3((this.#from.x + this.#to.x) / 2,
                                      (this.#from.y + this.#to.y) / 2,
                                      (this.#from.z + this.#to.z) / 2);
        }

        isPointIn(point) {
            return point.x >= this.#from.x && point.x <= this.#to.x &&
                   point.y >= this.#from.y && point.y <= this.#to.y &&
                   point.z >= this.#from.z && point.z <= this.#to.z;
        }

        toString() { return `ColshapeCuboid{ id: ${this.id} }`; }
    }

    /// Шар: центр и расстояние.
    class ColshapeSphere extends Colshape {
        #center = null;
        #radius = 0;

        constructor(x, y, z, radius) {
            super(shared.ColShapeType.Sphere);

            this.#center = new shared.Vector3(x, y, z);
            this.#radius = Number(radius) || 0;
        }

        get pos() { return this.#center; }
        get radius() { return this.#radius; }

        isPointIn(point) {
            return this.#center.distanceTo(point) <= this.#radius;
        }

        toString() { return `ColshapeSphere{ id: ${this.id} }`; }
    }

    /// Круг без высоты: столб бесконечной высоты.
    class ColshapeCircle extends Colshape {
        #x = 0; #y = 0; #radius = 0;

        constructor(x, y, radius) {
            super(shared.ColShapeType.Circle);

            this.#x = Number(x) || 0;
            this.#y = Number(y) || 0;
            this.#radius = Number(radius) || 0;
        }

        get radius() { return this.#radius; }

        isPointIn(point) {
            const dx = point.x - this.#x;
            const dy = point.y - this.#y;

            return dx * dx + dy * dy <= this.#radius * this.#radius;
        }

        toString() { return `ColshapeCircle{ id: ${this.id} }`; }
    }

    /// Многоугольник с высотой: границы районов рисуют именно им.
    ///
    /// Тот же, что на сервере (`script-js/js/alt_objects.js`), и считает он так
    /// же — лучевым алгоритмом. Одна и та же зона обязана отвечать одинаково с
    /// обеих сторон: разойдись они, режим показывал бы игроку «вы в районе»
    /// там, где сервер этого не признаёт.
    class ColshapePolygon extends Colshape {
        #minZ = 0;
        #maxZ = 0;
        #points = [];

        constructor(minZ, maxZ, points) {
            super(shared.ColShapeType.Polygon);

            this.#minZ = Number(minZ) || 0;
            this.#maxZ = Number(maxZ) || 0;
            this.#points = (points ?? []).map((each) => new shared.Vector2(each));
        }

        get minZ() { return this.#minZ; }
        get maxZ() { return this.#maxZ; }
        get points() { return this.#points; }

        get pos() {
            if (this.#points.length === 0) {
                return shared.Vector3.zero;
            }

            // Середина по углам, а не по описанной рамке: у alt:V `pos` зоны —
            // место, вокруг которого она нарисована, и для невыпуклого
            // многоугольника середина углов ближе к нему, чем угол рамки.
            let x = 0;
            let y = 0;

            for (const point of this.#points) {
                x += point.x;
                y += point.y;
            }

            return new shared.Vector3(x / this.#points.length, y / this.#points.length,
                                      (this.#minZ + this.#maxZ) / 2);
        }

        /// Лучевой алгоритм: сколько раз луч из точки пересёк границу.
        ///
        /// Нечётное число — точка внутри. Способ старый и надёжный, и берётся он
        /// не из красоты, а из того, что многоугольник здесь может быть и
        /// невыпуклым: границы районов в режимах рисуют как придётся.
        isPointIn(point) {
            const spot = new shared.Vector3(point);

            if (spot.z < this.#minZ || spot.z > this.#maxZ || this.#points.length < 3) {
                return false;
            }

            let inside = false;

            for (let i = 0, j = this.#points.length - 1; i < this.#points.length; j = i++) {
                const a = this.#points[i];
                const b = this.#points[j];

                const crosses = (a.y > spot.y) !== (b.y > spot.y);

                if (crosses && spot.x < ((b.x - a.x) * (spot.y - a.y)) / (b.y - a.y) + a.x) {
                    inside = !inside;
                }
            }

            return inside;
        }

        toString() { return `ColshapePolygon{ id: ${this.id} }`; }
    }

    /// Обходит зоны раз в кадр, пока хоть одна заведена.
    ///
    /// Заводится по требованию и снимается, когда зон не осталось: обход
    /// пустого набора каждый кадр — это вызов натива положения игрока на пустом
    /// месте, а нативы в кадре стоят дороже всего остального здесь.
    let watcher = 0;

    function watchShapes() {
        if (shapes.size === 0) {
            if (watcher !== 0) {
                alt.client.clearEveryTick(watcher);
                watcher = 0;
            }

            return;
        }

        const игрок = entities.local;
        const где = игрок.pos;

        for (const зона of shapes) {
            зона.check(где, игрок);
        }
    }

    /// Зоны заводятся конструктором, а следить за ними надо снаружи: конструктор
    /// не знает, первая ли это зона. Поэтому обход поднимается при первой же
    /// проверке после заведения.
    const startWatching = () => {
        if (watcher === 0 && shapes.size !== 0) {
            watcher = alt.client.everyTick(watchShapes);
        }
    };

    // Обход поднимается лениво: слоя `alt.client` в это мгновение ещё нет — он
    // собирается следующим файлом, — поэтому подписка откладывается на первый
    // такт. Иначе здесь была бы ссылка на то, чего пока не существует.
    const armWatcher = () => setTimeout(startWatching, 0);

    // --- Сборка ----------------------------------------------------------------

    alt.locals = {
        LocalEntity,
        LocalVehicle,
        LocalPed,
        WeaponObject,
        Checkpoint,
        Colshape,
        ColshapeCylinder,
        ColshapeCuboid,
        ColshapeSphere,
        ColshapeCircle,
        ColshapePolygon,

        /// Зову́т снаружи, заводя зону: см. armWatcher.
        armWatcher,

        warnOnce,

        /// Местная сущность по номеру, любого рода. Так `getByID` устроен у alt:V.
        byId: (id) => locals.get(Number(id)) ?? null,
    };
})(globalThis.__oxympAlt);
