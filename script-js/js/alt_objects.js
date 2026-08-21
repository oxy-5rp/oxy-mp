// Серверные объекты alt:V: метки, зоны, чекпоинты, маркеры, голосовые каналы.
//
// Общее у них то, что они существуют на сервере и показываются на клиенте. У
// oxyMP клиентской части alt:V пока нет, и отсюда правило, по которому написан
// весь файл: **то, что можно исполнить на сервере, исполняется по-настоящему,
// а о том, что дошло бы только до клиента, говорится вслух один раз.**
//
// Так, зона (Colshape) здесь работает целиком: сервер знает, где стоят игроки,
// и считает вход и выход сам — обработчики enterColshape и leaveColshape
// получают настоящие события, а не тишину. А метка (Blip) на сервере
// заводится, живёт и отдаётся скрипту, но игрок её пока не увидит, и об этом
// сказано в журнале.
//
// Заглушки, молчащие о том, что ничего не делают, были бы хуже отсутствия: их
// ищут часами. Поэтому здесь их нет.
//
// Файл исполняется после alt_server.js и дополняет __oxympAlt.server.

'use strict';

(function build(alt) {
    const native = alt.native;
    const shared = alt.shared;
    const server = alt.server;

    /// О чём уже предупреждали. По одному разу на повод, а не на объект: режим,
    /// заводящий тысячу меток, залил бы журнал тысячей одинаковых строк.
    const warned = new Set();

    function warnOnce(what) {
        if (warned.has(what)) {
            return;
        }

        warned.add(what);
        server.logWarning(`${what} заводится на сервере, но игроку пока не показывается: ` +
                          'клиентской части alt:V в oxyMP ещё нет');
    }

    /// Общий предок всего, что заводит скрипт и что живёт до destroy().
    ///
    /// Номера свои и сквозные для всех родов: у alt:V они тоже свои у каждого
    /// рода объектов, но сравнивать их между собой всё равно никто не вправе.
    let nextId = 1;

    class BaseObject {
        #alive = true;

        constructor(type) {
            this.id = nextId++;
            this.type = type;

            const kept = BaseObject.registry.get(type);

            if (kept === undefined) {
                BaseObject.registry.set(type, [this]);
            } else {
                kept.push(this);
            }
        }

        static registry = new Map();

        static allOf(type) {
            return (BaseObject.registry.get(type) ?? []).filter((each) => each.valid);
        }

        get valid() {
            return this.#alive;
        }

        destroy() {
            if (!this.#alive) {
                return;
            }

            this.#alive = false;

            const kept = BaseObject.registry.get(this.type);
            const at = kept?.indexOf(this) ?? -1;

            if (at >= 0) {
                kept.splice(at, 1);
            }
        }

        // Метаданные — тем же способом, что и у сущностей: словарь на объект.
        // Здесь его можно держать прямо в объекте, в отличие от игрока: этот
        // объект и есть сам себе хозяин, он не пересоздаётся на каждое обращение.
        #meta = new Map();

        setMeta(key, value) { this.#meta.set(key, value); }
        getMeta(key) { return this.#meta.get(key); }
        hasMeta(key) { return this.#meta.has(key); }
        deleteMeta(key) { this.#meta.delete(key); }
        getMetaKeys() { return [...this.#meta.keys()]; }
    }

    // --- Зоны ----------------------------------------------------------------

    /// Зона в мире: сервер сам следит, кто в неё вошёл и кто вышел.
    ///
    /// Работает по-настоящему, и это возможно ровно потому, что положения
    /// игроков сервер и так знает: они приходят снимками двадцать раз в секунду.
    /// Клиент для этого не нужен вовсе — а значит и ждать его незачем.
    class Colshape extends BaseObject {
        /// Кто сейчас внутри, по номерам игроков.
        ///
        /// Номера, а не объекты: объект-обёртка заводится заново на каждое
        /// обращение к игроку, и сравнивать их между собой нельзя.
        #inside = new Set();

        constructor(shapeType) {
            super('colshape');

            this.colshapeType = shapeType;
            this.playersOnly = false;
            this.dimension = 0;
        }

        /// Внутри ли точка. Переопределяется наследниками.
        isPointIn() {
            return false;
        }

        isEntityIn(entity) {
            return entity?.valid === true && this.#inside.has(entity.id);
        }

        /// Сверяет своё содержимое с положением игроков. Зовётся раз в такт.
        refresh(players) {
            for (const player of players) {
                const here = this.isPointIn(player.pos);
                const was = this.#inside.has(player.id);

                if (here && !was) {
                    this.#inside.add(player.id);
                    server.emit('enterColshape', this, player);
                } else if (!here && was) {
                    this.#inside.delete(player.id);
                    server.emit('leaveColshape', this, player);
                }
            }

            // Ушедшие из сессии убираются отдельно: события выхода из зоны им уже
            // не объявляется — они и из мира вышли, а не из зоны.
            const present = new Set(players.map((player) => player.id));

            for (const id of [...this.#inside]) {
                if (!present.has(id)) {
                    this.#inside.delete(id);
                }
            }
        }
    }

    class ColshapeSphere extends Colshape {
        constructor(x, y, z, radius) {
            super(shared.ColShapeType.Sphere);

            this.pos = new shared.Vector3(x, y, z);
            this.radius = Number(radius) || 0;
        }

        isPointIn(point) {
            return this.pos.distanceToSquared(point) <= this.radius * this.radius;
        }
    }

    class ColshapeCylinder extends Colshape {
        constructor(x, y, z, radius, height) {
            super(shared.ColShapeType.Cylinder);

            this.pos = new shared.Vector3(x, y, z);
            this.radius = Number(radius) || 0;
            this.height = Number(height) || 0;
        }

        isPointIn(point) {
            const spot = new shared.Vector3(point);

            // По высоте — от основания вверх: так цилиндр задаёт и alt:V, и это
            // важно, потому что точку ставят на землю, а не в середину столба.
            if (spot.z < this.pos.z || spot.z > this.pos.z + this.height) {
                return false;
            }

            const dx = spot.x - this.pos.x;
            const dy = spot.y - this.pos.y;

            return (dx * dx) + (dy * dy) <= this.radius * this.radius;
        }
    }

    class ColshapeCircle extends Colshape {
        constructor(x, y, radius) {
            super(shared.ColShapeType.Circle);

            this.pos = new shared.Vector3(x, y, 0);
            this.radius = Number(radius) || 0;
        }

        /// Круг высоты не имеет: он ловит точку по двум осям на любой высоте.
        isPointIn(point) {
            const spot = new shared.Vector3(point);
            const dx = spot.x - this.pos.x;
            const dy = spot.y - this.pos.y;

            return (dx * dx) + (dy * dy) <= this.radius * this.radius;
        }
    }

    class ColshapeCuboid extends Colshape {
        constructor(x1, y1, z1, x2, y2, z2) {
            super(shared.ColShapeType.Cuboid);

            // Углы приводятся к «меньший — больший»: задать коробку задом наперёд
            // — обычная описка, и ловить точку в пустоте из-за неё незачем.
            this.min = new shared.Vector3(Math.min(x1, x2), Math.min(y1, y2), Math.min(z1, z2));
            this.max = new shared.Vector3(Math.max(x1, x2), Math.max(y1, y2), Math.max(z1, z2));
            this.pos = this.min.add(this.max).mul(0.5);
        }

        isPointIn(point) {
            const spot = new shared.Vector3(point);

            return spot.x >= this.min.x && spot.x <= this.max.x &&
                   spot.y >= this.min.y && spot.y <= this.max.y &&
                   spot.z >= this.min.z && spot.z <= this.max.z;
        }
    }

    class ColshapeRectangle extends Colshape {
        constructor(x1, y1, x2, y2) {
            super(shared.ColShapeType.Rectangle);

            this.min = new shared.Vector2(Math.min(x1, x2), Math.min(y1, y2));
            this.max = new shared.Vector2(Math.max(x1, x2), Math.max(y1, y2));
            this.pos = new shared.Vector3((this.min.x + this.max.x) / 2,
                                          (this.min.y + this.max.y) / 2, 0);
        }

        isPointIn(point) {
            const spot = new shared.Vector3(point);

            return spot.x >= this.min.x && spot.x <= this.max.x &&
                   spot.y >= this.min.y && spot.y <= this.max.y;
        }
    }

    class ColshapePolygon extends Colshape {
        constructor(minZ, maxZ, points) {
            super(shared.ColShapeType.Polygon);

            this.minZ = Number(minZ) || 0;
            this.maxZ = Number(maxZ) || 0;
            this.points = (points ?? []).map((each) => new shared.Vector2(each));
        }

        /// Лучевой алгоритм: считаем, сколько раз луч из точки пересёк границу.
        ///
        /// Нечётное число — точка внутри. Способ старый и надёжный, и берётся он
        /// не из красоты, а из того, что многоугольник здесь может быть и
        /// невыпуклым: границы районов в игровых режимах рисуют как придётся.
        isPointIn(point) {
            const spot = new shared.Vector3(point);

            if (spot.z < this.minZ || spot.z > this.maxZ || this.points.length < 3) {
                return false;
            }

            let inside = false;

            for (let i = 0, j = this.points.length - 1; i < this.points.length; j = i++) {
                const a = this.points[i];
                const b = this.points[j];

                const crosses = (a.y > spot.y) !== (b.y > spot.y);

                if (crosses &&
                    spot.x < ((b.x - a.x) * (spot.y - a.y)) / (b.y - a.y) + a.x) {
                    inside = !inside;
                }
            }

            return inside;
        }
    }

    /// Чекпоинт — это зона, которая вдобавок рисуется.
    ///
    /// Наследование от зоны не для стройности: у alt:V чекпоинт и правда даёт
    /// события входа и выхода, и режимы пользуются именно ими. Работать они будут
    /// целиком; невидимой пока остаётся только сама нарисованная фигура.
    class Checkpoint extends ColshapeCylinder {
        constructor(checkpointType, x, y, z, radius, height, r, g, b, a) {
            super(x, y, z, radius, height);

            this.checkpointType = checkpointType;
            this.color = new shared.RGBA(r, g, b, a);

            warnOnce('чекпоинт');
        }
    }

    // --- Показываемое --------------------------------------------------------

    /// Метка на карте.
    class Blip extends BaseObject {
        /// Номер метки у сервера. Ноль — метка ещё не заведена или уже убрана.
        ///
        /// Свой, отдельно от `id`: тот принадлежит BaseObject и сквозной для
        /// всех родов, как и у alt:V, а этим меткой распоряжается сервер.
        #serverId = 0;

        /// Пока метка собирается, отправлять нечего: конструктор наследника
        /// допишет ей размер или радиус, и уехавшая раньше метка мигнула бы у
        /// всех дважды.
        #settled = false;

        constructor(x, y, z) {
            super('blip');

            this.pos = new shared.Vector3(x, y, z);
            this.dimension = 0;

            // Значения по умолчанию — те же, что у alt:V: режим, задавший только
            // положение, ожидает обычную белую точку, а не невидимку.
            this.sprite = 1;
            this.color = 0;
            this.alpha = 255;
            this.name = '';
            this.scale = 1;
            this.shortRange = false;
            this.route = false;
            this.routeColor = 0;
            this.display = 2;
        }

        /// Отдаёт метку серверу: заводит или поправляет.
        ///
        /// Зовётся вручную, а не из присваивания каждому полю, и это не
        /// небрежность. Ресурс правит метку помногу сразу — покрасил,
        /// переименовал, подвинул, — и отправка на каждое поле означала бы
        /// четыре сообщения вместо одного и метку, поправленную наполовину,
        /// у всех, кто её видит.
        ///
        /// Наблюдать за полями через посредника мы не стали намеренно: alt:V
        /// отдаёт метку обычным объектом, и всякий, кто её у себя сохранит,
        /// сравнит или положит ключом, ждёт именно объекта.
        settle() {
            this.#settled = true;
            this.#send();
        }

        #describe() {
            return {
                position: this.pos,
                sprite: this.sprite,
                color: this.color,
                alpha: this.alpha,
                display: this.display,
                scale: this.scale,
                shortRange: this.shortRange,
                name: this.name,
                dimension: this.dimension,
            };
        }

        #send() {
            if (!this.#settled) {
                return;
            }

            if (this.#serverId === 0) {
                const id = native.createBlip(this.#describe());

                if (id === null) {
                    server.logWarning('метка не поставлена: в сессии их столько, ' +
                                      'сколько разрешено настройкой maxblips');
                    return;
                }

                this.#serverId = id;
                return;
            }

            native.updateBlip(this.#serverId, this.#describe());
        }

        /// Отдаёт серверу то, что ресурс успел поправить.
        ///
        /// У alt:V метка обновляется сама, потому что живёт у него объектом с
        /// наблюдаемыми полями. Здесь она обычный объект, и о правке нужно
        /// сказать — иначе она останется у ресурса и никуда не уедет.
        update() {
            this.#send();
        }

        destroy() {
            if (this.#serverId !== 0) {
                native.removeBlip(this.#serverId);
                this.#serverId = 0;
            }

            super.destroy();
        }

        attachTo() {
            // Привязка метки к сущности живёт целиком на клиенте: там она за ней
            // и ездит. Сказать об этом честнее, чем принять и не сделать.
            throw new Error('blip.attachTo: в oxyMP этого ещё нет');
        }
    }

    class PointBlip extends Blip {
        constructor(x, y, z) {
            // alt:V принимает и точку, и три числа.
            if (typeof x === 'object' && x !== null) {
                const point = new shared.Vector3(x);
                super(point.x, point.y, point.z);
                this.settle();
                return;
            }

            super(x, y, z);
            this.settle();
        }
    }

    class AreaBlip extends Blip {
        constructor(x, y, z, width, height) {
            super(x, y, z);

            this.scaleXY = new shared.Vector2(width, height);
            this.settle();
        }
    }

    class RadiusBlip extends Blip {
        constructor(x, y, z, radius) {
            super(x, y, z);

            this.radius = Number(radius) || 0;
            this.settle();
        }
    }

    /// Маркер — фигура, нарисованная в мире.
    class Marker extends BaseObject {
        constructor(markerType, pos, color) {
            super('marker');

            this.markerType = markerType;
            this.pos = new shared.Vector3(pos);
            this.color = color === undefined ? shared.RGBA.white : new shared.RGBA(color);
            this.dimension = 0;
            this.visible = true;
            this.scale = new shared.Vector3(1, 1, 1);
            this.rot = shared.Vector3.zero;
            this.direction = shared.Vector3.zero;

            warnOnce('маркер');
        }
    }

    // --- Голос ---------------------------------------------------------------

    /// Голосовой канал.
    ///
    /// Заводится и ведёт список участников по-настоящему — на этом держится вся
    /// логика режима: кто кого слышит, кто в рации, кто в машине. Самого звука
    /// oxyMP пока не передаёт вовсе.
    class VoiceChannel extends BaseObject {
        #members = new Set();

        constructor(spatial, maxDistance) {
            super('voiceChannel');

            this.isSpatial = Boolean(spatial);
            this.maxDistance = Number(maxDistance) || 0;

            warnOnce('голосовой канал');
        }

        addPlayer(player) {
            if (player?.valid) {
                this.#members.add(player.id);
            }
        }

        removePlayer(player) {
            if (player !== null && player !== undefined) {
                this.#members.delete(player.id);
            }
        }

        hasPlayer(player) {
            return player?.valid === true && this.#members.has(player.id);
        }

        get players() {
            return native.players().filter((player) => this.#members.has(player.id));
        }

        mutePlayer() { throw new Error('voiceChannel.mutePlayer: в oxyMP этого ещё нет'); }
        unmutePlayer() { throw new Error('voiceChannel.unmutePlayer: в oxyMP этого ещё нет'); }
        isPlayerMuted() { throw new Error('voiceChannel.isPlayerMuted: в oxyMP этого ещё нет'); }
    }

    // --- Сверка зон ----------------------------------------------------------

    /// Сверяет зоны с положением игроков каждый такт.
    ///
    /// В один проход по всем зонам, а не по таймеру на зону: режим заводит их
    /// сотнями, и сотня таймеров стоила бы дороже самой сверки. Пустой проход при
    /// отсутствии зон не стоит ничего — потому и заведён сразу, а не по первой
    /// заведённой зоне.
    server.on('tick', () => {
        const shapes = BaseObject.allOf('colshape');
        if (shapes.length === 0) {
            return;
        }

        const players = native.players();

        for (const shape of shapes) {
            shape.refresh(players);
        }
    });

    Object.assign(server, {
        BaseObject,
        Colshape,
        ColshapeSphere,
        ColshapeCylinder,
        ColshapeCircle,
        ColshapeCuboid,
        ColshapeRectangle,
        ColshapePolygon,
        Checkpoint,
        Blip,
        PointBlip,
        AreaBlip,
        RadiusBlip,
        Marker,
        VoiceChannel,
    });
})(globalThis.__oxympAlt);
