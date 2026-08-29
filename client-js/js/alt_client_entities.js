// Сущности на клиенте: то, что игрок видит вокруг себя.
//
// У сущности здесь два имени, и путать их нельзя. Номер выдаёт сервер — он один
// для всех, им сущность называют в событиях и метаданных. Дескриптор выдаёт
// игра — он у каждого игрока свой, и только им можно позвать натив. Переводчик
// между ними живёт в клиенте (`client/src/game_session.cpp`), и слой сущностей
// спрашивает его через мостик: `native.sessionEntities`, `sessionHandle`,
// `sessionId`.
//
// Пока переводчика не было, клиент знал вокруг себя одного — себя, — и всё
// остальное отсюда и росло: `Player.all` из одного, пустой `Vehicle.all`, отказ
// `getSyncedMeta` у чужих.
//
// Правило файла осталось прежним: **свойство, которое можно спросить у игры,
// спрашивается у игры**. Ни одно свойство не придумывается и не кешируется:
// игра меняет их каждый кадр, а сохранённое разошлось бы с настоящим к
// следующему. Помнится здесь ровно одно — тождество самих объектов, и почему
// именно оно, объяснено у реестра ниже.
//
// Файл исполняется после alt_natives.js и до alt_client.js.

'use strict';

(function build(alt) {
    const shared = alt.shared;

    /// Хеш оружия или обвеса: числом как есть, именем — посчитанным.
    ///
    /// У alt:V всё оружейное принимается обоими видами, и режимы пишут
    /// `hasWeapon('weapon_pistol')` не реже, чем с хешем.
    function weaponHash(weapon) {
        return typeof weapon === 'string' ? shared.hash(weapon) : Number(weapon) >>> 0;
    }

    /// Жалоба, сказанная один раз.
    ///
    /// Один раз, а не на каждый вызов: распоряжения эти зовут из кадра, и
    /// тридцать строк в секунду завалили бы журнал так, что в нём не осталось бы
    /// ничего другого.
    const warned = new Set();

    function warnOnce(what, why) {
        if (warned.has(what)) {
            return;
        }

        warned.add(what);
        alt.client.logWarning(`${what}: ${why}`);
    }

    /// Хеш, приведённый к беззнаковому.
    ///
    /// Нативы отдают хеш знаковым числом: у безоружного он выходит
    /// `-1569615261` вместо `2725352035`. А `alt.hash` считает беззнаковый, и
    /// режимы сравнивают одно с другим через `===`.
    ///
    /// **Молчит это полностью**: оба числа законны, исключения нет, а
    /// `player.currentWeapon === alt.hash('weapon_unarmed')` не сходится ни
    /// разу. Виноватым при этом выглядит что угодно — оружие, игрок, порядок
    /// событий, — только не знак.
    ///
    /// Всякий хеш, уходящий из слоя наружу, проходит здесь.
    function asHash(value) {
        return Number(value) >>> 0;
    }


    /// Признаки состояния игрока — те же номера битов, что и у протокола
    /// (`shared::PlayerFlag`). Закреплены проверкой в `shared/tests`: перестань
    /// они совпадать — игрок у всех остальных почему-то поплывёт вместо того,
    /// чтобы целиться, и ошибки при этом не будет никакой.
    const kPlayerFlag = {
        Dead: 1 << 0,
        Aiming: 1 << 1,
        Shooting: 1 << 2,
        Ragdoll: 1 << 3,
        Jumping: 1 << 4,
        InVehicle: 1 << 5,
        Crouching: 1 << 6,
        Climbing: 1 << 7,
        Vaulting: 1 << 8,
        Swimming: 1 << 9,
        Diving: 1 << 10,
        Falling: 1 << 11,
        Parachuting: 1 << 12,
        Reloading: 1 << 13,
        InCover: 1 << 14,
        Melee: 1 << 15,
        GettingUp: 1 << 16,
        DriveBy: 1 << 17,
        EnteringVehicle: 1 << 18,
        LeavingVehicle: 1 << 19,
        OnVehicle: 1 << 20,
    };
    const natives = alt.natives;
    const native = alt.native;

    function absent(what) {
        return function () {
            throw new Error(`${what}: в oxyMP этого ещё нет`);
        };
    }

    /// Роды сущностей сессии — теми же числами, что и на границе с клиентом
    /// (`OxympJsEntityKind` в `abi.h`). Разъедутся — игроки станут машинами.
    const kPlayerKind = 0;
    const kVehicleKind = 1;

    /// Прохожие, заведённые сервером. Не те, что расставила игра: у этих есть
    /// номер сессии, они одни и те же у всех игроков.
    const kPedKind = 2;

    /// Как род зовётся в метаданных. Те же слова, что и на сервере: разойдись
    /// они — `syncedMeta` перестала бы находиться, и без единой жалобы.
    const kMetaName = {
        [kPlayerKind]: 'player',
        [kVehicleKind]: 'vehicle',
        [kPedKind]: 'ped',
    };

    /// Место в машине: у игры своя нумерация, у alt:V своя.
    ///
    /// У игры водитель — минус единица, пассажиры с нуля; у alt:V водитель —
    /// единица, пассажиры с двойки, а ноль означает «ни в какой машине».
    /// Смещение на двойку переводит одно в другое целиком, и оно же стоит на
    /// сервере (`script-js/include/oxymp/script/js/alt_seat.hpp`) — две стороны
    /// обязаны считать места одинаково.
    ///
    /// Ошибка здесь молчит: числа обеих нумераций законны, и перепутанные они не
    /// дают ни исключения, ни строки в журнале.
    const kSeatShift = 2;
    const kNowhere = 0;

    function toAltSeat(seat) {
        return seat + kSeatShift;
    }

    /// Дескриптор игры, спрятанный от скрипта.
    ///
    /// Прятать нужно затем, что дескриптор — не номер сущности. Ресурс,
    /// отправивший дескриптор на сервер, получил бы там ерунду, и понять почему
    /// было бы не по чему.
    const handles = new WeakMap();

    /// Номер сессии у тех сущностей, у кого он есть, — вместе с родом.
    ///
    /// Держится номер, а не дескриптор, и это существенно. Тело игра вправе
    /// убрать и завести заново — при подгрузке мира, при смерти, просто по
    /// своему усмотрению, — и дескриптор при этом меняется. Сохранённый однажды,
    /// он указывал бы в никуда до конца сессии; так и было у своего персонажа,
    /// пока `LocalPlayer.scriptID` не стал спрашиваться заново.
    const numbers = new WeakMap();

    /// Наш номер в сессии. −1 — сервер ещё не принял.
    function selfId() {
        return native.selfId();
    }

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
        ///
        /// У сущности сессии спрашивается заново на каждое обращение: тело у неё
        /// сменное. Ноль означает «тела здесь ещё нет» — игрок стоит далеко или
        /// его модель ещё грузится. Это не поломка: у alt:V такая сущность тоже
        /// есть, и `scriptID` у неё тоже ноль.
        get scriptID() {
            const свой = numbers.get(this);

            if (свой !== undefined) {
                return native.sessionHandle(свой.kind, свой.id);
            }

            return handles.get(this) ?? 0;
        }

        get valid() {
            const handle = this.scriptID;
            return handle !== 0 && natives.doesEntityExist(handle) === true;
        }

        /// Отчёт о синхронизации сущности: сколько пакетов о ней пришло, когда
        /// последний, что в нём было. У alt:V это его собственный счётчик
        /// внутри его же синхронизации; у нас сущности ведутся снимками, и
        /// такого счёта рядом с ними никто не держит.
        getSyncInfo() {
            throw new Error('entity.getSyncInfo: своего счёта пакетов по сущности у нас нет');
        }

        /// Кто эту сущность ведёт — то есть у кого она живёт в игре.
        ///
        /// У машины это её ведущий, у игрока — он сам, у прохожего — никто:
        /// прохожими целиком распоряжается сервер. Заведённое клиентом тоже
        /// отвечает «никто»: сервер о нём не знает вовсе.
        get netOwner() {
            return ownerOf(this);
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
            // Измерения на сервере есть и работают, а на клиенте их нет и не
            // будет: он про них не знает вовсе, и это не пробел, а решение. Не
            // рассказать клиенту о том, чего он не должен видеть, — единственный
            // способ не дать ему это увидеть; расскажи мы, а прятать поручи
            // ему, — и первый же изменённый клиент увидел бы всех.
            //
            // Отсюда и отказ: переставить сущность в другой слой мира вправе
            // только сервер. Молчание было бы хуже — режим решил бы, что
            // переставил, и собрал бы всех в одной комнате.
            throw new Error('entity.dimension: измерения переставляет сервер, не клиент');
        }
    }

    /// Всё, что игра считает сущностью: персонаж, машина, предмет.
    /// Своя метаданная сущностей этой машины.
    ///
    /// Одна на весь клиент и по ключу, а не в самих объектах: объект сущности
    /// здесь собирается заново на всякое обращение, и положенное в него не
    /// доживает до следующей строки.
    const ownMeta = new Map();

    function ownMetaFor(key) {
        let store = ownMeta.get(key);

        if (store === undefined) {
            store = new Map();
            ownMeta.set(key, store);
        }

        return store;
    }

    class Entity extends WorldObject {
        /// Номер сущности в сессии — тот, которым её зовёт сервер.
        ///
        /// −1 у тех, у кого его нет вовсе: случайный прохожий или дерево у
        /// дороги принадлежат игре, а не серверу, и номера им никто не выдавал.
        get sessionId() {
            return numbers.get(this)?.id ?? -1;
        }

        /// Под каким именем сервер рассылает её метаданные. null — ни под каким.
        get syncedKind() {
            const свой = numbers.get(this);
            return свой === undefined ? null : kMetaName[свой.kind];
        }

        /// Появилось ли тело у сущности.
        ///
        /// У alt:V `isSpawned` означает «у сущности есть тело в игре»: игрок
        /// сессии, до которого километр, числится в списке, но тела у него
        /// здесь нет. Тем же самым отвечает и наш `valid`, но имя alt:V нужно
        /// своё — режим спрашивает именно его.
        get isSpawned() {
            return this.valid;
        }

        /// alt:V зовёт номер сессии просто `id`.
        get id() {
            const свой = numbers.get(this);

            if (свой === undefined) {
                // Отказ, а не ноль: ноль — законный номер, и отдав его, мы
                // выдали бы прохожего за игрока. Отправленный на сервер, он
                // указал бы там на живого человека.
                throw new Error('entity.id: у этой сущности нет номера сессии — ' +
                                'она принадлежит игре, а не серверу');
            }

            return свой.id;
        }

        get model() {
            return this.valid ? asHash(natives.getEntityModel(this.scriptID)) : 0;
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

        /// Своя метаданная — та, что никуда не уходит и живёт на этой машине.
        ///
        /// Ключ хранения — не сам объект: сущность здесь отражение, живущее один
        /// вызов, и записанное в объект пропало бы к следующему обращению.
        /// Хранится оно по номеру сессии, если он есть, и по номеру в игре, если
        /// его нет: первый переживает перезаезд сущности, второй — нет, но у
        /// прохожего и переживать нечего.
        get #metaKey() {
            const свой = numbers.get(this);

            return свой === undefined ? `game:${this.scriptID}`
                                      : `${kMetaName[свой.kind]}:${свой.id}`;
        }

        /// Принимает и пару, и объект целиком — обе формы alt:V. Объектная
        /// раскладывается на пары, и о каждой объявляется своё событие: ресурс
        /// подписан на ключ, а не на вызов.
        setMeta(key, value) {
            shared._eachMetaPair(key, value, (name, own) => {
                const store = ownMetaFor(this.#metaKey);
                const было = store.get(name);

                store.set(name, own);

                // Объявляется и своя метаданная: у alt:V `metaChange` есть на
                // обеих сторонах, и подписываются на него именно там, где живёт
                // то, что реагирует на перемену. Первым доводом идёт сама
                // сущность, а не пара «род и номер»: ресурс сравнивает её через
                // `===` со своей.
                alt.client.emit('metaChange', this, name, own, было);
            });
        }

        getMeta(key) {
            return ownMetaFor(this.#metaKey).get(key);
        }

        hasMeta(key) {
            return ownMetaFor(this.#metaKey).has(key);
        }

        deleteMeta(key) {
            const store = ownMetaFor(this.#metaKey);
            const было = store.get(key);

            store.delete(key);
            alt.client.emit('metaChange', this, key, undefined, было);
        }

        getMetaKeys() {
            return [...ownMetaFor(this.#metaKey).keys()];
        }

        getSyncedMeta(key) {
            const род = this.syncedKind;

            if (род === null) {
                // Отказ, а не undefined: второе выглядело бы как «ключа нет», и
                // режим искал бы ошибку на сервере, где её нет.
                throw new Error('entity.getSyncedMeta: у этой сущности нет номера ' +
                                'сессии — сервер о ней не знает');
            }

            return alt.readSyncedMeta(род, this.sessionId, key);
        }

        getSyncedMetaKeys() {
            const род = this.syncedKind;
            return род === null ? [] : alt.readSyncedMetaKeys(род, this.sessionId);
        }

        hasSyncedMeta(key) {
            return this.getSyncedMeta(key) !== undefined;
        }

        /// streamSyncedMeta читается оттуда же, откуда и synced, и это не
        /// небрежность: на сервере они лежат в одном хранилище — раздачи по
        /// видимости у oxyMP пока нет, и стриминговая доходит до всех. Читай мы
        /// её из другого места, режим, положивший её на сервере, не нашёл бы её
        /// здесь.
        getStreamSyncedMeta(key) {
            return this.getSyncedMeta(key);
        }

        hasStreamSyncedMeta(key) {
            return this.hasSyncedMeta(key);
        }

        getStreamSyncedMetaKeys() {
            return this.getSyncedMetaKeys();
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

        /// Перезаряжается ли сейчас. Спрашивается у игры, а не берётся из
        /// снимка: снимок описывает игроков сессии, а прохожий по улице
        /// перезаряжается так же, и ответ ему нужен тот же.
        get isReloading() {
            return this.valid ? natives.isPedReloading(this.scriptID) === true : false;
        }

        get currentWeapon() {
            if (!this.valid) {
                return 0;
            }

            // Оружие возвращается выходным доводом, поэтому ответ приходит
            // списком: сперва то, что вернул сам натив, затем записанное.
            const [, оружие] = natives.getCurrentPedWeapon(this.scriptID, 0, true);
            return asHash(оружие);
        }

        get vehicle() {
            if (!this.valid) {
                return null;
            }

            // false — «не считать машину, к которой он только идёт».
            const handle = natives.getVehiclePedIsIn(this.scriptID, false);

            return handle === 0 ? null : vehicleByHandle(handle);
        }

        /// Место в машине, в нумерации alt:V: единица — водитель, ноль — нигде.
        ///
        /// **Отдавалось игровой нумерацией, и это молчало ровно так же, как
        /// молчало на сервере.** У игры водитель — минус единица; у alt:V —
        /// единица, и его же объявление говорит об этом прямо. Перевод один и
        /// тот же на обеих сторонах: смещение на двойку
        /// (`script-js/include/oxymp/script/js/alt_seat.hpp`).
        ///
        /// Хуже того, минус единица возвращалась дважды: и когда игрок не в
        /// машине, и когда его места не нашлось перебором. А минус единица — это
        /// место водителя, и не севший в машину читался как сидящий за рулём.
        /// Теперь «нигде» это ноль, и спутать его с местом нельзя.
        get seat() {
            const car = this.vehicle;
            if (car === null) {
                return kNowhere;
            }

            // Места считаются от −1 (водитель). Перебор — единственный способ:
            // натива «на каком месте сидит» у игры нет.
            for (let место = -1; место < 8; место++) {
                if (natives.getPedInVehicleSeat(car.scriptID, место, false) === this.scriptID) {
                    return toAltSeat(место);
                }
            }

            return kNowhere;
        }

        toString() {
            return `Ped{ scriptID: ${this.scriptID} }`;
        }
    }

    /// Игрок сессии: тот, кого сервер зовёт номером.
    ///
    /// Заводится номером, а не дескриптором. Тело у него сменное и может
    /// отсутствовать вовсе — игрок в сессии есть, а здесь, рядом с нами, его ещё
    /// нет. Такой игрок не бесполезен: у него есть имя и метаданные, и список
    /// игроков рисуют по ним.
    class Player extends Ped {
        constructor(id) {
            // Ноль дескриптором — заглушка на одно мгновение: сразу за
            // конструктором сущность помечается номером, и `scriptID` начинает
            // спрашиваться у клиента. Обойтись без этого нельзя — `super()`
            // обязан быть первым, а до него `this` не существует.
            super(0);
            numbers.set(this, { kind: kPlayerKind, id });
        }

        get name() {
            return native.playerName(this.sessionId);
        }

        /// Говорит ли он в голосовой связи.
        ///
        /// Всегда «нет», и это не заглушка, а правда: голосовой связи у oxyMP
        /// нет вовсе, а значит никто и не говорит. Отказ здесь был бы хуже —
        /// режимы спрашивают это каждый кадр, ради значка над головой, и
        /// исключение тридцать раз в секунду завалило бы журнал.
        get isTalking() { return false; }

        /// Насколько громко. По той же причине — ноль.
        get micLevel() { return 0; }

        // --- Состояние из снимка -------------------------------------------
        //
        // Всё, что ниже, берётся из снимка, а не из вопросов к игре, и это
        // единственно верно: чужой персонаж здесь кукла, которой распоряжаемся
        // мы сами. Спросить у игры «целится ли он» — значит спросить, что мы
        // сами ей велели, и ответ отстанет от правды ровно на то, что кукла ещё
        // не отыграла. Правду знает хозяин, и она приезжает снимком.
        //
        // Свой игрок отвечает своим снимком — тем, что уходит на сервер: он снят
        // в этом же кадре.
        //
        // Снимок спрашивается на каждое обращение, а не запоминается: он меняется
        // тридцать раз в секунду, а запомненный врал бы тем убедительнее, чем
        // дольше его не трогали.
        //
        // Разделение здесь по роду ответа, а не по вкусу. То, что игра держит
        // **наложенным** — здоровье, оружие, смерть, — спрашивается у тела, если
        // оно здесь: накладываем его мы сами из этого же снимка, и игра ответит
        // тем же, но свежее. То, что есть **намерение хозяина** — прицел,
        // приседание, стрельба, укрытие, — спрашивается только у снимка: кукла
        // его лишь отыгрывает, и спросить у неё значит спросить, что мы сами ей
        // велели.

        /// Признак состояния. Пусто, если игрока клиент не знает, — «нет», а не
        /// отказ: игрок, только что вышедший, законно отвечает «не целится».
        _flag(bit) {
            const снимок = native.playerState(this.sessionId);
            return снимок === null ? false : (снимок.flags & bit) !== 0;
        }

        /// Мёртв ли. У тела спрашивается, если оно здесь: здоровье кукле
        /// накладываем мы сами из этого же снимка, и игра ответит тем же, но
        /// свежее. Нет тела — отвечает снимок, и это не придирка: `Ped.isDead`
        /// без тела отвечает «мёртв», то есть врёт про всякого, кто просто
        /// стоит за пределами подгрузки.
        get isDead() {
            return this.valid ? super.isDead : this._flag(kPlayerFlag.Dead);
        }

        get isAiming() { return this._flag(kPlayerFlag.Aiming); }
        get isShooting() { return this._flag(kPlayerFlag.Shooting); }
        get isInRagdoll() { return this._flag(kPlayerFlag.Ragdoll); }
        get isJumping() { return this._flag(kPlayerFlag.Jumping); }
        get isCrouching() { return this._flag(kPlayerFlag.Crouching); }
        get isInCover() { return this._flag(kPlayerFlag.InCover); }
        get isInMelee() { return this._flag(kPlayerFlag.Melee); }
        get isParachuting() { return this._flag(kPlayerFlag.Parachuting); }
        get isReloading() { return this._flag(kPlayerFlag.Reloading); }
        get isEnteringVehicle() { return this._flag(kPlayerFlag.EnteringVehicle); }
        get isLeavingVehicle() { return this._flag(kPlayerFlag.LeavingVehicle); }
        get isOnVehicle() { return this._flag(kPlayerFlag.OnVehicle); }
        get isSwimming() { return this._flag(kPlayerFlag.Swimming); }

        /// Крадётся ли. Тот же признак, что и `isCrouching`, и это не подмена:
        /// своего приседания у игрока в GTA V нет — крадущийся и пригнувшийся
        /// там одно и то же. У alt:V это два свойства, и оба отвечают одним
        /// признаком игры.
        get isStealthy() { return this._flag(kPlayerFlag.Crouching); }

        /// Куда он целится. Не целясь — куда смотрит: поле в снимке одно, и
        /// различает их признак прицела.
        get aimPos() {
            const снимок = native.playerState(this.sessionId);

            return снимок === null ? shared.Vector3.zero
                                   : new shared.Vector3(снимок.aimX, снимок.aimY, снимок.aimZ);
        }

        /// Скорость, метры в секунду. Три вида, как у alt:V.
        get moveSpeed() {
            const v = this._velocity();
            return Math.sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
        }

        get forwardSpeed() {
            const снимок = native.playerState(this.sessionId);

            if (снимок === null) {
                return 0;
            }

            // Направление взгляда у игры считается от севера по часовой стрелке,
            // а синус с косинусом — от востока против неё. Отсюда перестановка
            // осей: «вперёд» это (-sin, cos), а не (cos, sin). Та же поправка
            // стоит и на сервере.
            const радианы = снимок.heading * (Math.PI / 180);
            const вперёдX = -Math.sin(радианы);
            const вперёдY = Math.cos(радианы);

            return (снимок.velocityX * вперёдX) + (снимок.velocityY * вперёдY);
        }

        get strafeSpeed() {
            const снимок = native.playerState(this.sessionId);

            if (снимок === null) {
                return 0;
            }

            const радианы = снимок.heading * (Math.PI / 180);
            const вперёдX = -Math.sin(радианы);
            const вперёдY = Math.cos(радианы);

            return (снимок.velocityX * вперёдY) - (снимок.velocityY * вперёдX);
        }

        _velocity() {
            const снимок = native.playerState(this.sessionId);

            return снимок === null ? { x: 0, y: 0, z: 0 }
                                   : { x: снимок.velocityX, y: снимок.velocityY,
                                       z: снимок.velocityZ };
        }

        /// Оружие в руках. Ноль — безоружен.
        ///
        /// Как и смерть: у тела, если оно здесь, — оружие ему выдаём мы сами из
        /// этого же снимка, — а без тела у снимка. `Ped.currentWeapon` без тела
        /// отвечает нулём, то есть «безоружен», про всякого, кто стоит за
        /// пределами подгрузки.
        get currentWeapon() {
            if (this.valid) {
                return super.currentWeapon;
            }

            const снимок = native.playerState(this.sessionId);
            return снимок === null ? 0 : снимок.weapon;
        }

        /// Раскраска оружия в руках.
        ///
        /// У чужого игрока её нет и взяться ей неоткуда: в снимке раскраски не
        /// едет, а спросить у его куклы — значит спросить, что мы сами ей
        /// вложили, то есть всегда ноль. Ноль этот выглядел бы законным ответом,
        /// и режим, красящий оружие, ни разу бы с ним не сошёлся.
        ///
        /// У своего игрока ответ настоящий — он живёт в `LocalPlayer`.
        get currentWeaponTintIndex() {
            throw new Error('player.currentWeaponTintIndex: раскраска чужого оружия по сети '
                            + 'не едет, а у куклы она всегда своя');
        }

        getWeaponTintIndex() {
            throw new Error('player.getWeaponTintIndex: раскраска чужого оружия по сети '
                            + 'не едет, а у куклы она всегда своя');
        }

        /// Насадки на оружии. Перебрать их у игры можно только по списку самих
        /// насадок, а списка этого нет ни в справочниках alt:V, ни у нас.
        get currentWeaponComponents() {
            throw new Error('player.currentWeaponComponents: перебрать насадки не по чему — '
                            + 'списка их у нас нет');
        }

        /// Фонарь на оружии. У игры натива на вопрос нет вовсе, а по сети
        /// признак не едет.
        get flashlightActive() {
            throw new Error('player.flashlightActive: у игры об этом не спросить, '
                            + 'а по сети признак не едет');
        }

        /// Поворот головы. alt:V читает его прямо из памяти персонажа; у нас
        /// такого чтения нет, а из снимка едет только направление взгляда.
        get headRot() {
            throw new Error('player.headRot: alt:V читает его из памяти персонажа, '
                            + 'у нас такого чтения нет');
        }

        /// Во что целится и с каким смещением. В снимке едет точка прицела, а не
        /// сущность: вывести одно из другого нельзя — луч упрётся в первое, что
        /// подвернётся, и ответ будет правдоподобным и неверным.
        get entityAimingAt() {
            throw new Error('player.entityAimingAt: по сети едет точка прицела, а не цель');
        }

        get entityAimOffset() {
            throw new Error('player.entityAimOffset: по сети едет точка прицела, а не цель');
        }

        /// Громкость голоса. Голосовой связи у нас нет вовсе.
        get spatialVolume() {
            throw new Error('player.spatialVolume: голосовой связи в oxyMP нет');
        }

        get nonSpatialVolume() {
            throw new Error('player.nonSpatialVolume: голосовой связи в oxyMP нет');
        }

        /// Данные задачи, которую сейчас исполняет персонаж. alt:V берёт их из
        /// своей синхронизации задач; у нас задач по сети не едет.
        get taskData() {
            throw new Error('player.taskData: задачи по сети не едут');
        }

        /// На лестнице ли. У игры натива нет вовсе — лестница у неё задача, а
        /// номер задачи пришлось бы угадывать, и угаданный не тот отвечал бы
        /// правдоподобно. Отказ вслух: тишина на вопрос — это ложь.
        get isOnLadder() {
            throw new Error('player.isOnLadder: у игры нет натива, а номер задачи '
                            + 'угадывать нельзя');
        }

        toString() {
            return `Player{ id: ${this.sessionId}, name: ${this.name} }`;
        }
    }

    /// Свой персонаж.
    ///
    /// Игрок как игрок, с одним отличием: своё тело спрашивается прямо у игры, а
    /// не у переводчика. Так надо — пока сервер нас не принял, номера у нас ещё
    /// нет, а персонаж в мире уже есть, и ресурс вправе его трогать.
    class LocalPlayer extends Player {
        constructor() {
            super(-1);
        }

        /// Дескриптор берётся заново на каждое обращение.
        ///
        /// Так надо: после смерти и возрождения игра выдаёт персонажу **новый**
        /// дескриптор, а старый перестаёт существовать. Сохранённый однажды, он
        /// сделал бы `alt.Player.local` мёртвым до конца сессии.
        get scriptID() {
            return natives.playerPedId();
        }

        /// У своего игрока раскраска настоящая: игра держит её у нашего же
        /// персонажа, и спросить её есть чем. У чужого ответа нет — там отказ.
        get currentWeaponTintIndex() {
            return natives.getPedWeaponTintIndex(this.scriptID, this.currentWeapon);
        }

        getWeaponTintIndex(weaponHash) {
            return natives.getPedWeaponTintIndex(this.scriptID, asHash(weaponHash));
        }

        /// Свой номер в сессии — тот, которым нас зовёт сервер.
        ///
        /// Спрашивается заново, а не берётся из реестра: при переподключении
        /// сервер выдаёт другой, и запомненный однажды устарел бы молча.
        get sessionId() {
            return selfId();
        }

        get syncedKind() {
            return kMetaName[kPlayerKind];
        }

        get id() {
            return selfId();
        }

        get name() {
            // Своё имя — то, каким нас знают остальные, то есть имя сессии, а не
            // то, что написано в Social Club. Ресурс, показывающий его игроку
            // рядом со списком, обязан показать одно и то же имя себе и другим.
            //
            // У игры оно спрашивается только пока сервер нас не принял: имени
            // сессии в этот миг ещё нет, а персонаж в мире уже есть.
            const своё = native.playerName(this.sessionId);

            return своё === '' ? natives.getPlayerName(natives.playerId()) : своё;
        }

        // --- Своё оружие ----------------------------------------------------
        //
        // Спрашивается прямо у игры, а не у снимка: своё тело здесь, оно наше, и
        // игра о нём знает свежее всех. Снимок для своего игрока — это то, что
        // мы у неё же и списали тактом раньше.

        /// Сколько патронов в обойме того, что сейчас в руках.
        ///
        /// Именно в обойме, а не всего: так объявлено у alt:V, и режимы рисуют
        /// этим счётчик под прицелом.
        /// Ноль означает и «обойма пуста», и «оружия нет в руках», и различить
        /// их отсюда нечем: `GET_AMMO_IN_CLIP` отвечает «нет» во втором случае,
        /// а сколько патронов у неубранного ствола — не говорит никто.
        ///
        /// Измерено живой игрой: выданный пистолет числится выбранным
        /// (`GET_SELECTED_PED_WEAPON` отдаёт его хеш), патронов к нему
        /// шестьдесят, а `IS_PED_ARMED` отвечает «безоружен» и обойма не
        /// читается. То есть «выбран» и «в руках» — разные состояния, и
        /// спрашивать про обойму имеет смысл только про второе.
        get currentAmmo() {
            if (!this.valid) {
                return 0;
            }

            // Ответ приходит вторым местом: сам натив отвечает «есть ли обойма».
            const [ясно, обойма] = natives.getAmmoInClip(this.scriptID, this.currentWeapon, 0);
            return ясно === true ? обойма : 0;
        }

        /// Сколько патронов к этому оружию всего. Принимает и хеш, и имя.
        getWeaponAmmo(weapon) {
            return this.valid ? natives.getAmmoInPedWeapon(this.scriptID, weaponHash(weapon)) : 0;
        }

        hasWeapon(weapon) {
            // Последний довод — **false**, и это измерено, а не выведено. С
            // `true` натив отвечает «нет» про оружие, которое у человека есть:
            // проверено на выданном пистолете — `(…, false)` даёт «да»,
            // `(…, true)` даёт «нет». Что означает этот довод у игры, из её
            // подписи не видно, а гадать здесь дороже, чем измерить.
            return this.valid
                ? natives.hasPedGotWeapon(this.scriptID, weaponHash(weapon), false) === true
                : false;
        }

        /// Какие обвесы надеты на это оружие.
        ///
        /// Перебором по списку известных обвесов, а не одним вопросом: своего
        /// «перечисли обвесы» у игры нет, есть только «надет ли такой-то».
        /// Список берётся у справочника оружия — того же, что у сервера, — и
        /// без него отказывает вслух: пустой список означал бы «обвесов нет».
        getWeaponComponents(_weapon) {
            throw new Error('player.getWeaponComponents: у игры нет натива «перечисли обвесы», '
                            + 'а перебор требует их списка — спросите hasWeaponComponent '
                            + 'про нужный');
        }

        hasWeaponComponent(weapon, component) {
            return this.valid
                ? natives.hasPedGotWeaponComponent(this.scriptID, weaponHash(weapon),
                                                   weaponHash(component)) === true
                : false;
        }

        /// Сколько осталось бега. Игра держит это долей от единицы, а alt:V —
        /// тем же числом; переводить нечего.
        get stamina() {
            return natives.getPlayerSprintStaminaRemaining(natives.playerId());
        }

        set stamina(_value) {
            warnOnce('player.stamina',
                     'выносливость игре не задаётся: натив у неё только спрашивает');
        }

        get maxStamina() {
            throw new Error('player.maxStamina: у игры об этом не спросить — она держит '
                            + 'остаток, а не предел');
        }

        set maxStamina(_value) {
            warnOnce('player.maxStamina', 'предел выносливости игре не задаётся');
        }

        /// Описание оружия в руках. У alt:V это `WeaponData` — целый класс со
        /// своими полями урона и разброса; их держит справочник оружия, а не
        /// игра, и у клиента его нет. Отказ вслух: `null` здесь законный ответ
        /// alt:V и означает «в руках ничего», а не «мы не знаем».
        get currentWeaponData() {
            throw new Error('player.currentWeaponData: справочник оружия живёт у сервера — '
                            + 'спросите alt.getWeaponModelInfoByHash оттуда');
        }

        toString() {
            return `LocalPlayer{ name: ${this.name} }`;
        }
    }

    // --- Машины ---------------------------------------------------------------

    class Vehicle extends Entity {
        // --- Чего у машины нет, и почему ------------------------------------
        //
        // Колёса поштучно и приборная панель: у игры нативов на это нет вовсе —
        // alt:V читает и правит их прямо в памяти машины, а такого чтения у нас
        // нет. Отказ громкий, а не тишина: `undefined` ресурс примет за правду.

        getWheelCamber(_wheel) {
            throw new Error('vehicle.getWheelCamber: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelConfigFlag(_wheel) {
            throw new Error('vehicle.getWheelConfigFlag: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelDynamicFlag(_wheel) {
            throw new Error('vehicle.getWheelDynamicFlag: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelHeight(_wheel) {
            throw new Error('vehicle.getWheelHeight: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelRimRadius(_wheel) {
            throw new Error('vehicle.getWheelRimRadius: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelSurfaceMaterial(_wheel) {
            throw new Error('vehicle.getWheelSurfaceMaterial: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelTrackWidth(_wheel) {
            throw new Error('vehicle.getWheelTrackWidth: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelTyreRadius(_wheel) {
            throw new Error('vehicle.getWheelTyreRadius: колёса поштучно у игры нативами не спрашиваются');
        }

        getWheelTyreWidth(_wheel) {
            throw new Error('vehicle.getWheelTyreWidth: колёса поштучно у игры нативами не спрашиваются');
        }

        setWheelCamber(_wheel, _value) {
            warnOnce('vehicle.setWheelCamber', 'колёса поштучно у игры нативами не правятся');
        }

        setWheelConfigFlag(_wheel, _flag, _value) {
            warnOnce('vehicle.setWheelConfigFlag', 'колёса поштучно у игры нативами не правятся');
        }

        setWheelDynamicFlag(_wheel, _flag, _value) {
            warnOnce('vehicle.setWheelDynamicFlag', 'колёса поштучно у игры нативами не правятся');
        }

        setWheelHeight(_wheel, _value) {
            warnOnce('vehicle.setWheelHeight', 'колёса поштучно у игры нативами не правятся');
        }

        setWheelRimRadius(_wheel, _value) {
            warnOnce('vehicle.setWheelRimRadius', 'колёса поштучно у игры нативами не правятся');
        }

        setWheelTrackWidth(_wheel, _value) {
            warnOnce('vehicle.setWheelTrackWidth', 'колёса поштучно у игры нативами не правятся');
        }

        setWheelTyreRadius(_wheel, _value) {
            warnOnce('vehicle.setWheelTyreRadius', 'колёса поштучно у игры нативами не правятся');
        }

        setWheelTyreWidth(_wheel, _value) {
            warnOnce('vehicle.setWheelTyreWidth', 'колёса поштучно у игры нативами не правятся');
        }

        get absLight() {
            throw new Error('vehicle.absLight: лампы приборной панели у игры не спрашиваются');
        }

        get batteryLight() {
            throw new Error('vehicle.batteryLight: лампы приборной панели у игры не спрашиваются');
        }

        get engineLight() {
            throw new Error('vehicle.engineLight: лампы приборной панели у игры не спрашиваются');
        }

        get oilLight() {
            throw new Error('vehicle.oilLight: лампы приборной панели у игры не спрашиваются');
        }

        get petrolLight() {
            throw new Error('vehicle.petrolLight: лампы приборной панели у игры не спрашиваются');
        }

        get wheelsCount() {
            throw new Error('vehicle.wheelsCount: число колёс у игры не спросить — '
                            + 'alt:V читает его из памяти машины');
        }

        get maxGear() {
            throw new Error('vehicle.maxGear: число передач у игры не спросить — '
                            + 'alt:V читает его из памяти машины');
        }

        get suspensionHeight() {
            throw new Error('vehicle.suspensionHeight: высота подвески у игры не '
                            + 'спрашивается — alt:V читает её из памяти машины');
        }

        resetDashboardLights() {
            warnOnce('vehicle.resetDashboardLights', 'лампы приборной панели у игры не правятся');
        }

        setupTransmission() {
            warnOnce('vehicle.setupTransmission', 'передачи у игры нативами не настраиваются');
        }

        get driver() {
            if (!this.valid) {
                return null;
            }

            const handle = natives.getPedInVehicleSeat(this.scriptID, -1, false);
            return handle === 0 ? null : pedByHandle(handle);
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

        /// Как машина заперта — числами игры, они же числа alt:V.
        ///
        /// Спрашивается у игры, а не помнится по распоряжению сервера: замок
        /// накладывает каждый, кто машину видит, и наложенное могло не встать.
        get lockState() {
            return this.valid ? natives.getVehicleDoorLockStatus(this.scriptID) : 0;
        }

        /// Прочность бака. Отдельно от кузова и двигателя: горит машина именно
        /// от него, и режимы со своей механикой пожара смотрят на него.
        get petrolTankHealth() {
            return this.valid ? natives.getVehiclePetrolTankHealth(this.scriptID) : 0;
        }

        /// Сколько в машине мест. У модели, а не у самой машины: число это её
        /// свойство и не меняется от того, кто в ней сидит.
        get seatCount() {
            return this.valid ? natives.getVehicleModelNumberOfSeats(this.model) : 0;
        }

        /// Скорость по осям **самой машины**, а не мира: вперёд, вбок, вверх.
        ///
        /// Не то же, что `velocity`, и различать обязательно: по нему считают
        /// занос и езду задним ходом, а мировая скорость об этом не говорит
        /// ничего — машина, едущая назад на север, и машина, едущая вперёд на
        /// север, дают в ней одно и то же.
        get speedVector() {
            if (!this.valid) {
                return shared.Vector3.zero;
            }

            // true — «в осях машины». false дал бы мировые, то есть `velocity`.
            return new shared.Vector3(natives.getEntitySpeedVector(this.scriptID, true));
        }

        /// Указатели поворота. Набор битов игры, он же набор alt:V
        /// (`VehicleIndicatorLights`).
        ///
        /// Только запись: спросить у игры об этом нечем — натив у неё один и
        /// тот распоряжение, а не вопрос. Отказ на чтение вслух, а не ноль:
        /// ноль означает «оба погашены», и режим, мигающий поворотником по
        /// очереди, читал бы своё же состояние неверно.
        get indicatorLights() {
            return absent('vehicle.indicatorLights')();
        }

        set indicatorLights(value) {
            if (this.valid) {
                natives.setVehicleIndicatorLights(this.scriptID, Number(value) || 0, true);
            }
        }

        /// Передача и обороты у alt:V читаются из памяти самой машины, а не
        /// нативом: ни того ни другого натива нет ни в открытой базе, ни в
        /// таблице alt:V. Отказ вслух, а не ноль: ноль означал бы «стоит на
        /// нейтрали и заглушена», и режим, показывающий тахометр, показал бы
        /// его неподвижным.
        ///
        /// Той же породы `engineTemperature`, `fuelLevel`, `oilLevel`,
        /// `steeringAngle` и вся геометрия колёс: нативов нет, память машины
        /// нам не размечена, а выдумать число значило бы соврать про приборы.
        get gear() {
            return absent('vehicle.gear')();
        }

        get rpm() {
            return absent('vehicle.rpm')();
        }

        get engineTemperature() {
            return absent('vehicle.engineTemperature')();
        }

        get fuelLevel() {
            return absent('vehicle.fuelLevel')();
        }

        get oilLevel() {
            return absent('vehicle.oilLevel')();
        }

        get steeringAngle() {
            return absent('vehicle.steeringAngle')();
        }

        toString() {
            return `Vehicle{ scriptID: ${this.scriptID} }`;
        }
    }

    /// Машина сессии: заведённая номером, а не дескриптором.
    ///
    /// Своим именем, чтобы разница была видна прямо в коде: обычная `Vehicle`
    /// оборачивает дескриптор, отданный нативом, и живёт ровно столько, сколько
    /// живёт этот дескриптор.
    class SessionVehicle extends Vehicle {
        constructor(id) {
            super(0);
            numbers.set(this, { kind: kVehicleKind, id });
        }

        toString() {
            return `Vehicle{ id: ${this.sessionId} }`;
        }
    }

    /// Прохожий сессии: тот, кого сервер зовёт номером.
    ///
    /// Своим именем, как и `SessionVehicle`, и по той же причине: обычный `Ped`
    /// оборачивает дескриптор, отданный нативом, и живёт ровно столько, сколько
    /// живёт этот дескриптор. У этого есть номер сервера, и он один для всех.
    class SessionPed extends Ped {
        constructor(id) {
            super(0);
            numbers.set(this, { kind: kPedKind, id });
        }

        toString() {
            return `Ped{ id: ${this.sessionId} }`;
        }
    }

    // --- Реестр сущностей сессии ----------------------------------------------

    /// Один объект на номер, а не новый на каждое обращение.
    ///
    /// Это единственное, что здесь помнится, и помнится не ради скорости.
    /// Ресурсы alt:V сравнивают сущности через `===`, кладут их ключами в Map и
    /// WeakMap, ищут `alt.Player.all.find((кто) => кто === цель)`. Раздавай мы
    /// каждый раз новую обёртку — сравнение не сходилось бы никогда, подписки по
    /// ключу текли бы, и найти причину было бы не по чему: всё выглядит верным.
    ///
    /// Правды объект при этом не хранит: все его свойства спрашиваются заново.
    const registry = {
        [kPlayerKind]: new Map(),
        [kVehicleKind]: new Map(),
        [kPedKind]: new Map(),
    };

    /// Свой персонаж заводится один раз и живёт до конца сессии.
    let local = null;

    function localPlayer() {
        if (local === null) {
            local = new LocalPlayer();
        }

        return local;
    }

    /// Сущность сессии по её номеру. Заводит, если такой ещё не показывали.
    function tracked(kind, id) {
        // Себя отдаём тем же объектом, что и `alt.Player.local`: режимы сравнивают
        // найденного в списке с local через `===`, и два разных объекта на одного
        // человека означали бы «меня в списке нет».
        if (kind === kPlayerKind && id === selfId()) {
            return localPlayer();
        }

        const known = registry[kind];
        let found = known.get(id);

        if (found === undefined) {
            found = kind === kVehicleKind ? new SessionVehicle(id)
                  : kind === kPedKind ? new SessionPed(id)
                                      : new Player(id);
            known.set(id, found);
        }

        return found;
    }

    /// Сущности сессии этого рода, обёрнутые в объекты.
    ///
    /// streamedOnly — оставить только тех, у кого здесь есть тело. Разделение то
    /// же, что у alt:V: `all` — все, о ком сказал сервер, `streamedIn` — те, кто
    /// рядом. Первое нужно для списка игроков, второе — для всего, что рисуется
    /// над головой.
    function listOf(kind, streamedOnly) {
        // Плоский список тройками «номер, тело, ведущий» — так его отдаёт
        // мостик. Плоским, а не объектами: собирается он каждый кадр на всякую
        // сущность, и объект на каждую стоил бы уборки за собой.
        const flat = native.sessionEntities(kind);

        const list = [];
        const present = new Set();

        for (let i = 0; i + 2 < flat.length; i += 3) {
            const id = flat[i];
            const handle = flat[i + 1];
            const owner = flat[i + 2];

            present.add(id);
            noteOwner(kind, id, owner);

            if (!streamedOnly || handle !== 0) {
                list.push(tracked(kind, id));
            }
        }

        // Ушедших забываем. Без уборки реестр рос бы всю сессию: на людном
        // сервере за вечер через него проходят тысячи номеров, и каждый остался
        // бы висеть вместе со своим объектом.
        //
        // Убирается по полному списку, а не по обрезанному: удали мы
        // отсутствующих в `streamedIn`, отошедший за угол игрок терял бы
        // тождество и возвращался бы уже другим объектом.
        if (!streamedOnly) {
            for (const id of registry[kind].keys()) {
                if (!present.has(id)) {
                    registry[kind].delete(id);
                }
            }
        }

        return list;
    }

    /// Сущность сессии по дескриптору игры, если этот дескриптор её.
    ///
    /// Пусто — дескриптор принадлежит игре: случайный прохожий, машина из
    /// трафика, дерево.
    /// Кто ведёт каждую сущность, по роду и номеру. −1 — никто.
    ///
    /// Помнится здесь, а не спрашивается при обращении: смену ведущего ищут
    /// сравнением с прошлым кадром, и прошлое нужно где-то держать. Приходит оно
    /// тем же списком, что и тела, — отдельного вопроса на сущность не бывает.
    const owners = {
        [kPlayerKind]: new Map(),
        [kVehicleKind]: new Map(),
        [kPedKind]: new Map(),
    };

    /// Записывает ведущего и объявляет смену, если она была.
    ///
    /// Первое наблюдение сменой не считается: до него мы о сущности не знали
    /// ничего, и объявленная тогда «смена» означала бы «она появилась», о чём
    /// говорят другие события.
    function noteOwner(kind, id, owner) {
        const known = owners[kind];
        const был = known.get(id);

        if (был === owner) {
            return;
        }

        known.set(id, owner);

        // Первое наблюдение сменой не считается — но записать его надо было,
        // и потому проверка стоит после записи.
        if (был === undefined) {
            return;
        }

        alt.client.emit('netOwnerChange', tracked(kind, id),
                        owner < 0 ? null : tracked(kPlayerKind, owner),
                        был < 0 ? null : tracked(kPlayerKind, был));
    }

    /// Кто ведёт эту сущность. null — никто.
    function ownerOf(entity) {
        // Свой игрок лежит в реестре с номером −1, а не с настоящим: заводится
        // он раньше, чем сервер называет наш номер (`new LocalPlayer()` зовёт
        // `super(-1)`), и с тех пор номер там так и остаётся минус единицей.
        //
        // Проверять поэтому нужно сам объект, а не пустоту записи: первая
        // написанная здесь проверка искала «нет записи» и до своего игрока не
        // доходила — он отвечал «никем не ведётся», хотя ведёт себя сам.
        const свой = entity === localPlayer() ? { kind: kPlayerKind, id: selfId() }
                                              : numbers.get(entity);

        if (свой === undefined) {
            // Не сущность сессии вовсе: заведённое клиентом сервер не знает, и
            // ведущего у него нет.
            return null;
        }

        const owner = owners[свой.kind]?.get(свой.id);

        return owner === undefined || owner < 0 ? null : tracked(kPlayerKind, owner);
    }

    function trackedByHandle(kind, handle) {
        const id = native.sessionId(kind, handle);
        return id < 0 ? null : tracked(kind, id);
    }

    /// Персонаж по дескриптору: игрок сессии, если он игрок, иначе просто ped.
    function pedByHandle(handle) {
        // Игроки сперва, прохожие сессии следом, и только потом голая обёртка.
        //
        // Порядок значим: тело игрока — тоже ped, и спроси мы сперва про
        // прохожих, свой игрок нашёлся бы среди них. Голая обёртка последняя:
        // это случайный прохожий самой игры, у которого номера сессии нет.
        return trackedByHandle(kPlayerKind, handle)
            ?? trackedByHandle(kPedKind, handle)
            ?? new Ped(handle);
    }

    /// То же для машины.
    function vehicleByHandle(handle) {
        return trackedByHandle(kVehicleKind, handle) ?? new Vehicle(handle);
    }

    /// У кого из сущностей сессии тело было в прошлом кадре.
    ///
    /// Номера, а не объекты: объект берётся из того же реестра и сравнивать их
    /// незачем, а хранить их здесь значило бы держать вторую ссылку на то, что
    /// реестр вправе забыть.
    const bodied = {
        [kPlayerKind]: new Set(),
        [kVehicleKind]: new Set(),
        [kPedKind]: new Set(),
    };

    /// Сверяет, у кого тело появилось, а у кого пропало.
    ///
    /// **Событий об этом не было вовсе, а режим на них опирается.** У alt:V
    /// `worldObjectStreamIn` и `worldObjectStreamOut` — то, чем клиентская
    /// половина узнаёт, что рядом появился человек или машина: над ними рисуют
    /// имя, вешают метку, заводят кукле поведение. У проверенного режима на них
    /// подписаны пять мест, и не приходило туда ничего.
    ///
    /// Считается сверкой раз в кадр, а не приходит сообщением, и по-другому не
    /// выйдет: тело сущности заводит игра у себя, когда ей вздумается, и сервер
    /// об этом не знает — у одного игрока модель уже загрузилась, у другого ещё
    /// нет. Ровно поэтому и `streamedIn` считается так же.
    ///
    /// `gameEntityCreate` и `gameEntityDestroy` объявляются тем же поводом и
    /// теми же доводами: у alt:V они означают ровно это — «у сетевой сущности
    /// появилось (пропало) тело в игре». Разводить их по двум сверкам значило бы
    /// ходить по одному списку дважды.
    function watchBodies(kind) {
        const flat = native.sessionEntities(kind);
        const now = new Set();

        for (let i = 0; i + 2 < flat.length; i += 3) {
            const id = flat[i];
            const handle = flat[i + 1];

            // Ведущий сверяется здесь же, а не при обращении к спискам: сверка
            // эта идёт каждым кадром, а списки режим спрашивает когда захочет —
            // и `netOwnerChange` у режима, который их не спрашивает, не пришло
            // бы ни разу.
            noteOwner(kind, id, flat[i + 2]);

            if (handle === 0) {
                continue;
            }

            now.add(id);

            if (!bodied[kind].has(id)) {
                const entity = tracked(kind, id);
                alt.client.fireLocal('worldObjectStreamIn', [entity]);
                alt.client.fireLocal('gameEntityCreate', [entity]);
            }
        }

        for (const id of bodied[kind]) {
            if (now.has(id)) {
                continue;
            }

            // Сущность, у которой тело пропало, могла и вовсе уйти из сессии.
            // Объект под неё берётся всё равно: обработчику нужен тот же самый,
            // с которым он работал, — сравнивают их через `===`, — а реестр
            // забудет его сам, на ближайшем полном обходе.
            const entity = tracked(kind, id);
            alt.client.fireLocal('worldObjectStreamOut', [entity]);
            alt.client.fireLocal('gameEntityDestroy', [entity]);

            // И третьим именем — `removeEntity`. У alt:V оно означает то же
            // самое с другой стороны: сущность ушла отсюда. Разводить его по
            // своей сверке значило бы ходить по одному списку дважды, а молчать
            // о нём — оставить режим без имени, которое он слушает.
            alt.client.fireLocal('removeEntity', [entity]);
        }

        bodied[kind] = now;
    }

    /// Где свой игрок сидел в прошлом кадре: машина и место.
    ///
    /// Машина — дескриптором игры, а не сущностью: сущность заводится заново на
    /// каждое обращение, и сравнивать их между собой нельзя. Ноль — не сидел
    /// нигде.
    let riding = { handle: 0, seat: kNowhere };

    /// Сверяет, не сел ли свой игрок в машину, не вышел ли и не пересел ли.
    ///
    /// **Событий об этом на клиенте не было вовсе.** У alt:V ими открывают
    /// интерфейс машины, включают своё управление, показывают спидометр — и
    /// приходят они именно на клиент, потому что на клиенте всё это и рисуется.
    ///
    /// Считается сверкой раз в кадр, а не приходит сообщением, и по-другому не
    /// выйдет: сажает персонажа игра у себя, и сервер узнаёт об этом из того же
    /// снимка, из которого узнали бы и мы. Спрашивать игру напрямую вернее и
    /// дешевле, чем ждать круга через сервер.
    ///
    /// Место — в нумерации alt:V, как и везде наружу.
    function watchOwnVehicle() {
        const me = localPlayer();

        // Тела может не быть вовсе: игрок ещё грузится или уже вышел.
        if (me.scriptID === 0) {
            riding = { handle: 0, seat: kNowhere };
            return;
        }

        const car = me.vehicle;
        const seat = car === null ? kNowhere : me.seat;

        // Места нет — значит и в машине его нет, чем бы ни отвечала игра.
        //
        // **Это стоило неверного события, и нашла его живая игра.** Пока идёт
        // высадка, игра ещё числит персонажа при машине
        // (`GET_VEHICLE_PED_IS_IN` отвечает ею), а места он уже не занимает —
        // перебор сидений не находит его нигде. Выходило, что машина та же, а
        // место сменилось с первого на «нигде», и слой объявлял `changedVehicleSeat`
        // с местом ноль вместо `leftVehicle`. Режим, слушающий выход, не узнавал
        // о нём вовсе.
        const handle = car === null || seat === kNowhere ? 0 : car.scriptID;

        if (handle === riding.handle && seat === riding.seat) {
            return;
        }

        const was = riding;
        riding = { handle, seat };

        // Пересадка внутри одной машины — своё событие, а не «вышел и сел». У
        // alt:V оно тоже своё, и режим, следящий за тем, кто за рулём, ждёт
        // именно его.
        if (handle !== 0 && handle === was.handle) {
            alt.client.fireLocal('changedVehicleSeat', [car, was.seat, seat]);
            return;
        }

        if (was.handle !== 0) {
            // Машина, из которой вышли, могла уже исчезнуть — тогда сущность
            // берётся по дескриптору и окажется пустой. Это честнее выдумки:
            // объявить выход не из чего.
            const left = vehicleByHandle(was.handle);
            alt.client.fireLocal('leftVehicle', [left, was.seat]);
        }

        if (handle !== 0) {
            alt.client.fireLocal('enteredVehicle', [car, seat]);
        }
    }

    // --- Сборка ---------------------------------------------------------------

    alt.entities = {
        /// Кадровая сверка тел и своей машины. Зовётся слоем клиента раз в кадр.
        watchBodies() {
            watchBodies(kPlayerKind);
            watchBodies(kVehicleKind);

            // Прохожие тоже: у них есть тело, оно появляется и пропадает по
            // подгрузке, и события об этом режим слушает так же, как о машинах.
            watchBodies(kPedKind);

            watchOwnVehicle();
        },

        WorldObject,
        Entity,
        Ped,
        SessionPed,
        Player,
        Vehicle,
        LocalPlayer,

        get local() {
            return localPlayer();
        },

        players: (streamedOnly) => listOf(kPlayerKind, streamedOnly === true),
        vehicles: (streamedOnly) => listOf(kVehicleKind, streamedOnly === true),
        peds: (streamedOnly) => listOf(kPedKind, streamedOnly === true),

        /// Игрок сессии по его номеру. Пусто — такого в сессии нет.
        ///
        /// Ищется по списку, а не по дескриптору: дескриптор у игрока может быть
        /// нулевым — он в сессии есть, а тела у него здесь ещё нет, — и решать
        /// по нему «есть ли такой» значило бы терять всех дальних.
        playerById(id) {
            const число = Number(id);

            if (число === selfId()) {
                return localPlayer();
            }

            return listOf(kPlayerKind, false).find((кто) => кто.sessionId === число) ?? null;
        },

        vehicleById(id) {
            const число = Number(id);
            return listOf(kVehicleKind, false).find((что) => что.sessionId === число) ?? null;
        },

        pedById(id) {
            const число = Number(id);
            return listOf(kPedKind, false).find((кто) => кто.sessionId === число) ?? null;
        },

        /// Сущность по дескриптору игры.
        ///
        /// Нужна затем, что нативы отдают именно дескрипторы: луч, попавший в
        /// персонажа, вернёт число, и обернуть его во что-то осмысленное больше
        /// нечем. Если дескриптор оказался телом сущности сессии — отдаётся она
        /// сама, с номером и метаданными, а не безымянная обёртка.
        fromScriptID(handle) {
            const number = Number(handle) || 0;

            if (number === 0 || natives.doesEntityExist(number) !== true) {
                return null;
            }

            if (natives.isEntityAVehicle(number) === true) {
                return vehicleByHandle(number);
            }

            if (natives.isEntityAPed(number) === true) {
                return number === natives.playerPedId() ? localPlayer() : pedByHandle(number);
            }

            return new Entity(number);
        },

        absent,
    };
})(globalThis.__oxympAlt);
