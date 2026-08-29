"""Список нативов, которые объявляет alt:V, а у нас их нет.

    python tools/nativegen/missingnatives.py <путь к @altv/types-natives/index.d.ts> \
        > client-js/js/alt_natives_absent.js

Зачем он нужен. Наша таблица порождена из открытой базы имён CitizenFX, а в ней
5180 нативов; alt:V объявляет около восьми с половиной тысяч. Разницы — три с
половиной тысячи имён, и у alt:V они **есть**: режим, написанный под него, их не
проверяет и звать будет прямо.

Без этого списка такой вызов падал с «native.X is not a function» — и по этой
строке нельзя понять, опечатка это у режима или нехватка у нас. Со списком он
падает с названной причиной.

Хеши сюда не идут и идти не могут: их нет ни в открытой базе, ни в объявлениях
alt:V. Выдумывать нельзя — неверный хеш роняет игру в мгновение вызова.
"""

from __future__ import annotations

import io
import pathlib
import re
import sys


def theirs(path: pathlib.Path) -> set[str]:
    text = io.open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)

    return {found.group(1) for found in re.finditer(r"export function (\w+)\(", text)}


def ours(path: pathlib.Path) -> set[str]:
    text = io.open(path, encoding="utf-8", errors="replace").read()
    names = {found.group(1) for found in re.finditer(r"^\s*(\w+):\s*'[0-9A-Fa-f]+\|", text, re.M)}

    # Подчёркивание у alt:V означает «имя неофициальное», и в объявлениях оно то
    # стоит, то нет. Считаем своими оба написания.
    return names | {name.lstrip("_") for name in names} | {"_" + name for name in names}


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    нехватка = sorted(theirs(pathlib.Path(sys.argv[1]))
                      - ours(pathlib.Path("client-js/js/alt_natives_table.js")))

    print("// Нативы, которые объявляет alt:V, а у нас их нет.")
    print("//")
    print("// Порождено tools/nativegen/missingnatives.py. Правится не здесь.")
    print("//")
    print("// Нужен этот список ради одной строки в журнале: без него вызов такого")
    print("// натива падает с «is not a function», и по этой строке не понять, чья")
    print("// вина — опечатка режима или наша нехватка. С ним причина названа.")
    print("//")
    print("// Хешей здесь нет и быть не может: их нет ни в открытой базе имён, ни в")
    print("// объявлениях alt:V, а выдумывать нельзя — неверный роняет игру.")
    print("")
    print("'use strict';")
    print("")
    print("(function build(alt) {")
    print("    alt.nativesAbsent = new Set([")

    for name in нехватка:
        print("        '%s'," % name)

    print("    ]);")
    print("})(globalThis.__oxympAlt);")

    print("// имён: %d" % len(нехватка), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
