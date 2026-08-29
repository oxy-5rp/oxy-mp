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

Псевдонимы базы эту нехватку не закрывают, и это проверено, а не предположено.
В `gta_universal.lua` 657 строк `alias`, и соблазн велик: вдруг alt:V зовёт
нативы их вторыми именами. Из трёх с половиной тысяч недостающих имён
псевдонимами закрывается **ноль**. Прибор при этом проверен подсадкой:
настоящий псевдоним из базы сверка находит.

То есть имена эти у CitizenFX отсутствуют вовсе, а не лежат под другим
написанием. Закрыть их можно только клиентским двоичным файлом alt:V или более
полной базой; повторять опыт с псевдонимами не нужно.
"""

from __future__ import annotations

import io
import pathlib
import re
import sys


def theirs(path: pathlib.Path) -> dict[str, bool]:
    """Имя натива → правда ли, что он ничего не возвращает.

    Тип ответа здесь не украшение: от него зависит, как отказывать. Натив,
    объявленный `void`, — распоряжение, и отказ ему шепчется в журнал; всякий
    другой — вопрос, и молчаливый ответ на вопрос был бы ложью.
    """
    text = io.open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)

    объявление = re.compile(r"export function (\w+)\((.*?)\):\s*([^;]+);", re.S)

    return {found.group(1): found.group(3).strip() == "void"
            for found in объявление.finditer(text)}


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

    объявлено = theirs(pathlib.Path(sys.argv[1]))
    наши = ours(pathlib.Path("client-js/js/alt_natives_table.js"))

    нехватка = sorted(set(объявлено) - наши)
    молчащие = sorted(name for name in нехватка if объявлено[name])

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
    print("//")
    print("// Списка два, и делятся они по тому же правилу, что и весь слой alt:V:")
    print("// натив, объявленный `void`, — распоряжение, и отказ ему шепчется в")
    print("// журнал один раз; всякий другой — вопрос, и на вопрос отказывают вслух.")
    print("")
    print("'use strict';")
    print("")
    print("(function build(alt) {")
    print("    alt.nativesAbsent = new Set([")

    for name in нехватка:
        print("        '%s'," % name)

    print("    ]);")
    print("")
    print("    // Из них те, что ничего не возвращают: распоряжения.")
    print("    alt.nativesAbsentQuiet = new Set([")

    for name in молчащие:
        print("        '%s'," % name)

    print("    ]);")
    print("})(globalThis.__oxympAlt);")

    print("// имён: %d, из них распоряжений: %d" % (len(нехватка), len(молчащие)),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
