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

    /// Всё, что заведено скриптом и живёт до destroy().
    class BaseObject {
        #alive = true;

        get valid() {
            return this.#alive;
        }

        /// Помечает объект убранным. Наследник убирает своё сам.
        _forget() {
            this.#alive = false;
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
        #priority = 0;
        #category = 0;

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

        set routeColor(value) {
            natives.setBlipRouteColour(this.#handle, Number(value) || 0);
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
    class Marker extends BaseObject {
        constructor(markerType, pos, color, scale) {
            super();

            this.markerType = Number(markerType) || 0;
            this.pos = new shared.Vector3(pos);
            this.color = color === undefined ? shared.RGBA.white : new shared.RGBA(color);
            this.scale = scale === undefined ? new shared.Vector3(1, 1, 1)
                                             : new shared.Vector3(scale);

            this.dir = shared.Vector3.zero;
            this.rot = shared.Vector3.zero;

            this.visible = true;
            this.bobUpAndDown = false;
            this.faceCamera = false;
            this.rotate = false;

            Marker.all.push(this);
        }

        static all = [];

        /// Рисует себя. Зовётся раз в кадр.
        _draw() {
            if (!this.visible) {
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
        Blip,
        PointBlip,
        RadiusBlip,
        Marker,

        showCursor,

        /// Просил ли кто-нибудь курсор прямо сейчас.
        get cursorVisible() { return cursorRequests > 0; },

        get gameControlsEnabled() { return controlsEnabled; },

        toggleGameControls(enabled) {
            controlsEnabled = Boolean(enabled);
        },
    };
})(globalThis.__oxympAlt);
