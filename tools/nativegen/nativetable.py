"""Порождение таблицы нативов для клиентского слоя alt:V.

Ресурс, написанный под alt:V, зовёт нативы по имени: `natives.getEntityCoords`.
Чтобы это работало, слою нужно знать про каждый натив три вещи — как он зовётся
в JavaScript, какой у него хеш в **нашей** сборке игры и какая у него подпись.

Первое берётся из имени, третье — из открытой базы. Второе сложнее: Rockstar
перетасовывает хеши между сборками, и каноническое значение в игре не найдётся.
Перевод делает та же таблица соответствий CitizenFX, что и nativegen.py.

Подпись нужна затем, что игра типов не знает — она знает ширину. Натив принимает
буфер одинаковых восьмибайтовых ячеек, и что в них класть, решает зовущий:
целое кладётся как есть, дробное — своими битами, строка — указателем, а
выходной довод — указателем на место, куда натив запишет ответ. Без подписи
отличить одно от другого нельзя.

    python nativetable.py > ../../client-js/js/alt_natives_table.js

Проверка на месте: nativegen.py --verify сверяет перевод хешей с выверенными по
живой игре значениями. Этот скрипт пользуется тем же переводом.
"""

from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from nativegen import NATIVE_DB, Translator  # noqa: E402

# Как типы открытой базы ложатся на ячейки.
#
# Ключ — тип из базы, значение — буква подписи. Буквы выбраны так, чтобы
# читались с одного взгляда: строчная — довод, заглавная — выходной довод,
# то есть указатель на место под ответ.
#
#   i  целое            I  целое 64        f  дробное
#   b  логическое       s  строка          v  вектор из трёх дробных
#   L  выходное целое   F  выходное дробное  B  выходное логическое
#   V  выходной вектор  a  что угодно (передаётся числом как есть)
SCALARS = {
    "int": "i",
    "long": "I",
    "float": "f",
    "BOOL": "b",
    "bool": "b",
    "char*": "s",
    "const char*": "s",
    "Vector3": "v",
    "void": "n",
    "Any": "a",
    "Hash": "i",
}

# Указатели на выходные значения.
POINTERS = {
    "int": "L",
    "float": "F",
    "BOOL": "B",
    "bool": "B",
    "Vector3": "V",
    "Any": "L",
}


# Имена, которыми alt:V зовёт нативы иначе, чем открытая база.
#
# Ключ — имя у alt:V, значение — имя в базе CitizenFX. Расхождений немного, и
# все они у неофициальных нативов: имя им давали разные люди в разное время.
#
# Перечислено вручную и растёт по мере встреч. Угадывать нельзя: имя ведёт к
# хешу, а неверный хеш даёт не ошибку, а вылет игры в мгновение вызова.
ALT_ALIASES = {
    # Поворот камеры, каким его видит игрок. У CitizenFX он «второй вариант».
    "getFinalRenderedCamRot": "_GET_GAMEPLAY_CAM_ROT_2",
    "getFinalRenderedCamCoord": "GET_GAMEPLAY_CAM_COORD",
    "getFinalRenderedCamFov": "GET_GAMEPLAY_CAM_FOV",

    # Луч проверки видимости. У alt:V имя длиннее и говорит о том, что вызов
    # ждёт ответа прямо в кадре, — у базы оно короче.
    "startExpensiveSynchronousShapeTestLosProbe": "START_SHAPE_TEST_LOS_PROBE",

    # Привязка рисуемого к краю экрана. У базы имя описывает, что вызов
    # открывает область, у alt:V — что он задаёт выравнивание.
    "setScriptGfxAlign": "_SCREEN_DRAW_POSITION_BEGIN",
    "resetScriptGfxAlign": "_SCREEN_DRAW_POSITION_END",
    "setScriptGfxAlignParams": "_SET_SCRIPT_GFX_ALIGN_PARAMS",
}


def resolve_alias(name: str, aliases: dict[str, str]) -> str:
    """Разворачивает псевдоним типа до его основы.

    В базе полно имён вида Ped, Vehicle, Entity — все они целые. Разворот идёт
    по цепочке: Vehicle наследует Entity, Entity объявлен целым.
    """
    seen: set[str] = set()

    while name in aliases and name not in seen:
        seen.add(name)
        name = aliases[name]

    return name


