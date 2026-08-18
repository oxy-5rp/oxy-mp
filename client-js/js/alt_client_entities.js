// Сущности на клиенте: то, что игрок видит вокруг себя.
//
// Собраны поверх нативов, а не поверх сети, и это осознанный выбор. У alt:V
// клиентская сущность — отражение серверной, приходящее по своему каналу
// синхронизации; такого канала у oxyMP пока нет. Зато есть сама игра, которая
// про каждого персонажа и каждую машину знает всё: где стоит, сколько здоровья,
// какая модель.
//
// Отсюда правило этого файла: **свойство, которое можно спросить у игры,
// спрашивается у игры**. Что нельзя — отказывает вслух. Ни одно свойство не
// придумывается и не кешируется: игра меняет их каждый кадр, а сохранённое
// разошлось бы с настоящим к следующему.
//
// Файл исполняется после alt_natives.js и до alt_client.js.

'use strict';

(function build(alt) {
    const shared = alt.shared;
    const natives = alt.natives;

    function absent(what) {
        return function () {
            throw new Error(`${what}: в oxyMP этого ещё нет`);
        };
    }

    /// Дескриптор игры, спрятанный от скрипта.
    ///
    /// Прятать нужно затем, что дескриптор — не номер сущности. Номер выдаёт
    /// сервер, и он один для всех; дескриптор выдаёт игра, и у каждого игрока он
    /// свой. Ресурс, отправивший дескриптор на сервер, получил бы там ерунду, и
    /// понять почему было бы не по чему.
    const handles = new WeakMap();

    // --- Основа ---------------------------------------------------------------

    /// Общее у всего, что стоит в мире.
    class WorldObject {
        constructor(handle) {
            handles.set(this, Number(handle) || 0);
        }

        /// Дескриптор игры. Нативам он и нужен.
        ///
        /// Зовётся `scriptID` — так же, как в alt:V, и по той же причине: имя
        /// напоминает, что число это принадлежит игре, а не сессии.
        get scriptID() {
            return handles.get(this) ?? 0;
        }

        get valid() {
            const handle = this.scriptID;
            return handle !== 0 && natives.doesEntityExist(handle) === true;
        }

        get pos() {
            if (!this.valid) {
                return shared.Vector3.zero;
            }

            // true — «от середины», а не от ног: так координату отдаёт alt:V, и
            // ресурсы считают расстояния именно от неё.
            return new shared.Vector3(natives.getEntityCoords(this.scriptID, true));
        }

        get dimension() {
            return 0;
        }

        set dimension(_value) {
            throw new Error('entity.dimension: в oxyMP этого ещё нет');
        }
    }

    /// Всё, что игра считает сущностью: персонаж, машина, предмет.
    class Entity extends WorldObject {
        get id() {
            // Номера сессии у клиентской сущности пока нет: их раздаёт сервер, а
            // канала для этого в протоколе ещё не заведено. Отказ честнее нуля —
            // ноль ресурс отправил бы серверу как настоящий номер.
            throw new Error('entity.id: номера сессии на клиенте ещё нет');
        }

        get model() {
            return this.valid ? natives.getEntityModel(this.scriptID) : 0;
        }

        get rot() {
            if (!this.valid) {
                return shared.Vector3.zero;
            }

            // Игра отдаёт поворот в градусах, alt:V — в радианах.
            return new shared.Vector3(natives.getEntityRotation(this.scriptID, 2)).toRadians();
        }

        get heading() {
            return this.valid ? natives.getEntityHeading(this.scriptID) : 0;
        }

        get health() {
            return this.valid ? natives.getEntityHealth(this.scriptID) : 0;
        }

        get maxHealth() {
            return this.valid ? natives.getEntityMaxHealth(this.scriptID) : 0;
        }

        get frozen() {
            // Спросить у игры об этом нечем: натива-вопроса нет, есть только
            // натив-распоряжение.
            throw new Error('entity.frozen: у игры об этом не спросить');
        }

        get visible() {
            return this.valid ? natives.isEntityVisible(this.scriptID) === true : false;
        }

        get isDead() {
            return this.valid ? natives.isEntityDead(this.scriptID, false) === true : true;
        }

        get speed() {
            return this.valid ? natives.getEntitySpeed(this.scriptID) : 0;
        }

        get velocity() {
            return this.valid ? new shared.Vector3(natives.getEntityVelocity(this.scriptID))
                              : shared.Vector3.zero;
        }

        /// Расстояние до точки или до другой сущности.
        distanceTo(other) {
            const there = other instanceof WorldObject ? other.pos : other;
            return this.pos.distanceTo(there);
        }

        setMeta() {
            // Своя метаданная на клиенте не заведена: у alt:V она принадлежит
            // сущности, а сущность здесь — отражение чужой, живущее один вызов.
            throw new Error('entity.setMeta: на клиенте этого ещё нет');
        }

        /// Номер сущности в сессии — тот, которым её зовёт сервер.
        ///
        /// Есть он пока у одного: у своего персонажа. Остальных клиент видит
        /// дескрипторами игры, а переводчика между дескриптором и номером
        /// сессии ещё нет — для этого нужен канал синхронизации сущностей.
        get sessionId() {
            return -1;
        }

        getSyncedMeta(key) {
            const номер = this.sessionId;

            if (номер < 0) {
                // Отказ, а не undefined: второе выглядело бы как «ключа нет», и
                // режим искал бы ошибку на сервере, где её нет.
                throw new Error('entity.getSyncedMeta: номер этой сущности клиенту ' +
                                'неизвестен — сервер называет её своим номером, а здесь ' +
                                'дескриптор игры');
            }

            return alt.readSyncedMeta('player', номер, key);
        }

        getSyncedMetaKeys() {
            const номер = this.sessionId;
            return номер < 0 ? [] : alt.readSyncedMetaKeys('player', номер);
        }

        hasSyncedMeta(key) {
            return this.getSyncedMeta(key) !== undefined;
        }

        toString() {
            return `Entity{ scriptID: ${this.scriptID} }`;
        }
    }

    // --- Персонажи ------------------------------------------------------------

    class Ped extends Entity {
        get armour() {
            return this.valid ? natives.getPedArmour(this.scriptID) : 0;
        }

        get currentWeapon() {
            if (!this.valid) {
                return 0;
            }

            // Оружие возвращается выходным доводом, поэтому ответ приходит
            // списком: сперва то, что вернул сам натив, затем записанное.
            const [, оружие] = natives.getCurrentPedWeapon(this.scriptID, 0, true);
            return оружие;
        }

        get vehicle() {
            if (!this.valid) {
                return null;
            }

            // false — «не считать машину, к которой он только идёт».
            const handle = natives.getVehiclePedIsIn(this.scriptID, false);

            return handle === 0 ? null : new Vehicle(handle);
        }

        get seat() {
            const car = this.vehicle;
            if (car === null) {
                return -1;
            }

            // Места считаются от −1 (водитель). Перебор — единственный способ:
            // натива «на каком месте сидит» у игры нет.
            for (let место = -1; место < 8; место++) {
                if (natives.getPedInVehicleSeat(car.scriptID, место, false) === this.scriptID) {
                    return место;
                }
            }

            return -1;
        }

        toString() {
            return `Ped{ scriptID: ${this.scriptID} }`;
        }
    }

    /// Свой персонаж.
    ///
    /// Единственная сущность, которую клиент знает наверняка: игра сама говорит,
    /// кто это. Всех остальных пришлось бы разрешать через сеть, а канала для
    /// этого ещё нет.
    class LocalPlayer extends Ped {
        constructor() {
            super(natives.playerPedId());
        }

        /// Дескриптор берётся заново на каждое обращение.
        ///
        /// Так надо: после смерти и возрождения игра выдаёт персонажу **новый**
        /// дескриптор, а старый перестаёт существовать. Сохранённый однажды, он
        /// сделал бы `alt.Player.local` мёртвым до конца сессии.
        get scriptID() {
            return natives.playerPedId();
        }

        get name() {
            return natives.getPlayerName(natives.playerId());
        }

        /// Свой номер в сессии — тот, которым нас зовёт сервер.
        get sessionId() {
            return alt.selfId();
        }

        /// alt:V зовёт его просто `id`. У остальных сущностей его пока нет.
        get id() {
            return alt.selfId();
        }

        get isTalking() { throw new Error('player.isTalking: в oxyMP этого ещё нет'); }

        toString() {
            return `LocalPlayer{ name: ${this.name} }`;
        }
    }

    // --- Машины ---------------------------------------------------------------

    class Vehicle extends Entity {
        get driver() {
            if (!this.valid) {
                return null;
            }

            const handle = natives.getPedInVehicleSeat(this.scriptID, -1, false);
            return handle === 0 ? null : new Ped(handle);
        }

        get engineOn() {
            return this.valid ? natives.getIsVehicleEngineRunning(this.scriptID) === true : false;
        }

        get speed() {
            return this.valid ? natives.getEntitySpeed(this.scriptID) : 0;
        }

        get bodyHealth() {
            return this.valid ? natives.getVehicleBodyHealth(this.scriptID) : 0;
        }

        get engineHealth() {
            return this.valid ? natives.getVehicleEngineHealth(this.scriptID) : 0;
        }

        get numberPlateText() {
            return this.valid ? natives.getVehicleNumberPlateText(this.scriptID) : '';
        }

        toString() {
            return `Vehicle{ scriptID: ${this.scriptID} }`;
        }
    }

    // --- Сборка ---------------------------------------------------------------

    /// Свой персонаж заводится один раз и живёт до конца сессии.
    ///
    /// Один объект, а не новый на каждое обращение: ресурсы кладут его в
    /// переменную и сравнивают с другими через `===`. Дескриптор при этом
    /// спрашивается заново — см. LocalPlayer.scriptID.
    let local = null;

    alt.entities = {
        WorldObject,
        Entity,
        Ped,
        Vehicle,
        LocalPlayer,

        get local() {
            if (local === null) {
                local = new LocalPlayer();
            }

            return local;
        },

        /// Сущность по дескриптору игры.
        ///
        /// Нужна затем, что нативы отдают именно дескрипторы: нашли машину
        /// поблизости — получили число, и обернуть его во что-то осмысленное
        /// больше нечем.
        fromScriptID(handle) {
            const number = Number(handle) || 0;

            if (number === 0 || natives.doesEntityExist(number) !== true) {
                return null;
            }

            if (natives.isEntityAVehicle(number) === true) {
                return new Vehicle(number);
            }

            if (natives.isEntityAPed(number) === true) {
                return new Ped(number);
            }

            return new Entity(number);
        },

        absent,
    };
})(globalThis.__oxympAlt);
