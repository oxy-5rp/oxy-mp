"""Сверка нашей таблицы нативов с объявлениями alt:V.

    python tools/altvparity/native_arity.py <путь к @altv/types-natives/index.d.ts>

Таблица `client-js/js/alt_natives_table.js` хранит на каждый натив хеш и
раскладку доводов: `'25DD447A6EB3A86F|ff:n'` — два дробных на вход, ничего на
выход. Разойдись эта раскладка с настоящей — натив получит на довод меньше, и
недостающий он прочтёт из неподготовленной ячейки.

**Ошибка эта молчит целиком.** Исключения нет, в журнале ничего, а натив просто
делает не то: `END_TEXT_COMMAND_DISPLAY_TEXT` с двумя доводами вместо трёх не
рисует текста вовсе — ни ошибки, ни надписи.

Считается только число доводов, а не их род: род у нас записан подробнее, чем в
объявлениях alt:V (там всё `number`), и сверять его не по чему. Число же —
необходимое условие, и его достаточно, чтобы поймать пропущенный довод.

Возвращаемое значение в число доводов не входит: в нашей раскладке оно после
двоеточия. Выходные доводы (`L`, `F`, `B`, `V`) входят — натив получает на них
указатель, и место в списке они занимают.
"""

from __future__ import annotations

import io
import pathlib
import re
import sys


def theirs(path: pathlib.Path) -> dict:
    """Число доводов у каждого натива по объявлениям alt:V."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)

    out = {}

    for found in re.finditer(r"export function (\w+)\(([^)]*)\)", text):
        name = found.group(1)
        body = found.group(2).strip()

        if not body:
            out[name] = 0
            continue

        # Доводы разделены запятыми верхнего уровня. Внутри бывают объединения
        # видов (`string | null`) — запятых в них нет, так что деления довольно.
        out[name] = len([one for one in body.split(",") if one.strip()])

    return out


def ours(path: pathlib.Path) -> dict:
    """Число доводов у каждого натива по нашей таблице."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    out = {}

    for found in re.finditer(r"^\s*(\w+):\s*'([0-9A-Fa-f]+)\|([^']*)'", text, re.M):
        name = found.group(1)
        shape = found.group(3)

        takes = shape.split(":")[0]
        out[name] = (len(takes), found.group(2))

    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    их = theirs(pathlib.Path(sys.argv[1]))
    наши = ours(pathlib.Path("client-js/js/alt_natives_table.js"))

    расхождений = 0
    неизвестных = 0

    for name, (сколько, хеш) in sorted(наши.items()):
        # Имена с подчёркиванием впереди — те, что alt:V объявляет без него: у
        # него это пометка «имя неофициальное», и в объявлениях оно то с ней, то
        # без.
        у_них = их.get(name, их.get(name.lstrip("_")))

        if у_них is None:
            неизвестных += 1
            continue

        if у_них != сколько:
            расхождений += 1
            print("  %-44s наш %d, alt:V %d   (%s)" % (name, сколько, у_них, хеш))

    print("\nсверено %d, расхождений %d, не найдено в объявлениях %d"
          % (len(наши), расхождений, неизвестных))

    return 1 if расхождений else 0


if __name__ == "__main__":
    raise SystemExit(main())