def load_type_aliases(text: str) -> dict[str, str]:
    """Имя типа в его основу, по объявлениям `type` из базы."""
    aliases: dict[str, str] = {}
    current: str | None = None

    for line in text.splitlines():
        stripped = line.strip()

        if stripped.startswith('type "'):
            current = stripped[6:-1]
        elif current is not None and stripped.startswith(("nativeType ", "extends ")):
            value = stripped.split(" ", 1)[1].strip().strip("\"'")
            aliases[current] = value
            current = None

    return aliases


def letter_for(kind: str, aliases: dict[str, str]) -> str | None:
    """Буква подписи для типа. None — тип неизвестен, натив пропускается."""
    kind = kind.strip()

    # Указатель записывают двумя способами: звёздочкой у довода (`float* "out"`)
    # и суффиксом Ptr у результата (`returns "charPtr"`). Смысл один.
    pointer = kind.endswith("*")

    if kind.endswith("Ptr"):
        pointer = True
        kind = kind[:-3]

    base = resolve_alias(kind.rstrip("*").strip(), aliases)

    if not pointer:
        letter = SCALARS.get(base)
        if letter is not None:
            return letter

        # Необъявленный тип с заглавной буквы — дескриптор игры: Ped, Object,
        # Cam, Blip, Pickup, ScrHandle. Все они целые, и объявлять их в базе
        # никто не стал. Считать их целыми — не догадка: игра хранит дескриптор
        # именно так, и alt:V поступает точно же.
        return "i" if base[:1].isupper() else None

    # Строка — единственный указатель, который отдают, а не принимают.
    if base == "char":
        return "s"

    letter = POINTERS.get(base)
    if letter is not None:
        return letter

    return "L" if base[:1].isupper() else None


def camel_case(native: str) -> str:
    """Имя натива, каким его зовут из JavaScript.

    Так же поступает и alt:V: SHOUTY_CASE превращается в camelCase. Ведущее
    подчёркивание у неофициальных имён сохраняется — иначе `_GET_X` и `GET_X`
    слились бы в одно.
    """
    leading = "_" if native.startswith("_") else ""
    parts = [part for part in native.strip("_").split("_") if part]

    if not parts:
        return leading

    head = parts[0].lower()
    tail = "".join(part.capitalize() for part in parts[1:])

    return leading + head + tail


def load_natives(text: str) -> list[tuple[str, int, list[str], str]]:
    """Имя, канонический хеш, типы доводов и тип результата."""
    natives: list[tuple[str, int, list[str], str]] = []

    name: str | None = None
    canonical: int | None = None
    arguments: list[str] = []
    returns = "void"
    inside_arguments = False

    # Довод записан как «Тип "имя",»: тип, пробелы, имя в кавычках. Указатель
    # помечается звёздочкой у типа — она может стоять и слитно, и отдельно.
    argument = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*\s*\**)\s*"')

    # Тип результата — одно слово, а не предложение.
    #
    # Проверка нужна из-за описаний: внутри блока `doc [[! ... ]]` полно строк,
    # начинающихся со слова «returns» и продолжающихся человеческим текстом
    # («returns the players ped used in many functions»). Без разбора описаний
    # такая строка выглядит объявлением типа с именем в двадцать слов.
    plain_type = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*\**$")

    inside_doc = False

    def flush() -> None:
        if name is not None and canonical is not None:
            natives.append((name, canonical, list(arguments), returns))

    for line in text.splitlines():
        stripped = line.strip()

        # Блок описания пропускается целиком: в нём человеческий текст, и
        # разбирать его как объявления нельзя.
        if inside_doc:
            if "]]" in stripped:
                inside_doc = False
            continue

        if stripped.startswith("doc [[") or stripped.startswith("--"):
            inside_doc = stripped.startswith("doc [[") and "]]" not in stripped
            continue

        if stripped.startswith('native "'):
            flush()
            name = stripped[8:-1]
            canonical = None
            arguments = []
            returns = "void"
            inside_arguments = False
            continue

        if name is None:
            continue

        if stripped.startswith('hash "'):
            canonical = int(stripped[6:-1], 16)
        elif stripped.startswith("arguments {"):
            inside_arguments = True
        elif inside_arguments and stripped.startswith("}"):
            inside_arguments = False
        elif inside_arguments:
            match = argument.match(stripped)
            if match:
                arguments.append(match.group(1).replace(" ", ""))
        elif stripped.startswith("returns"):
            candidate = stripped.split(None, 1)[1].strip().strip("\"'")

            if plain_type.match(candidate):
                returns = candidate

    flush()
    return natives


