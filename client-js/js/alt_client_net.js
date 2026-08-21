// Сеть ресурса на клиенте: alt.HttpClient и alt.WebSocketClient.
//
// Обе живут поверх того, что уже умеет Node, и это главное решение этого файла.
// Своего клиента HTTP и своего разбора кадров WebSocket здесь нет и не будет:
// Node 24 несёт и `http`/`https`, и настоящий глобальный `WebSocket`, а всё,
// чего недостаёт, — это имена и обещания в том виде, в каком их ждёт ресурс,
// написанный под alt:V.
//
// Обе принадлежат ресурсу сервера, а не игроку, и это стоит понимать буквально.
// Своей волей игрок ни одной строки отсюда не вызовет: у клиента нет ни меню, ни
// команд, ни клавиш. Ходит в сеть ресурс, который поднял сервер, — ровно так же,
// как это устроено у alt:V.
//
// Файл исполняется после alt_client_locals.js и до alt_client.js.

'use strict';

(function build(alt) {
    const { request: httpRequest } = require('http');
    const { request: httpsRequest } = require('https');

    // --- HTTP ----------------------------------------------------------------

    /// Заголовки ответа: у Node они объект, у alt:V — тоже, но со строками.
    ///
    /// Node складывает повторяющиеся заголовки в набор (`set-cookie` приходит
    /// именно так). Ресурс же ждёт строку, и склеенная запятой — то, что отдаёт
    /// alt:V и чего ждёт всякий, кто эти заголовки читает.
    function flattenHeaders(headers) {
        const flat = {};

        for (const [name, value] of Object.entries(headers ?? {})) {
            flat[name] = Array.isArray(value) ? value.join(', ') : String(value);
        }

        return flat;
    }

    /// Один запрос. Обещание, как и у alt:V.
    ///
    /// Отказ обещания, а не пустой ответ: сеть могла не отозваться, и ресурс,
    /// принявший тишину за пустое тело, унёс бы эту ложь дальше. Ответ же с
    /// кодом 404 — не отказ: сервер ответил, и ответ его законный.
    function perform(method, url, body, headers) {
        return new Promise((resolve, reject) => {
            let address = null;

            try {
                address = new URL(String(url));
            } catch {
                reject(new Error(`alt.HttpClient: адрес «${url}» не разобран`));
                return;
            }

            if (address.protocol !== 'http:' && address.protocol !== 'https:') {
                reject(new Error(`alt.HttpClient: ${address.protocol} не поддерживается`));
                return;
            }

            const send = address.protocol === 'https:' ? httpsRequest : httpRequest;
            const payload = body === undefined || body === null ? null : String(body);

            const outgoing = { ...headers };

            // Длина тела считается в байтах, а не в знаках: кириллица в UTF-8
            // занимает два байта на букву, и заголовок с числом знаков обрезал бы
            // тело на середине слова.
            if (payload !== null && outgoing['Content-Length'] === undefined) {
                outgoing['Content-Length'] = Buffer.byteLength(payload);
            }

            const call = send(address, { method, headers: outgoing }, (answer) => {
                let text = '';

                answer.setEncoding('utf8');
                answer.on('data', (part) => { text += part; });
                answer.on('end', () => resolve({
                    statusCode: answer.statusCode ?? 0,
                    body: text,
                    headers: flattenHeaders(answer.headers),
                }));
            });

            call.on('error', (failure) => reject(failure));

            if (payload !== null) {
                call.write(payload);
            }

            call.end();
        });
    }

    /// Клиент HTTP, каким его знает ресурс alt:V.
    ///
    /// Заголовки живут у клиента, а не у запроса: так это устроено у alt:V —
    /// ресурс заводит один клиент, кладёт в него ключ доступа и потом ходит им
    /// по всем своим адресам.
    class HttpClient {
        #headers = {};

        setExtraHeader(header, value) {
            this.#headers[String(header)] = String(value);
        }

        getExtraHeaders() {
            // Копия, а не сам набор: отданный наружу, он позволил бы менять
            // заголовки в обход setExtraHeader — и молча.
            return { ...this.#headers };
        }

        get(url) { return perform('GET', url, null, this.#headers); }
        head(url) { return perform('HEAD', url, null, this.#headers); }
        post(url, body) { return perform('POST', url, body, this.#headers); }
        put(url, body) { return perform('PUT', url, body, this.#headers); }
        delete(url, body) { return perform('DELETE', url, body, this.#headers); }
        connect(url, body) { return perform('CONNECT', url, body, this.#headers); }
        options(url, body) { return perform('OPTIONS', url, body, this.#headers); }
        trace(url, body) { return perform('TRACE', url, body, this.#headers); }
        patch(url, body) { return perform('PATCH', url, body, this.#headers); }

        toString() { return 'HttpClient{}'; }
    }

    // --- WebSocket -----------------------------------------------------------

    /// Состояния связи в нумерации alt:V. Они же и у самого WebSocket.
    const kConnecting = 0;
    const kOpen = 1;
    const kClosing = 2;
    const kClosed = 3;

    /// Насколько ждать перед повторным подключением, в миллисекундах.
    ///
    /// Число одно и небольшое: у alt:V `autoReconnect` тоже без нарастающей
    /// задержки. Растягивать её самим нельзя не подумав — ресурс, ждущий связи
    /// через секунду, получил бы её через минуту и решил бы, что сервер лежит.
    const kReconnectDelay = 1000;

    /// Связь по WebSocket, какой её знает ресурс alt:V.
    ///
    /// Поверх глобального WebSocket из Node, а не своего разбора кадров. Тот
    /// умеет всё, что здесь нужно: рукопожатие, маскирование, пинг-понг и
    /// сжатие сообщений. Писать это заново значило бы завести вторую реализацию
    /// протокола и сопровождать её вечно.
    ///
    /// Чего он не умеет — сказано вслух: свои заголовки при рукопожатии
    /// стандартный WebSocket не принимает.
    class WebSocketClient {
        #socket = null;
        #handlers = new Map();
        #protocols = [];
        #wanted = false;

        constructor(url) {
            this.url = String(url ?? '');
            this.autoReconnect = false;
            this.perMessageDeflate = false;
            this.pingInterval = 0;
        }

        get readyState() {
            return this.#socket === null ? kClosed : this.#socket.readyState;
        }

        on(name, handler) {
            if (typeof handler !== 'function') {
                return;
            }

            const key = String(name);

            if (!this.#handlers.has(key)) {
                this.#handlers.set(key, []);
            }

            this.#handlers.get(key).push(handler);
        }

        off(name, handler) {
            const kept = this.#handlers.get(String(name));
            if (kept === undefined) {
                return;
            }

            const at = kept.indexOf(handler);
            if (at !== -1) {
                kept.splice(at, 1);
            }
        }

        getEventListeners(name) {
            return [...(this.#handlers.get(String(name)) ?? [])];
        }

        addSubProtocol(protocol) {
            this.#protocols.push(String(protocol));
        }

        getSubProtocols() {
            return [...this.#protocols];
        }

        setExtraHeader(header, value) {
            // Распоряжение, которого мы не умеем исполнить: говорит один раз и
            // возвращается. Стандартный WebSocket своих заголовков при
            // рукопожатии не принимает, а бросок отсюда унёс бы с собой всё, что
            // ресурс делает следом.
            alt.locals.warnOnce(
                'websocket.setExtraHeader',
                `свои заголовки при рукопожатии не передаются (${header}: ${value})`);
        }

        start() {
            this.#wanted = true;

            if (this.#socket !== null) {
                return;
            }

            let socket = null;

            try {
                socket = this.#protocols.length === 0
                    ? new WebSocket(this.url)
                    : new WebSocket(this.url, this.#protocols);
            } catch (failure) {
                // Событием, а не броском: `start` стоит посреди чужого
                // обработчика, и брошенное отсюда унесло бы всё, что идёт следом.
                this.#fire('error', [String(failure?.message ?? failure)]);
                return;
            }

            this.#socket = socket;

            socket.addEventListener('open', () => this.#fire('open', []));
            socket.addEventListener('message', (event) => this.#fire('message', [event.data]));
            socket.addEventListener('error', () => this.#fire('error', ['ошибка связи']));

            socket.addEventListener('close', (event) => {
                this.#socket = null;
                this.#fire('close', [event.code ?? 0, event.reason ?? '']);

                if (this.autoReconnect && this.#wanted) {
                    // Обычный setTimeout от Node, а не alt.setTimeout: тот
                    // объявляется в alt_client.js, который исполняется позже
                    // этого файла. Крутит их обоих один и тот же цикл событий,
                    // который клиент прокручивает каждым кадром.
                    setTimeout(() => {
                        if (this.autoReconnect && this.#wanted) {
                            this.start();
                        }
                    }, kReconnectDelay);
                }
            });
        }

        stop() {
            // Признак снимается до закрытия: обработчик close сработает и
            // проверит его, и без этого связь поднялась бы заново сразу после
            // того, как её попросили прекратить.
            this.#wanted = false;

            if (this.#socket !== null) {
                this.#socket.close();
            }
        }

        send(message) {
            if (this.#socket === null || this.#socket.readyState !== kOpen) {
                return false;
            }

            this.#socket.send(message);
            return true;
        }

        toString() { return `WebSocketClient{ url: ${this.url} }`; }

        #fire(name, args) {
            // Копия списка нарочно: обработчик волен отписаться прямо отсюда.
            for (const handler of [...(this.#handlers.get(name) ?? [])]) {
                try {
                    handler(...args);
                } catch (failure) {
                    // Упавший обработчик не уносит ни остальных, ни связь.
                    alt.client.logError(`websocket ${name}: ${failure?.stack ?? failure}`);
                }
            }
        }
    }

    alt.net = {
        HttpClient,
        WebSocketClient,

        /// Числа состояний — наружу, ресурсу: у alt:V это перечисление
        /// WebSocketReadyState, и режимы сравнивают с ним `readyState`.
        WebSocketReadyState: {
            Connecting: kConnecting,
            Open: kOpen,
            Closing: kClosing,
            Closed: kClosed,
        },
    };
})(globalThis.__oxympAlt);
