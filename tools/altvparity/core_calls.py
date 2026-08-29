"""Сверка: всякое ли имя ядра, которое зовёт слой, у ядра есть.

    python tools/altvparity/core_calls.py <bindings.cpp> <файл слоя> [ещё]

Слой alt:V разговаривает с ядром через один объект (`alt.native` в
`alt_server.js`, он же `native` в клиентском слое). Имена в нём заводит C++
строкой — `addFunction(context, oxymp, "playSpeech", playSpeech)`, — и опечатка
или чужое имя не даёт ни ошибки сборки, ни строки в журнале до самого вызова.

А вызов этот стоит посреди чужого обработчика, и `ReferenceError` уносит с собой
всё, что шло следом. Ровно так `__oxymp.clearBlood` вместо `native.clearBlood`
однажды унёс весь `playerConnect` живого режима — при зелёном наборе.

Это машинное продолжение правила из CLAUDE.md: **заводя дорогу от слоя к ядру,
проверяйте, что по ней кто-то ходит.**
"""

from __future__ import annotations

import io
import pathlib
import re
import sys


def ядро(path: pathlib.Path) -> set[str]:
    text = io.open(path, encoding="utf-8", errors="replace").read()

    имена = set(re.findall(r'addFunction\([^,]+,\s*\w+,\s*"(\w+)"', text))
    имена |= set(re.findall(r'->Set\([^,]+,\s*toJs\(isolate,\s*"(\w+)"\)', text))

    return имена


def зовут(path: pathlib.Path) -> list[tuple[int, str, str]]:
    """Строка, объект и имя у каждого обращения к ядру."""
    строки = io.open(path, encoding="utf-8", errors="replace").read().splitlines()
    найдено = []

    for номер, строка in enumerate(строки, 1):
        for место in re.finditer(r"\b(native|__oxymp)\.(\w+)\s*\(", строка):
            найдено.append((номер, место.group(1), место.group(2)))

    return найдено


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    есть = ядро(pathlib.Path(sys.argv[1]))
    находки = 0

    for файл in sys.argv[2:]:
        путь = pathlib.Path(файл)
        print("==== %s ====" % путь.name)

        for номер, объект, имя in зовут(путь):
            if объект == "native" and имя in есть:
                continue

            находки += 1
            print("  %s:%d %s.%s — у ядра такого имени нет"
                  % (путь.name, номер, объект, имя))

    if находки == 0:
        print("мёртвых дорог нет")

    return 1 if находки else 0


if __name__ == "__main__":
    raise SystemExit(main())
