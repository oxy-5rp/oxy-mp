/* Граница между клиентом и его скриптовой машиной.
 *
 * На C, а не на C++, и это не дань привычке. Клиент собирается со статической
 * библиотекой времени выполнения (`/MT`) — иначе Windows не найдёт ему
 * `msvcp140.dll`, потому что ищет её рядом с `GTA5.exe`, а не рядом с ним. А
 * машина обязана быть динамической (`/MD`): рядом с ней живёт `libnode.dll`,
 * собранный так же, и иначе Node не собирается вовсе. Подробности — в
 * `cmake/dual_runtime.cmake`.
 *
 * Смешать `/MT` и `/MD` в одном бинарнике нельзя, поэтому машина живёт отдельной
 * DLL. А раз она отдельная и куча у неё своя, через границу не имеет права
 * ходить ничего, что выделено одной стороной и освобождается другой:
 * `std::string`, `std::vector`, умный указатель. Выделено там, освобождается
 * здесь — это не «немного неаккуратно», а порча памяти, которая проявится за
 * час игры и в другом месте.
 *
 * Отсюда правила, которые здесь соблюдаются без исключений:
 *
 *   - только типы C и указатели на них;
 *   - строки и буферы передаются указателем и длиной, и **принадлежат
 *     передающему**: получатель обязан либо употребить их сразу, либо
 *     скопировать себе;
 *   - строки не обязаны оканчиваться нулём — длина главнее;
 *   - ни одна сторона не освобождает памяти другой.
 *
 * Так же устроен и alt:V: его клиентский `js-module.dll` — отдельная библиотека
 * ровно по этой же причине.
 */

#ifndef OXYMP_CLIENT_JS_ABI_H
#define OXYMP_CLIENT_JS_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Версия границы.
 *
 * Проверяется при загрузке, и несовпадение — отказ, а не попытка. Клиент и
 * машина раздаются вместе, но у человека на диске может оказаться DLL от
 * прошлой сборки, и разобрать чужую раскладку структуры она попытается молча.
 *
 * Увеличивается при всяком изменении состава или порядка полей ниже.
 */
#define OXYMP_CLIENT_JS_ABI_VERSION 1u

/* Строка: указатель и длина. Нулём оканчиваться не обязана. */
typedef struct OxympJsText {
    const char* data;
    uint32_t length;
} OxympJsText;

/* Двоичные данные: указатель и длина. */
typedef struct OxympJsBytes {
    const uint8_t* data;
    uint32_t length;
} OxympJsBytes;

/* Уровень строки в журнале. Числа те же, что у уровней spdlog по смыслу. */
typedef enum OxympJsLogLevel {
    kOxympJsLogInfo = 0,
    kOxympJsLogWarning = 1,
    kOxympJsLogError = 2,
    kOxympJsLogDebug = 3
} OxympJsLogLevel;

/* Сколько ячеек вмещает вызов натива.
 *
 * То же число, что у NativeContext на стороне клиента: доводы кладутся в буфер
 * одинаковых восьмибайтовых ячеек, и обе стороны обязаны считать его размер
 * одинаково.
 */
#define OXYMP_JS_NATIVE_CELLS 32u

/* Что машина просит у клиента.
 *
 * Заполняет клиент, зовёт машина. Указатель `context` возвращается в каждом
 * вызове неизменным — через него клиент находит себя, не заводя глобальных.
 */
typedef struct OxympJsHost {
    void* context;

    /* Строка в журнал клиента. */
    void (*log)(void* context, OxympJsLogLevel level, OxympJsText resource, OxympJsText line);

    /* Именованное событие серверу. Нагрузка — уложенный набор доводов. */
    void (*emitServer)(void* context, OxympJsText name, OxympJsBytes payload);

    /* Вызов натива игры.
     *
     * Доводы и результаты — ячейками по восемь байт, как их и принимает сама
     * игра. Толкование ячеек — забота зовущего: игра не знает типов, она знает
     * ширину.
     *
     * Возвращает 0 при отказе — натив не разрешился в адрес. Ноль отличается от
     * «натив вернул ноль» именно тем, что при отказе `results` не трогается.
     */
    int32_t (*callNative)(void* context, uint64_t hash, const uint64_t* arguments,
                          uint32_t argumentCount, uint64_t* results, uint32_t resultCapacity);

    /* Читает файл ресурса из кеша клиента.
     *
     * Отдаёт указатель на содержимое, которое принадлежит клиенту и живо до
     * следующего вызова этого же метода. Пусто — файла нет.
     */
    OxympJsBytes (*readResourceFile)(void* context, OxympJsText resource, OxympJsText file);

    /* Заводит окно интерфейса поверх кадра. Возвращает его номер, 0 — отказ. */
    uint32_t (*createWebView)(void* context, OxympJsText resource, OxympJsText url);

    /* Убирает окно. */
    void (*destroyWebView)(void* context, uint32_t view);

    /* Событие странице окна. */
    void (*emitWebView)(void* context, uint32_t view, OxympJsText name, OxympJsBytes payload);

    /* Показывать ли окно и брать ли им мышь. */
    void (*setWebViewVisible)(void* context, uint32_t view, int32_t visible);
    void (*setWebViewFocused)(void* context, uint32_t view, int32_t focused);
} OxympJsHost;

/* Что умеет машина.
 *
 * Заполняет машина, зовёт клиент. Все вызовы — из одного потока, того самого, на
 * котором идёт такт клиента: движок V8 не переносит обращения из двух потоков
 * без блокировки, а заводить её ради того, чего не бывает, незачем.
 */
typedef struct OxympJsEngine {
    /* Поднимает машину. 0 — отказ. Зовётся один раз за жизнь процесса. */
    int32_t (*setUp)(void);

    /* Поднимает ресурс: имя, корень в кеше и точка входа от корня.
     *
     * Причина отказа уходит в журнал клиента через host->log, а не возвращается
     * строкой: строка через границу потребовала бы владения, а владеть ей здесь
     * некому.
     */
    int32_t (*startResource)(OxympJsText name, OxympJsText root, OxympJsText entry);

    /* Останавливает ресурс. */
    void (*stopResource)(OxympJsText name);

    /* Даёт движку поработать: таймеры, обещания, ввод-вывод. Раз в такт. */
    void (*tick)(void);

    /* Событие от сервера — всем поднятым ресурсам. */
    void (*dispatchServerEvent)(OxympJsText name, OxympJsBytes payload);

    /* Событие от страницы интерфейса. */
    void (*dispatchWebViewEvent)(uint32_t view, OxympJsText name, OxympJsBytes payload);

    /* Событие сессии без нагрузки: connectionComplete, disconnect и прочие. */
    void (*dispatchSessionEvent)(OxympJsText name);

    /* Нажата или отпущена клавиша. */
    void (*dispatchKey)(uint32_t key, int32_t down);

    /* Разбирает всё и отпускает движок. */
    void (*tearDown)(void);
} OxympJsEngine;

/* Единственное, что машина выставляет наружу.
 *
 * Одна точка входа, а не десяток экспортов: так проверка версии случается
 * раньше первого обращения к чему бы то ни было, и раскладку структур ниже
 * никто не успевает истолковать по-своему.
 *
 * Возвращает NULL, если версия границы не совпала.
 */
typedef const OxympJsEngine* (*OxympJsEntry)(uint32_t abiVersion, const OxympJsHost* host);

/* Имя этой функции в таблице экспорта. */
#define OXYMP_CLIENT_JS_ENTRY_NAME "oxympClientJsEntry"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OXYMP_CLIENT_JS_ABI_H */
