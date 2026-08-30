"""Чего у нас нет из того, что живой режим зовёт на самом деле.

    python tools/altvparity/used_by_mode.py <index.d.ts> <корень режима> [файлы...]

Главное число паритета. Общее в `missing.py` включает поезда, бронестёкла и
подвеску поколёсно — то, чего не трогает никто; здесь остаётся только то, на чём
режим споткнётся.

Файлы нашей стороны можно не называть: по имени объявлений (`types-server` или
`types-client`) берётся свой набор — слой **и ядро**. Ядро здесь обязательно, и
это не мелочь: почти все члены игрока и машины приходят прямо из `bindings.cpp`,
а в слое их нет вовсе. Забыв ядро, сверка объявляет отсутствующими два десятка
готовых имён — проверено на себе.

Отсюда общее правило для сверок этого рода: **список «у нас нет» надёжен ровно
настолько, насколько полон перечень того, где искали.** Поэтому перечень тут свой,
а не на памяти зовущего.
"""

from __future__ import annotations

import glob
import os
import pathlib
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from extract import extract

# Где живёт наша сторона. Слой и ядро, порознь для сервера и для клиента.
НАШИ = {
    "server": ["script-js/js/alt_*.js", "script-js/src/*.cpp"],
    # Клиентская сторона шире серверной на один каталог: события сессии
    # (`connectionComplete`, `disconnect`) объявляет не слой и не машина, а сам
    # клиент — `GameSession::runScripts`. Без него сверка объявляет их немыми.
    "client": ["client-js/js/alt_*.js", "client-js/src/*.cpp", "client/src/*.cpp"],
}

# Что сверяется. Порядок для вывода, а не для дела.
КЛАССЫ = ["Player", "Vehicle", "Entity", "Blip", "Colshape",
          "Checkpoint", "Ped", "Object", "Marker"]

# Как режимы называют сам модуль alt. Нужно для верхних имён — тех, что зовут
# не у сущности, а у модуля: `alt.emit`, `alt.setTimeout`.
ПСЕВДОНИМЫ = ["alt", "altServer", "altClient", "altShared"]


def сторона(declarations: str) -> str:
    """Серверные это объявления или клиентские — по имени файла."""
    return "client" if "types-client" in declarations.replace("\\", "/") else "server"


def имена(patterns: list[str]) -> set[str]:
    """Все слова из наших файлов. Грубо и нарочно.

    Сверка отвечает «нет» надёжно, а «есть» — оценкой сверху: слово могло
    попасть сюда из комментария. Так и задумано — ложная тревога дешевле
    пропущенной нехватки.
    """
    найдено: set[str] = set()

    for образец in patterns:
        for файл in glob.glob(образец):
            текст = open(файл, encoding="utf-8", errors="replace").read()
            найдено |= set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", текст))

    return найдено


def зовёт(root: str) -> set[str]:
    """Всё, что режим спрашивает через точку."""
    найдено: set[str] = set()

    for каталог, _, файлы in os.walk(root):
        for имя in файлы:
            if not имя.endswith((".ts", ".cjs", ".js")):
                continue

            try:
                текст = развернуть(open(os.path.join(каталог, имя), encoding="utf-8",
                                             errors="replace").read())
            except OSError:
                continue

            найдено |= set(re.findall(r"\.([A-Za-z_][A-Za-z0-9_]*)", текст))

    return найдено


def верхние(declarations: str) -> set[str]:
    """Имена, которые alt:V объявляет у самого модуля, а не у класса."""
    текст = open(declarations, encoding="utf-8", errors="replace").read()
    текст = re.sub(r"/\*.*?\*/", "", текст, flags=re.S)

    return set(re.findall(r"^\s*export function (\w+)", текст, re.M))


def зовёт_у_модуля(root: str) -> set[str]:
    """То, что режим зовёт именно у модуля alt, а не у чего попало.

    Точнее, чем `зовёт`, и нарочно. У членов класса свободный поиск по точке
    верен: игроком может оказаться любой объект. У верхних имён — нет:
    `Utils.loadMapArea` и `camera.getCamPos` совпали бы с объявлениями alt:V,
    не имея к ним отношения. Ровно четыре таких совпадения и вышло на первом
    прогоне, и все четыре оказались собственными объектами режима.
    """
    образец = re.compile(r"\b(?:%s)\.(\w+)\s*\(" % "|".join(ПСЕВДОНИМЫ))
    найдено: set[str] = set()

    for каталог, _, файлы in os.walk(root):
        for имя in файлы:
            if not имя.endswith((".ts", ".cjs", ".js")):
                continue

            try:
                текст = развернуть(open(os.path.join(каталог, имя), encoding="utf-8",
                                             errors="replace").read())
            except OSError:
                continue

            найдено |= set(образец.findall(текст))

    return найдено