def main() -> int:
    text = NATIVE_DB.read_text(encoding="utf-8", errors="replace")

    aliases = load_type_aliases(text)
    translator = Translator()

    entries: dict[str, str] = {}

    # Имена без ведущего подчёркивания — те, что добавит alt:V. Кладутся в
    # отдельный словарь и вливаются в конце: официальный натив с тем же именем
    # обязан победить, а встретиться он может и позже.
    stripped: dict[str, str] = {}

    skipped_unmapped = 0
    skipped_types = 0

    for native, canonical, argument_types, return_type in load_natives(text):
        build = translator.mapping.get(canonical)

        # Натива нет в таблице соответствий — значит в нашей сборке его нет
        # вовсе. Выдумывать ему хеш нельзя: неверный обернулся бы вылетом игры в
        # мгновение вызова, а не строкой в журнале.
        if build is None:
            skipped_unmapped += 1
            continue

        letters = []
        unknown = False

        for kind in argument_types:
            letter = letter_for(kind, aliases)
            if letter is None:
                unknown = True
                break
            letters.append(letter)

        result = letter_for(return_type, aliases) if return_type else "n"

        if unknown or result is None:
            skipped_types += 1
            continue

        signature = f"{build:X}|{''.join(letters)}:{result}"
        name = camel_case(native)

        entries[name] = signature

        # Неофициальные нативы кладутся и под именем без подчёркивания.
        #
        # Так их зовёт alt:V: `_GET_ASPECT_RATIO` у него `getAspectRatio`, и
        # режимы написаны под это. Оба имени, а не одно взамен другого: под
        # подчёркиванием их зовут тоже, и отнять его значило бы сломать половину
        # уже написанного.
        #
        # Занятое имя не перебивается: если официальный натив с таким именем
        # есть, оно принадлежит ему. Порядок обхода базы при этом значения не
        # имеет — проверка идёт по готовому словарю в конце.
        if name.startswith('_'):
            stripped.setdefault(name[1:], signature)

    print("// Таблица нативов игры: имя, хеш нашей сборки и подпись.")
    print("//")
    print("// Порождено tools/nativegen/nativetable.py из открытой базы имён и")
    print("// таблицы соответствий CitizenFX. Правится не здесь, а там: игра")
    print("// обновится — таблицу нужно будет пересобрать заново.")
    print("//")
    print("// Хеши здесь **нашей сборки**, а не канонические. Rockstar перетасовывает")
    print("// их между сборками, и канонический хеш в игре не найдётся: из шести с")
    print("// лишним тысяч уцелело шестьдесят шесть. Ошибка здесь не даёт ни строки в")
    print("// журнале — она даёт вылет игры в мгновение вызова.")
    print("//")
    print("// Подпись: буквы доводов, двоеточие, буква результата.")
    print("//   i целое   I целое 64   f дробное   b логическое   s строка")
    print("//   v вектор  a что угодно  n ничего")
    print("//   L F B V — выходные доводы: указатель на место под ответ")
    print("")
    for name, signature in stripped.items():
        entries.setdefault(name, signature)

    # Имена alt:V, расходящиеся с базой. Ставятся поверх: своё имя у alt:V
    # главнее, ресурсы написаны под него.
    by_native = {}
    for native, canonical, _, _ in load_natives(text):
        by_native[native] = canonical

    for alt_name, native_name in ALT_ALIASES.items():
        canonical = by_native.get(native_name)
        build = translator.mapping.get(canonical) if canonical is not None else None

        if build is None:
            print(f"// псевдоним {alt_name}: натива {native_name} нет", file=sys.stderr)
            continue

        # Подпись берётся у того же натива — она у них одна.
        source = camel_case(native_name)
        if source in entries:
            entries[alt_name] = entries[source]

    print("globalThis.__oxympAlt.nativeTable = {")

    for name in sorted(entries):
        print(f"    {name}: '{entries[name]}',")

    print("};")

    print(f"// нативов: {len(entries)}; нет в нашей сборке: {skipped_unmapped}; "
          f"с непонятой подписью: {skipped_types}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
