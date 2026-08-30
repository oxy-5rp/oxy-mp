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
    "client": ["client-js/js/alt_*.js", "client-js/src/*.cpp"],
}

# Что сверяется. Порядок для вывода, а не для дела.
КЛАССЫ = ["Player", "Vehicle", "Entity", "Blip", "Colshape",
          "Checkpoint", "Ped", "Object", "Marker"]


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
                текст = open(os.path.join(каталог, имя), encoding="utf-8",
                             errors="replace").read()
            except OSError:
                continue

            найдено |= set(re.findall(r"\.([A-Za-z_][A-Za-z0-9_]*)", текст))

    return найдено


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

    print("\nсмотрели у себя в: %s" % ", ".join(образцы))

    if нехватка == 0:
        print("нехватки нет")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