def события(declarations: str) -> set[str]:
    """Имена событий, которые объявляет alt:V.

    Лежат они не перечислением, а полями интерфейса `IClientEvent` или
    `IServerEvent`: `on` объявлен по ключам этого интерфейса.
    """
    текст = open(declarations, encoding="utf-8", errors="replace").read()
    начало = re.search(r"interface I(?:Client|Server)Event\s*{", текст)

    if начало is None:
        return set()

    глубина, конец = 0, начало.end()
    for место, знак in enumerate(текст[начало.end() - 1:], начало.end() - 1):
        глубина += (знак == "{") - (знак == "}")
        if глубина == 0:
            конец = место
            break

    return set(re.findall(r"^\s+(\w+)\s*[?:(]",
                          текст[начало.end():конец], re.M))


def подписки(root: str) -> set[str]:
    """На какие события режим подписывается."""
    образец = re.compile(
        r"\b(?:%s)\.(?:on|once|onClient|onServer)\(\s*['\"](\w+)"
        % "|".join(ПСЕВДОНИМЫ))
    найдено: set[str] = set()

    for каталог, _, файлы in os.walk(root):
        for имя in файлы:
            if not имя.endswith((".ts", ".cjs", ".js")):
                continue

            try:
                найдено |= set(образец.findall(развернуть(
                    open(os.path.join(каталог, имя), encoding="utf-8",
                         errors="replace").read())))
            except OSError:
                continue

    return найдено


def немые_у_нас(patterns: list[str]) -> set[str]:
    """События, про которые слой сам говорит, что не объявляет их ни разу.

    Берётся из карты `неОбъявляемые` в слое, а не угадывается. Иначе выходит
    ложь наоборот: имя такого события в наших файлах **есть** — оно там ключом
    жалобы, — и свободный поиск считает его объявляемым. Так сверка молчала про
    три события, которые живой запуск нашёл в первую же минуту.
    """
    найдено: set[str] = set()

    for образец in patterns:
        for файл in glob.glob(образец):
            текст = open(файл, encoding="utf-8", errors="replace").read()
            место = текст.find("неОбъявляемые")

            if место < 0:
                continue

            конец = текст.find("]);", место)
            найдено |= set(re.findall(r"\['(\w+)'",
                                      текст[место:конец if конец > 0 else None]))

    return найдено


def развернуть(текст: str) -> str:
    """Приводит собранный webpack-ом ресурс к обычному виду.

    Webpack складывает каждый модуль в `eval("…")` одной строкой, и переводы
    строк в ней записаны двумя знаками — обратной косой и `n`. Отсюда беда,
    которую видно только на живом ресурсе: перед `altServer.on(` стоит буква
    `n`, границы слова между ними нет, и поиск по `\\b` не находит ничего.

    Сверка молчала так про `playerAnimationChange`, который живой запуск
    находил в первую же минуту. А собранных webpack-ом режимов — большинство.
    """
    for знак in ("n", "r", "t"):
        текст = текст.replace("\\" + знак, " ")

    return текст


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    объявления = sys.argv[1]
    образцы = sys.argv[3:] or НАШИ[сторона(объявления)]

    # Пустой перечень означал бы «у нас нет ничего», и сверка выдала бы весь
    # список членов за нехватку. Молчать об этом нельзя.
    наши = имена(образцы)
    if not наши:
        print("не нашлось ни одного нашего файла по образцам: %s" % ", ".join(образцы))
        return 2

    api = extract(объявления)
    трогает = зовёт(sys.argv[2])
    нехватка = 0

    for класс in КЛАССЫ:
        if класс not in api:
            continue

        нет = sorted(член for член in api[класс]["members"]
                     if член not in наши and член in трогает)

        if not нет:
            continue

        нехватка += len(нет)
        print("\n%s — режим зовёт, у нас нет (%d):" % (класс, len(нет)))
        print("  " + ", ".join(нет))

    # Верхние имена — вторая половина поверхности, и до этого её не смотрели
    # вовсе.
    у_модуля = sorted(член for член in верхние(объявления)
                      if член not in наши and член in зовёт_у_модуля(sys.argv[2]))

    if у_модуля:
        нехватка += len(у_модуля)
        print("\nу самого alt — режим зовёт, у нас нет (%d):" % len(у_модуля))
        print("  " + ", ".join(у_модуля))

    # Третья поверхность: события. Считается только то, что объявляет сам
    # alt:V, — своих событий у режима вдвое больше наших, и они не в счёт.
    их_события = события(объявления)
    объявлено_немыми = немые_у_нас(образцы)
    немые = sorted(имя for имя in подписки(sys.argv[2])
                   if имя in их_события
                   and (имя in объявлено_немыми or имя not in наши))

    if немые:
        нехватка += len(немые)
        print("\nсобытия — режим слушает, мы не объявляем (%d):" % len(немые))
        print("  " + ", ".join(немые))

    print("\nсмотрели у себя в: %s" % ", ".join(образцы))

    if нехватка == 0:
        print("нехватки нет")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
