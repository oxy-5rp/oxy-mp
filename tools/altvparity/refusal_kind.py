"""Сверка: вопрос ли отказывает вслух и распоряжение ли шепчет.

    python tools/altvparity/refusal_kind.py <путь к index.d.ts> <файл слоя> [ещё]

Правило слоя alt:V делит отказы по роду, а не по вкусу: **вопрос** без ответа
бросает (`absent`), **распоряжение** говорит о себе раз в журнал и возвращается
(`unperformed`). Перепутанные они дают две разные беды, и обе молчат.

Распоряжение, которое бросает, уносит с собой весь чужой обработчик: вызов стоит
посреди него, и всё, что шло следом, не исполняется. Ровно так `player.model`
однажды унёс вход игрока целиком.

Вопрос, который шепчет, отдаёт `undefined` — и принявший его за ответ понесёт эту
ложь дальше.

Род берётся у alt:V: объявленный `void` — распоряжение, всякий другой — вопрос.
Свойство (`get`) — вопрос всегда, чем бы его ни объявляли.
"""

from __future__ import annotations

import io
import pathlib
import re
import sys


def объявления(path: pathlib.Path) -> dict[str, list[str]]:
    """Имя члена → типы ответа во всех его объявлениях."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)

    найдено: dict[str, list[str]] = {}

    # Метод класса или объявление функции: имя, доводы, тип ответа.
    for член in re.finditer(r"(?:^|\n)\s*(?:public |static |readonly )*"
                            r"(\w+)\s*\([^()]*\)\s*:\s*([^;{\n]+)", text):
        найдено.setdefault(член.group(1), []).append(член.group(2).strip())

    return найдено


def отказы(path: pathlib.Path) -> list[tuple[int, str, str, bool]]:
    """Строка, что отказано, полное имя, правда ли это `absent`."""
    строки = io.open(path, encoding="utf-8", errors="replace").read().splitlines()
    найдено = []

    for номер, строка in enumerate(строки, 1):
        for вид, кричит in (("absent", True), ("unperformed", False), ("warnOnce", False)):
            место = re.search(r"\b%s\('([^']+)'" % вид, строка)
            if место is None:
                continue

            if отвечает(строки, номер, вид):
                break

            полное = место.group(1)
            свойство = "get:" in строка.replace(" ", "") or "get " in строка
            найдено.append((номер, полное.rsplit(".", 1)[-1], полное, кричит and not свойство))
            break

    return найдено

def отвечает(строки: list[str], номер: int, вид: str) -> bool:
    """Отдаёт ли этот отказ значение, а не одну жалобу.

    Отвечающее в счёт не идёт, и таких два рода. `warnOnce`, за которым стоит
    настоящий `return`, — это не отказ, а оговорка при неполном ответе
    (`vehicle.getDamageStatusBase64`). `unperformed` с третьим доводом
    отвечает им же — обычно `false`, «не получилось».
    """
    if вид == "unperformed":
        # Третий довод — то, чем ответить. Он может уехать на строку ниже.
        хвост = " ".join(строки[номер - 1:номер + 2])
        внутри = хвост[хвост.index("unperformed("):]
        return внутри.count(",") >= 2

    if вид != "warnOnce":
        return False

    начало = строки[номер - 1]
    отступ = len(начало) - len(начало.lstrip())

    for строка in строки[номер:номер + 40]:
        голая = строка.strip()
        if not голая:
            continue

        if len(строка) - len(строка.lstrip()) < отступ:
            return False

        if re.match(r"return\s+\S", голая):
            return True

    return False


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    их = объявления(pathlib.Path(sys.argv[1]))
    находки = 0

    for файл in sys.argv[2:]:
        путь = pathlib.Path(файл)
        print("==== %s ====" % путь.name)

        for номер, имя, полное, кричит in отказы(путь):
            ответы = их.get(имя)
            if ответы is None:
                continue

            # Хотя бы одно объявление с ответом — значит спрашивать его можно, и
            # молчать в ответ нельзя. Перегрузки у alt:V обычны.
            вопрос = any(ответ != "void" for ответ in ответы)

            if вопрос == кричит:
                continue

            находки += 1
            print("  %s:%d %s — у alt:V %s, а у нас %s"
                  % (путь.name, номер, полное, " | ".join(sorted(set(ответы))),
                     "кричит" if кричит else "шепчет"))

    if находки == 0:
        print("расхождений нет")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
