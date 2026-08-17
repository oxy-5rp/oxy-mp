// Общая часть API alt:V: то, что одинаково на сервере и на клиенте.
//
// Написано на JavaScript, а не привязками на C++, и это не срезание угла. Здесь
// нет ничего, что требовало бы доступа к сессии: вектор складывается с вектором,
// цвет хранит четыре байта, хеш считается по строке. Привязка на C++ для такого
// стоила бы впятеро больше строк, каждое обращение к полю платило бы переходом
// через границу движка, а Vector3.add вернулся бы в JS всё равно. Так же
// поступает и сам alt:V: его Vector3, Vector2, RGBA и Quaternion написаны на
// JavaScript (shared/bindings/*.js), а на C++ оставлено то, что без движка не
// работает.
//
// Файл исполняется до alt_server.js и складывает готовое в __oxympAlt.shared.

'use strict';

(function build(alt) {
    const enums = alt.enums;

    /// Ошибка о том, чего здесь пока нет.
    ///
    /// Заведена нарочно и зовётся из каждого незаконченного места. Соблазн велик
    /// сделать наоборот — промолчать и вернуть undefined, — и он же губителен:
    /// ресурс, у которого молча ничего не произошло, ищут часами, а ресурс,
    /// сказавший «alt.Voice здесь ещё не сделан», понятен сразу. Молчаливая
    /// заглушка хуже отсутствия: она врёт.
    function absent(what) {
        return function () {
            throw new Error(`${what}: в oxyMP этого ещё нет`);
        };
    }

    /// Годится ли значение в число.
    ///
    /// Проверка своя, потому что Number(null) — это 0, а Number('') — тоже 0, и
    /// вектор, собранный из пустоты, оказался бы в начале координат вместо того,
    /// чтобы пожаловаться.
    function toNumber(value, fallback) {
        const asNumber = typeof value === 'number' ? value : Number(value);
        return Number.isFinite(asNumber) ? asNumber : fallback;
    }

    // --- Векторы -------------------------------------------------------------

    /// Точка или направление в трёх измерениях.
    ///
    /// Неизменяемый: каждое действие возвращает новый вектор, а не правит этот.
    /// Так сделано в alt:V, и причина не в чистоте стиля. Позиция сущности
    /// отдаётся вектором, и будь он изменяемым, `player.pos.z += 10` выглядел бы
    /// как перенос игрока, а на деле правил бы копию и не делал ничего.
    class Vector3 {
        constructor(x, y, z) {
            // Три числа, массив или объект — alt:V принимает все три вида, и
            // ресурсы пользуются всеми тремя.
            if (Array.isArray(x)) {
                this.x = toNumber(x[0], 0);
                this.y = toNumber(x[1], 0);
                this.z = toNumber(x[2], 0);
            } else if (typeof x === 'object' && x !== null) {
                this.x = toNumber(x.x, 0);
                this.y = toNumber(x.y, 0);
                this.z = toNumber(x.z, 0);
            } else {
                this.x = toNumber(x, 0);
                this.y = toNumber(y, 0);
                this.z = toNumber(z, 0);
            }

            Object.freeze(this);
        }

        static get zero() { return new Vector3(0, 0, 0); }
        static get one() { return new Vector3(1, 1, 1); }
        static get up() { return new Vector3(0, 0, 1); }
        static get down() { return new Vector3(0, 0, -1); }
        static get forward() { return new Vector3(0, 1, 0); }
        static get back() { return new Vector3(0, -1, 0); }
        static get left() { return new Vector3(-1, 0, 0); }
        static get right() { return new Vector3(1, 0, 0); }
        static get negativeInfinity() {
            return new Vector3(-Infinity, -Infinity, -Infinity);
        }
        static get positiveInfinity() {
            return new Vector3(Infinity, Infinity, Infinity);
        }

        /// Разбирает довод: вектор, массив, объект или три числа.
        static #parse(args) {
            if (args.length >= 3) {
                return [toNumber(args[0], 0), toNumber(args[1], 0), toNumber(args[2], 0)];
            }

            const value = args[0];

            if (typeof value === 'number') {
                // Одно число означает «то же по всем осям»: так в alt:V пишут
                // умножение на скаляр.
                return [value, value, value];
            }
            if (Array.isArray(value)) {
                return [toNumber(value[0], 0), toNumber(value[1], 0), toNumber(value[2], 0)];
            }
            if (typeof value === 'object' && value !== null) {
                return [toNumber(value.x, 0), toNumber(value.y, 0), toNumber(value.z, 0)];
            }

            return [0, 0, 0];
        }

        add(...args) {
            const [x, y, z] = Vector3.#parse(args);
            return new Vector3(this.x + x, this.y + y, this.z + z);
        }

        sub(...args) {
            const [x, y, z] = Vector3.#parse(args);
            return new Vector3(this.x - x, this.y - y, this.z - z);
        }

        mul(...args) {
            const [x, y, z] = Vector3.#parse(args);
            return new Vector3(this.x * x, this.y * y, this.z * z);
        }

        div(...args) {
            const [x, y, z] = Vector3.#parse(args);
            return new Vector3(this.x / x, this.y / y, this.z / z);
        }

        dot(...args) {
            const [x, y, z] = Vector3.#parse(args);
            return (this.x * x) + (this.y * y) + (this.z * z);
        }

        cross(...args) {
            const [x, y, z] = Vector3.#parse(args);
            return new Vector3((this.y * z) - (this.z * y), (this.z * x) - (this.x * z),
                               (this.x * y) - (this.y * x));
        }

        get length() {
            return Math.sqrt(this.lengthSquared);
        }

        /// Длина без корня.
        ///
        /// Нужна затем же, зачем она нужна в самом сервере: расстояния сравнивают,
        /// а порядок у квадратов тот же самый — корень только тратит время.
        get lengthSquared() {
            return (this.x * this.x) + (this.y * this.y) + (this.z * this.z);
        }

        distanceTo(other) {
            return Math.sqrt(this.distanceToSquared(other));
        }

        distanceToSquared(other) {
            const [x, y, z] = Vector3.#parse([other]);
            const dx = this.x - x;
            const dy = this.y - y;
            const dz = this.z - z;

            return (dx * dx) + (dy * dy) + (dz * dz);
        }

        isInRange(range, other) {
            return this.distanceToSquared(other) <= range * range;
        }

        get normalize() {
            const length = this.length;

            // Нулевой вектор нормализуется в себя, а не в NaN: направления у него
            // нет, и деление на ноль ответило бы тремя NaN, которые потом всплыли
            // бы за десять вызовов отсюда.
            return length === 0 ? Vector3.zero
                                : new Vector3(this.x / length, this.y / length, this.z / length);
        }

        negative() {
            return new Vector3(-this.x, -this.y, -this.z);
        }

        inverse() {
            return new Vector3(1 / this.x, 1 / this.y, 1 / this.z);
        }

        lerp(other, ratio) {
            const [x, y, z] = Vector3.#parse([other]);
            const step = toNumber(ratio, 0);

            return new Vector3(this.x + ((x - this.x) * step), this.y + ((y - this.y) * step),
                               this.z + ((z - this.z) * step));
        }

        angleTo(other) {
            const [x, y, z] = Vector3.#parse([other]);
            const lengths = Math.sqrt(this.lengthSquared) *
                            Math.sqrt((x * x) + (y * y) + (z * z));

            if (lengths === 0) {
                return 0;
            }

            // Обрезка перед арккосинусом обязательна: округление даёт 1.0000000002,
            // а Math.acos от такого — NaN.
            return Math.acos(Math.min(1, Math.max(-1, this.dot(other) / lengths)));
        }

        angleToDegrees(other) {
            return this.angleTo(other) * (180 / Math.PI);
        }

        toRadians() {
            const step = Math.PI / 180;
            return new Vector3(this.x * step, this.y * step, this.z * step);
        }

        toDegrees() {
            const step = 180 / Math.PI;
            return new Vector3(this.x * step, this.y * step, this.z * step);
        }

        toArray() {
            return [this.x, this.y, this.z];
        }

        toString() {
            return `Vector3{ x: ${this.x}, y: ${this.y}, z: ${this.z} }`;
        }

        /// Что уходит в JSON.
        ///
        /// Обычный объект, а не строка: ресурсы кладут позицию в ответ странице
        /// интерфейса, и страница ждёт `{x, y, z}`, а не «Vector3{...}».
        toJSON() {
            return { x: this.x, y: this.y, z: this.z };
        }
    }

    /// То же на плоскости.
    class Vector2 {
        constructor(x, y) {
            if (Array.isArray(x)) {
                this.x = toNumber(x[0], 0);
                this.y = toNumber(x[1], 0);
            } else if (typeof x === 'object' && x !== null) {
                this.x = toNumber(x.x, 0);
                this.y = toNumber(x.y, 0);
            } else {
                this.x = toNumber(x, 0);
                this.y = toNumber(y, 0);
            }

            Object.freeze(this);
        }

        static get zero() { return new Vector2(0, 0); }
        static get one() { return new Vector2(1, 1); }
        static get up() { return new Vector2(0, 1); }
        static get down() { return new Vector2(0, -1); }
        static get left() { return new Vector2(-1, 0); }
        static get right() { return new Vector2(1, 0); }

        static #parse(args) {
            if (args.length >= 2) {
                return [toNumber(args[0], 0), toNumber(args[1], 0)];
            }

            const value = args[0];

            if (typeof value === 'number') {
                return [value, value];
            }
            if (Array.isArray(value)) {
                return [toNumber(value[0], 0), toNumber(value[1], 0)];
            }
            if (typeof value === 'object' && value !== null) {
                return [toNumber(value.x, 0), toNumber(value.y, 0)];
            }

            return [0, 0];
        }

        add(...args) {
            const [x, y] = Vector2.#parse(args);
            return new Vector2(this.x + x, this.y + y);
        }

        sub(...args) {
            const [x, y] = Vector2.#parse(args);
            return new Vector2(this.x - x, this.y - y);
        }

        mul(...args) {
            const [x, y] = Vector2.#parse(args);
            return new Vector2(this.x * x, this.y * y);
        }

        div(...args) {
            const [x, y] = Vector2.#parse(args);
            return new Vector2(this.x / x, this.y / y);
        }

        dot(...args) {
            const [x, y] = Vector2.#parse(args);
            return (this.x * x) + (this.y * y);
        }

        get length() {
            return Math.sqrt(this.lengthSquared);
        }

        get lengthSquared() {
            return (this.x * this.x) + (this.y * this.y);
        }

        distanceTo(other) {
            return Math.sqrt(this.distanceToSquared(other));
        }

        distanceToSquared(other) {
            const [x, y] = Vector2.#parse([other]);
            const dx = this.x - x;
            const dy = this.y - y;

            return (dx * dx) + (dy * dy);
        }

        isInRange(range, other) {
            return this.distanceToSquared(other) <= range * range;
        }

        get normalize() {
            const length = this.length;
            return length === 0 ? Vector2.zero : new Vector2(this.x / length, this.y / length);
        }

        negative() {
            return new Vector2(-this.x, -this.y);
        }

        toArray() {
            return [this.x, this.y];
        }

        toString() {
            return `Vector2{ x: ${this.x}, y: ${this.y} }`;
        }

        toJSON() {
            return { x: this.x, y: this.y };
        }
    }

    /// Цвет с прозрачностью.
    class RGBA {
        constructor(r, g, b, a) {
            if (Array.isArray(r)) {
                [r, g, b, a] = r;
            } else if (typeof r === 'object' && r !== null) {
                ({ r, g, b, a } = r);
            }

            // Обрезка по байту здесь, а не при отправке: цвет, вышедший за
            // границу, обнаружится в месте, где его собрали, а не там, где он
            // понадобился.
            const byte = (value, fallback) =>
                Math.min(255, Math.max(0, Math.round(toNumber(value, fallback))));

            this.r = byte(r, 0);
            this.g = byte(g, 0);
            this.b = byte(b, 0);

            // Непрозрачность по умолчанию: цвет без четвёртого довода — сплошной,
            // а не невидимый.
            this.a = byte(a, 255);

            Object.freeze(this);
        }

        static get red() { return new RGBA(255, 0, 0, 255); }
        static get green() { return new RGBA(0, 255, 0, 255); }
        static get blue() { return new RGBA(0, 0, 255, 255); }
        static get black() { return new RGBA(0, 0, 0, 255); }
        static get white() { return new RGBA(255, 255, 255, 255); }
        static get clear() { return new RGBA(0, 0, 0, 0); }

        toArray() {
            return [this.r, this.g, this.b, this.a];
        }

        toString() {
            return `RGBA{ r: ${this.r}, g: ${this.g}, b: ${this.b}, a: ${this.a} }`;
        }

        toJSON() {
            return { r: this.r, g: this.g, b: this.b, a: this.a };
        }
    }

    /// Поворот кватернионом.
    ///
    /// Нужен затем же, зачем он нужен игре: поворот тремя углами теряет одну
    /// степень свободы, когда две оси совпадают, и объект в этот миг дёргается.
    class Quaternion {
        constructor(x, y, z, w) {
            if (Array.isArray(x)) {
                [x, y, z, w] = x;
            } else if (typeof x === 'object' && x !== null) {
                ({ x, y, z, w } = x);
            }

            this.x = toNumber(x, 0);
            this.y = toNumber(y, 0);
            this.z = toNumber(z, 0);
            this.w = toNumber(w, 1);

            Object.freeze(this);
        }

        static get zero() { return new Quaternion(0, 0, 0, 1); }

        get length() {
            return Math.sqrt((this.x * this.x) + (this.y * this.y) + (this.z * this.z) +
                             (this.w * this.w));
        }

        get conjugate() {
            return new Quaternion(-this.x, -this.y, -this.z, this.w);
        }

        get normalize() {
            const length = this.length;

            return length === 0 ? Quaternion.zero
                                : new Quaternion(this.x / length, this.y / length,
                                                 this.z / length, this.w / length);
        }

        toArray() {
            return [this.x, this.y, this.z, this.w];
        }

        toString() {
            return `Quaternion{ x: ${this.x}, y: ${this.y}, z: ${this.z}, w: ${this.w} }`;
        }

        toJSON() {
            return { x: this.x, y: this.y, z: this.z, w: this.w };
        }
    }

    // --- Хеш -----------------------------------------------------------------

    /// Хеш имени в нумерации игры — тот самый joaat.
    ///
    /// Повторяет shared/include/oxymp/shared/math/joaat.hpp слово в слово, и это
    /// обязано оставаться так: скрипт называет машину «adder», сервер ищет её по
    /// числу, и разойдись эти две функции хоть в одном разряде — машина не
    /// нашлась бы, а объяснения не было бы никакого.
    ///
    /// `>>> 0` после каждого шага не украшение: побитовые действия в JavaScript
    /// дают знаковое 32-разрядное число, и без приведения хеш вышел бы
    /// отрицательным ровно в половине случаев.
    function hash(text) {
        if (typeof text !== 'string') {
            return 0;
        }

        let result = 0;

        for (let i = 0; i < text.length; i++) {
            let letter = text.charCodeAt(i);

            // К нижнему регистру только латиницу — как это делает и сама игра, и
            // наш joaat: трогать байты чужой кодировки значило бы портить их.
            if (letter >= 0x41 && letter <= 0x5A) {
                letter += 0x20;
            }

            result = (result + letter) >>> 0;
            result = (result + (result << 10)) >>> 0;
            result = (result ^ (result >>> 6)) >>> 0;
        }

        result = (result + (result << 3)) >>> 0;
        result = (result ^ (result >>> 11)) >>> 0;
        result = (result + (result << 15)) >>> 0;

        return result >>> 0;
    }

    // --- Сборка модуля -------------------------------------------------------

    const shared = {
        Vector3,
        Vector2,
        RGBA,
        Quaternion,
        hash,

        /// Значение, которым alt:V метит «нет сущности».
        get defaultDimension() { return 0; },
        get globalDimension() { return -2147483648; },

        // Того, чего ещё нет, — с внятным отказом вместо тишины.
        File: { exists: absent('alt.File'), read: absent('alt.File') },
    };

    Object.assign(shared, enums);

    alt.shared = shared;
})(globalThis.__oxympAlt);
