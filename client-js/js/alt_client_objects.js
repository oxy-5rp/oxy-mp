// Метки, маркеры и чекпоинты на клиенте.
//
// В отличие от серверных однофамильцев (`script-js/js/alt_objects.js`), эти
// действительно видны: рисует их сама игра, а мы лишь зовём нативы. Серверные
// заводятся, живут и ждут своего часа — канала синхронизации между сторонами
// пока нет, — а эти работают целиком и сегодня.
//
// Файл исполняется после alt_client_entities.js и до alt_client.js.

'use strict';

(function build(alt) {
    const shared = alt.shared;
    const natives = alt.natives;

    /// Отказ вслух. Тот же, что у сущностей: вопрос без ответа обязан отказать,
    /// а не промолчать — тишину ресурс примет за правду и унесёт её дальше.
    function absent(what) {
        return function () {
            throw new Error(`${what}: в oxyMP этого ещё нет`);
        };
    }

    /// Всё, что заведено скриптом и живёт до destroy().
    class BaseObject {
        #alive = true;

        constructor() {
            // О рождении объявляется здесь, в основании, а не у каждого рода
            // порознь: заведись новый род — событие достанется ему само.
            //
            // Готовы к этому мгновению только общие поля: наследник ставит свои
            // после `super()`. Так и у alt:V — довод там объявлен `BaseObject`,
            // а не меткой и не маркером.
            alt.client.emit('baseObjectCreate', this);
        }

        get valid() {
            return this.#alive;
        }

        /// Номер, под которым объект знает сервер.
        ///
        /// У заведённого клиентом его нет и быть не может: сервер о нём не
        /// знает вовсе. Минус единица, а не ноль: ноль — законный номер, и
        /// отданный вместо «нет номера» он указал бы на чужой объект.
        get remoteID() {
            return -1;
        }

        /// Помечает объект убранным. Наследник убирает своё сам.
        _forget() {
            if (!this.#alive) {
                return;
            }

            this.#alive = false;

            // После снятия признака жизни: обработчик, спросивший `valid`,
            // обязан услышать «нет», а не застать объект живым в событии о его
            // смерти.
            alt.client.emit('baseObjectRemove', this);
        }
    }

    // --- Метки на карте --------------------------------------------------------

    /// Метка на карте и радаре.
    ///
    /// Игра выдаёт метке свой дескриптор и им же ею и распоряжается. Свойства
    /// здесь только на запись: спросить у игры, какого цвета метка, нечем — у
    /// неё есть натив-распоряжение и нет натива-вопроса. Поэтому последнее
    /// заданное хранится рядом, и это единственное место в клиентском слое, где
    /// значение держится у нас, а не спрашивается у игры.
    class Blip extends BaseObject {
        #handle = 0;
        #sprite = 1;
        #color = 0;
        #alpha = 255;
        #scale = 1;
        #name = '';
        #shortRange = false;
        #route = false;
        #routeColor = 0;
        #priority = 0;
        #category = 0;
        #flashes = false;
        #flashTimer = 0;
        #flashInterval = 0;
        #bright = false;
        #friendly = false;
        #showCone = false;
        #display = 2;
        #highDetail = false;
        #missionCreator = false;
        #headingIndicator = false;
        #tick = false;
        #shrinked = false;
        #number = 0;
        #secondaryColor = null;
        #gxtName = '';

        constructor(handle) {
            super();
            this.#handle = Number(handle) || 0;

            if (this.#handle === 0) {
                throw new Error('метка не завелась: игра отказала');
            }

            Blip.all.push(this);
        }

        static all = [];

        get scriptID() {
            return this.#handle;
        }

        get valid() {
            return super.valid && natives.doesBlipExist(this.#handle) === true;
        }

        get sprite() { return this.#sprite; }

        set sprite(value) {
            this.#sprite = Number(value) || 0;
            natives.setBlipSprite(this.#handle, this.#sprite);
        }

        get color() { return this.#color; }

        set color(value) {
            this.#color = Number(value) || 0;
            natives.setBlipColour(this.#handle, this.#color);
        }

        get alpha() { return this.#alpha; }

        set alpha(value) {
            this.#alpha = Number(value) || 0;
            natives.setBlipAlpha(this.#handle, this.#alpha);
        }

        get scale() { return this.#scale; }

        set scale(value) {
            this.#scale = Number(value) || 1;
            natives.setBlipScale(this.#handle, this.#scale);
        }

        get shortRange() { return this.#shortRange; }

        set shortRange(value) {
            this.#shortRange = Boolean(value);
            natives.setBlipAsShortRange(this.#handle, this.#shortRange);
        }

        get route() { return this.#route; }

        set route(value) {
            this.#route = Boolean(value);
            natives.setBlipRoute(this.#handle, this.#route);
        }

        /// Кто рисуется поверх кого, когда метки сошлись в одной точке, и в
        /// какой раздел списка на карте метка попадёт. Помнятся здесь, а не
        /// спрашиваются у игры: обратных нативов у неё нет ни для того, ни для
        /// другого — только запись.
        get priority() { return this.#priority; }

        set priority(value) {
            this.#priority = Number(value) || 0;
            natives.setBlipPriority(this.#handle, this.#priority);
        }

        get category() { return this.#category; }

        set category(value) {
            this.#category = Number(value) || 0;
            natives.setBlipCategory(this.#handle, this.#category);
        }

        /// Цвет линии маршрута. Помнится нами, а не спрашивается у игры: своего
        /// вопроса у неё нет, есть только распоряжение.
        ///
        /// Помнить, а не отказывать, — потому что помнить есть что: цвет
        /// назначили мы сами, и наш ответ верен ровно до тех пор, пока игра его
        /// не перепишет. Переписать она его не может: линия маршрута — не её
        /// затея, а нашей метки.
        get routeColor() { return this.#routeColor; }

        set routeColor(value) {
            this.#routeColor = Number(value) || 0;
            natives.setBlipRouteColour(this.#handle, this.#routeColor);
        }

        /// Где метка стоит. У игры спрашивается, а не помнится: метку вправе
        /// подвинуть и она сама — например, если метка привязана к сущности.
        get pos() {
            return new shared.Vector3(natives.getBlipCoords(this.#handle));
        }

        set pos(value) {
            const точка = new shared.Vector3(value);
            natives.setBlipCoords(this.#handle, точка.x, точка.y, точка.z);
        }

        get name() { return this.#name; }

        /// Подпись метки задаётся тремя нативами подряд, и порядок обязателен.
        ///
        /// Игра собирает строку через свой сборщик текста: «начали», «вот
        /// кусок», «кончили». Пропусти любой из трёх — и подпись либо не
        /// появится, либо прилипнет к следующей заданной, потому что сборщик
        /// остался открытым.
        set name(value) {
            this.#name = String(value);

            natives.beginTextCommandSetBlipName('STRING');
            natives.addTextComponentSubstringPlayerName(this.#name);
            natives.endTextCommandSetBlipName(this.#handle);
        }

        set pos(value) {
            const point = new shared.Vector3(value);
            natives.setBlipCoords(this.#handle, point.x, point.y, point.z);
        }

        // --- Остальное убранство метки ---------------------------------------
        //
        // Всё это у игры только на запись: обратных нативов нет ни у одного из
        // них, и последнее заданное хранится рядом — как и у цвета со значком
        // выше. Ошибка тут молчит: метка просто не мигает, и понять, что
        // распоряжение не дошло, нельзя ничем.

        get flashes() { return this.#flashes; }

        set flashes(value) {
            this.#flashes = Boolean(value);
            natives.setBlipFlashes(this.#handle, this.#flashes);
        }

        get flashTimer() { return this.#flashTimer; }

        set flashTimer(value) {
            this.#flashTimer = Number(value) || 0;
            natives.setBlipFlashTimer(this.#handle, this.#flashTimer);
        }

        get flashInterval() { return this.#flashInterval; }

        set flashInterval(value) {
            this.#flashInterval = Number(value) || 0;
            natives.setBlipFlashInterval(this.#handle, this.#flashInterval);
        }

        get bright() { return this.#bright; }

        set bright(value) {
            this.#bright = Boolean(value);
            natives.setBlipBright(this.#handle, this.#bright);
        }

        get isFriendly() { return this.#friendly; }

        set isFriendly(value) {
            this.#friendly = Boolean(value);
            natives.setBlipAsFriendly(this.#handle, this.#friendly);
        }

        get showCone() { return this.#showCone; }

        set showCone(value) {
            this.#showCone = Boolean(value);
            natives.setBlipShowCone(this.#handle, this.#showCone);
        }

        get display() { return this.#display; }

        set display(value) {
            this.#display = Number(value) || 0;
            natives.setBlipDisplay(this.#handle, this.#display);
        }

        get highDetail() { return this.#highDetail; }

        set highDetail(value) {
            this.#highDetail = Boolean(value);
            natives.setBlipHighDetail(this.#handle, this.#highDetail);
        }

        get asMissionCreator() { return this.#missionCreator; }

        set asMissionCreator(value) {
            this.#missionCreator = Boolean(value);
            natives.setBlipAsMissionCreatorBlip(this.#handle, this.#missionCreator);
        }

        get headingIndicatorVisible() { return this.#headingIndicator; }

        set headingIndicatorVisible(value) {
            this.#headingIndicator = Boolean(value);
            natives.showHeadingIndicatorOnBlip(this.#handle, this.#headingIndicator);
        }

        get tickVisible() { return this.#tick; }

        set tickVisible(value) {
            this.#tick = Boolean(value);
            natives.showTickOnBlip(this.#handle, this.#tick);
        }

        get shrinked() { return this.#shrinked; }

        set shrinked(value) {
            this.#shrinked = Boolean(value);
            natives.setBlipShrink(this.#handle, this.#shrinked);
        }

        get number() { return this.#number; }

        set number(value) {
            this.#number = Number(value) || 0;
            natives.showNumberOnBlip(this.#handle, this.#number);
        }

        get secondaryColor() { return this.#secondaryColor; }

        /// Второй цвет задаётся тремя дробными долями, а не номером из палитры:
        /// у натива довод именно такой. Ресурс же вправе дать и RGBA, и три
        /// числа — приводим к тому, что понимает игра.
        set secondaryColor(value) {
            const цвет = new shared.RGBA(value);

            this.#secondaryColor = цвет;

            natives.setBlipSecondaryColour(this.#handle, цвет.r / 255, цвет.g / 255,
                                           цвет.b / 255);
        }

        get gxtName() { return this.#gxtName; }

        /// Подпись из словаря игры, а не своя строка. Тем и отличается от
        /// `name`: та собирается сборщиком текста, а эта берётся по имени из
        /// файла переводов — и переводится вместе с игрой.
        set gxtName(value) {
            this.#gxtName = String(value);
            natives.setBlipNameFromTextFile(this.#handle, this.#gxtName);
        }

        /// Мигнуть один раз. Метод, а не признак: у alt:V это `pulse()`.
        pulse() {
            natives.pulseBlip(this.#handle);
        }

        /// Растворить метку и вернуть её. Доводы — прозрачность и время.
        fade(opacity, duration) {
            natives.setBlipFade(this.#handle, Number(opacity) || 0, Number(duration) || 0);
        }

        /// Чего у игры нет вовсе.
        ///
        /// Нативов на эти четыре нет ни в открытой базе имён, по которой
        /// переводятся хеши, ни в таблице alt:V. Молчать нельзя: метка без
        /// значка друга выглядит точно так же, как метка, которой этот значок
        /// не дошёл.
        get friendIndicatorVisible() { return absent('blip.friendIndicatorVisible')(); }
        set friendIndicatorVisible(_value) { absent('blip.friendIndicatorVisible')(); }

        get crewIndicatorVisible() { return absent('blip.crewIndicatorVisible')(); }
        set crewIndicatorVisible(_value) { absent('blip.crewIndicatorVisible')(); }

        get outlineIndicatorVisible() { return absent('blip.outlineIndicatorVisible')(); }
        set outlineIndicatorVisible(_value) { absent('blip.outlineIndicatorVisible')(); }

        get isHiddenOnLegend() { return absent('blip.isHiddenOnLegend')(); }
        set isHiddenOnLegend(_value) { absent('blip.isHiddenOnLegend')(); }

        destroy() {
            if (!super.valid) {
                return;
            }

            // Дескриптор передаётся выходным доводом: игра обнуляет его сама.
            natives.removeBlip(this.#handle);

            const at = Blip.all.indexOf(this);
            if (at >= 0) {
                Blip.all.splice(at, 1);
            }

            this._forget();
        }
    }

    /// Метка в точке.
    class PointBlip extends Blip {
        constructor(x, y, z) {
            const point = typeof x === 'object' && x !== null ? new shared.Vector3(x)
                                                              : new shared.Vector3(x, y, z);

            super(natives.addBlipForCoord(point.x, point.y, point.z));
        }
    }

    /// Метка кругом заданного радиуса.
    class RadiusBlip extends Blip {
        constructor(x, y, z, radius) {
            super(natives.addBlipForRadius(x, y, z, Number(radius) || 0));
        }
    }

    // --- Маркеры ---------------------------------------------------------------

    /// Фигура, нарисованная в мире.
    ///
    /// Держится не игрой, а нами: у маркера нет дескриптора, его рисуют заново
    /// каждый кадр. Отсюда и устройство — список живых маркеров и один обход по
    /// нему на кадр. Забудь мы про кадр — маркер мигнёт и исчезнет.
    /// Сколько надписей заведено. Служит номерами, как и у маркеров.
    let nextLabelId = 0;

    /// Сколько маркеров заведено. Служит номерами: у alt:V маркер спрашивают
    /// по номеру (`Marker.getByID`), и номер этот клиентский — сервер о
    /// маркерах не знает вовсе.
    let nextMarkerId = 0;

    class Marker extends BaseObject {
        #id;
        #streamingDistance;

        /// Доводы — как у alt:V, и четвёртый здесь **не размер**.
        ///
        /// Прежде четвёртым принимался `scale`, а у alt:V там `useStreaming`.
        /// Ошибка эта молчала вдвойне: `new alt.Marker(t, pos, color, true, 50)`
        /// давал размер `Vector3(true)`, то есть единицу по всем осям — как раз
        /// умолчание, — и маркер выглядел правильным, пока режим не заводил
        /// подгружаемый маркер с размером по умолчанию. Размер ставится
        /// свойством, как и у alt:V.
        constructor(markerType, pos, color, useStreaming, streamingDistance) {
            super();

            this.#id = nextMarkerId++;

            this.markerType = Number(markerType) || 0;
            this.pos = new shared.Vector3(pos);
            this.color = color === undefined ? shared.RGBA.white : new shared.RGBA(color);
            this.scale = new shared.Vector3(1, 1, 1);

            this.dir = shared.Vector3.zero;
            this.rot = shared.Vector3.zero;

            this.visible = true;
            this.bobUpAndDown = false;
            this.faceCamera = false;
            this.rotate = false;

            // Ноль означает «рисовать всегда», как у alt:V без подгрузки.
            this.#streamingDistance = useStreaming ? (Number(streamingDistance) || 0) : 0;

            Marker.all.push(this);
        }

        static all = [];

        static getByID(id) {
            return Marker.all.find((marker) => marker.id === id) ?? null;
        }

        static get count() { return Marker.all.length; }

        get id() { return this.#id; }

        get streamingDistance() { return this.#streamingDistance; }

        /// Всякий заведённый здесь маркер общий: он виден тому, у кого заведён,
        /// и больше никому. Направленные — те, что сервер показывает одному
        /// игроку, — сюда не приходят: канала для них нет.
        get isGlobal() { return true; }

        /// Кому маркер направлен. У общего — никому, и у alt:V тоже `null`.
        /// Отказывать здесь нельзя: `null` — законный ответ, а не умолчание
        /// вместо ответа.
        get target() { return null; }

        /// Виден ли маркер прямо сейчас.
        ///
        /// Спрашивается у расстояния, а не помнится: маркер без подгрузки виден
        /// всегда, а с подгрузкой — пока игрок не ушёл дальше названного.
        get isStreamedIn() {
            if (this.#streamingDistance <= 0) {
                return true;
            }

            const я = natives.getEntityCoords(natives.playerPedId(), true);
            const dx = я.x - this.pos.x;
            const dy = я.y - this.pos.y;
            const dz = я.z - this.pos.z;

            return dx * dx + dy * dy + dz * dz
                <= this.#streamingDistance * this.#streamingDistance;
        }

        /// Рисует себя. Зовётся раз в кадр.
        _draw() {
            if (!this.visible || !this.isStreamedIn) {
                return;
            }

            natives.drawMarker(this.markerType, this.pos.x, this.pos.y, this.pos.z,
                               this.dir.x, this.dir.y, this.dir.z,
                               this.rot.x, this.rot.y, this.rot.z,
                               this.scale.x, this.scale.y, this.scale.z,
                               this.color.r, this.color.g, this.color.b, this.color.a,
                               this.bobUpAndDown, this.faceCamera, 2, this.rotate,
                               // Текстуры нет: маркер сплошной. Пустота здесь —
                               // нулевой указатель, а не пустая строка, и игра
                               // различает их.
                               null, null, false);
        }

        destroy() {
            const at = Marker.all.indexOf(this);
            if (at >= 0) {
                Marker.all.splice(at, 1);
            }

            this._forget();
        }
    }

    // --- Надписи в мире --------------------------------------------------------

    /// Названия шрифтов alt:V и номера, которыми их знает игра.
    ///
    /// Именем, а не числом, потому что так объявлено у alt:V: `fontName`
    /// строкой. Неизвестное имя даёт шрифт по умолчанию — отказывать здесь
    /// нельзя, надпись рисуют из кадра, и исключение тридцать раз в секунду
    /// завалило бы журнал.
    /// Выравнивание alt:V в нумерацию игры.
    ///
    /// У alt:V `Left = 0, Right = 1, Center = 2, Justify = 3`; у игры ноль — «по
    /// центру», единица — «влево», двойка — «вправо». Не совпадает ни одно
    /// число, и перепутанные они молчат: исключения нет, а надпись просто
    /// уезжает за край экрана мимо своей точки.
    ///
    /// Отданное как есть, `Right` приезжало игре центром — это и было видно на
    /// снимке: надпись, поставленная у левого края, была обрезана слева.
    ///
    /// «По ширине» у игры нет вовсе; ближе всего к нему левое.
    function toGameJustification(align) {
        return [1, 2, 0, 1][Number(align) || 0] ?? 1;
    }

    /// Выставляет выравнивание надписи вокруг точки `x`.
    ///
    /// Одного `SET_TEXT_JUSTIFICATION` мало, и это измерено: без рамки игра
    /// равняет не по точке, а **по краю экрана**. Надпись, поставленную у левого
    /// края с выравниванием «вправо», уносило к правому краю целиком.
    ///
    /// Рамку задаёт `SET_TEXT_WRAP`, и задаётся она так, чтобы нужный край
    /// пришёлся ровно на точку: левому выравниванию рамка от точки до правого
    /// края, правому — от левого края до точки. Центр рамки не спрашивает вовсе:
    /// у него свой натив.
    function alignAround(align, x) {
        const кудаИгре = toGameJustification(align);

        natives.setTextJustification(кудаИгре);
        natives.setTextCentre(кудаИгре === 0);

        // Рамка ставится всегда, а не только при выравнивании по краю: своей
        // умолчательной у игры нет, и с прошлого чертежа в ней остаётся то, что
        // положил он. Одна узкая рамка от соседа гасит все надписи кадра.
        if (кудаИгре === 2) {
            natives.setTextWrap(x - 1.0, x);
        } else {
            natives.setTextWrap(x, x + 1.0);
        }

        return кудаИгре;
    }

    const kFonts = {
        chaletlondon: 0,
        housescript: 1,
        monospace: 2,
        charletcomprimecolonge: 4,
        pricedown: 7,
    };

    /// Надпись, висящая в мире.
    ///
    /// Как и маркер, игра её у себя не помнит: текст рисуется заново каждым
    /// кадром. Оттого и живёт она рядом с маркером и рисуется тем же обходом.
    ///
    /// Рисуется через «начало координат отрисовки» (`SET_DRAW_ORIGIN`): текст у
    /// игры плоский, экранный, а этот натив переносит его начало в точку мира и
    /// сам считает, куда она попадает на экране. Другого способа повесить текст
    /// в мире у игры нет.
    class TextLabel extends BaseObject {
        #id;
        #streamingDistance;

        constructor(text, fontName, fontSize, scale, pos, rot, color, outlineWidth,
                    outlineColor, useStreaming, streamingDistance) {
            super();

            this.#id = nextLabelId++;

            this.text = String(text ?? '');
            this.font = String(fontName ?? 'chaletlondon');
            this.fontSize = Number(fontSize) || 1;
            this.scale = Number(scale) || 1;
            this.pos = new shared.Vector3(pos);
            this.rot = rot === undefined ? shared.Vector3.zero : new shared.Vector3(rot);
            this.color = color === undefined ? shared.RGBA.white : new shared.RGBA(color);
            this.outlineWidth = Number(outlineWidth) || 0;
            this.outlineColor = outlineColor === undefined
                ? new shared.RGBA(0, 0, 0, 255) : new shared.RGBA(outlineColor);

            this.align = shared.TextLabelAlignment.Center;
            this.visible = true;

            this.#streamingDistance = useStreaming ? (Number(streamingDistance) || 0) : 0;

            TextLabel.all.push(this);
        }

        static all = [];

        static getByID(id) {
            return TextLabel.all.find((label) => label.id === id) ?? null;
        }

        static get count() { return TextLabel.all.length; }

        get id() { return this.#id; }

        get streamingDistance() { return this.#streamingDistance; }

        /// Всякая заведённая здесь надпись общая: она видна тому, у кого
        /// заведена. Направленных — тех, что сервер показывает одному игроку, —
        /// сюда не приходит.
        get isGlobal() { return true; }

        get isStreamedIn() {
            if (this.#streamingDistance <= 0) {
                return true;
            }

            const я = natives.getEntityCoords(natives.playerPedId(), true);
            const dx = я.x - this.pos.x;
            const dy = я.y - this.pos.y;
            const dz = я.z - this.pos.z;

            return dx * dx + dy * dy + dz * dz
                <= this.#streamingDistance * this.#streamingDistance;
        }

        /// Рисует себя. Зовётся раз в кадр.
        _draw() {
            if (!this.visible || !this.isStreamedIn) {
                return;
            }

            natives.setTextFont(kFonts[this.font.toLowerCase()] ?? 0);

            // Размер шрифта и общий размер перемножаются: у alt:V это два
            // разных числа, а у игры одно.
            const размер = (Number(this.fontSize) || 1) * (Number(this.scale) || 1) * 0.1;
            natives.setTextScale(размер, размер);

            natives.setTextColour(this.color.r, this.color.g, this.color.b, this.color.a);

            // Обводка у игры одна на всё и своего цвета не принимает: натив
            // берётся без доводов. Цвет обводки поэтому доезжает только как
            // «есть она или нет» — сказать об этом честнее, чем принять число и
            // не сделать ничего.
            if (this.outlineWidth > 0) {
                natives.setTextOutline();
            }

            // Начало координат ставится в точку мира, и потому рамка меряется
            // от нуля: смещение от начала, а не место на экране.
            alignAround(this.align, 0);

            // Начало координат отрисовки — точка мира. Последний довод игра не
            // разбирает; ноль здесь её же умолчание.
            natives.setDrawOrigin(this.pos.x, this.pos.y, this.pos.z, 0);

            natives.beginTextCommandDisplayText('STRING');
            natives.addTextComponentSubstringPlayerName(this.text);

            // Нули — смещение от начала координат: надпись стоит ровно в точке.
            natives.endTextCommandDisplayText(0, 0);

            // Снимать обязательно: не снятое начало координат уносит с собой всё
            // остальное, что игра рисует в этом кадре, — интерфейс, чат, метки.
            natives.clearDrawOrigin();
        }

        destroy() {
            const at = TextLabel.all.indexOf(this);
            if (at >= 0) {
                TextLabel.all.splice(at, 1);
            }

            this._forget();
        }
    }

    // --- Курсор и управление ---------------------------------------------------

    /// Сколько раз просили показать курсор.
    ///
    /// Счётчиком, а не признаком, и это не придирка: два окна режима могут
    /// попросить курсор независимо, и закрытие одного не должно отнимать его у
    /// второго. Так же считает и alt:V.
    let cursorRequests = 0;

    function showCursor(show) {
        cursorRequests += show ? 1 : -1;

        if (cursorRequests < 0) {
            cursorRequests = 0;
        }
    }

    /// Отобрано ли у игры управление.
    let controlsEnabled = true;

    // --- Кадр -----------------------------------------------------------------

    /// То, что нужно делать каждый кадр.
    ///
    /// Один обход на всё, а не по обработчику на объект: маркеров режим заводит
    /// сотнями, и сотня подписок стоила бы дороже самой отрисовки.
    alt.drawFrame = function drawFrame() {
        for (const marker of Marker.all) {
            marker._draw();
        }

        // Надписи — после маркеров, и порядок этот значим: текст рисуется
        // поверх, а маркер под ним. Обратный порядок спрятал бы надпись внутри
        // цилиндра маркера, которым её как раз и подписывают.
        for (const label of TextLabel.all) {
            label._draw();
        }

        // Курсор и запрет управления держатся ровно кадр: у игры это натив «на
        // этот кадр», а не признак. Перестань звать — и всё вернётся само.
        if (cursorRequests > 0) {
            natives._showCursorThisFrame();
        }

        if (!controlsEnabled) {
            // 0 — «обычное управление игрока». Отключается всё разом: разбирать
            // по действиям режим будет сам, если ему понадобится.
            natives.disableAllControlActions(0);
        }
    };

    alt.objects = {
        alignAround,
        Blip,
        PointBlip,
        RadiusBlip,
        Marker,
        TextLabel,

        showCursor,

        /// Просил ли кто-нибудь курсор прямо сейчас.
        get cursorVisible() { return cursorRequests > 0; },

        get gameControlsEnabled() { return controlsEnabled; },

        toggleGameControls(enabled) {
            controlsEnabled = Boolean(enabled);
        },
    };
})(globalThis.__oxympAlt);
