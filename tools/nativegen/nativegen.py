"""Перевод имён нативов в хеши целевой сборки игры.

Rockstar перетасовывает хеши нативов между сборками: каноническое значение из
открытой базы в игре не найдётся. Соответствие лежит в таблице
`CrossMapping_Universal.h` проекта CitizenFX — строка на натив, столбец на
поколение сборок; нулевой столбец канонический, последний общий для всех сборок
от 2944 и новее, то есть и для нашей.

Скрипт делает ровно два действия и ничего сверх них: находит по имени натива его
канонический хеш в открытой базе и переводит этот хеш в значение целевой сборки.
Проверить, что натив с таким хешем в игре действительно зарегистрирован, отсюда
нельзя — это делает сверка по живой игре, описанная в README.

Доказательство правильности столбца — режим `--verify`: он берёт хеши, уже
лежащие в native_hashes.hpp и выверенные по живой игре, и требует, чтобы перевод
воспроизвёл каждый из них.

    python nativegen.py --verify
    python nativegen.py GET_VEHICLE_COLOURS SET_VEHICLE_COLOURS
    python nativegen.py --list natives.txt
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

# Пути к исходным данным. Ссылка на зеркало CitizenFX, а не копия внутри
# проекта: таблица весит мегабайты и обновляется вместе с игрой, а не с нами.
REFERENCE = pathlib.Path(r"D:\gtav-engine-reference")
NATIVE_DB = REFERENCE / "ext" / "natives" / "natives_stash" / "gta_universal.lua"
CROSS_MAP = (REFERENCE / "code" / "components" / "rage-scripting-five" / "include"
             / "CrossMapping_Universal.h")

PROJECT = pathlib.Path(__file__).resolve().parents[2]
HASHES_HEADER = PROJECT / "client" / "src" / "game" / "native_hashes.hpp"

# Столбец таблицы соответствий, отвечающий сборкам 2944 и новее.
TARGET_COLUMN = 27


def load_native_database() -> dict[str, int]:
    """Имя натива в канонический хеш, по открытой базе имён."""
    names: dict[str, int] = {}
    current: str | None = None

    for line in NATIVE_DB.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()

        if stripped.startswith('native "'):
            current = stripped[8:-1]
        elif current is not None and stripped.startswith('hash "'):
            names[current] = int(stripped[6:-1], 16)
            current = None

    return names


def load_cross_mapping() -> dict[int, int]:
    """Канонический хеш в хеш целевой сборки."""
    mapping: dict[int, int] = {}
    row = re.compile(r"\{\s*\{\s*(0x[0-9A-Fa-f]+(?:\s*,\s*0x[0-9A-Fa-f]+)*)\s*\}\s*\}")

    for match in row.finditer(CROSS_MAP.read_text(encoding="utf-8", errors="replace")):
        columns = [int(value, 16) for value in match.group(1).split(",")]

        if len(columns) <= TARGET_COLUMN:
            continue

        mapping[columns[0]] = columns[TARGET_COLUMN]

    return mapping


class Translator:
    def __init__(self) -> None:
        self.canonical = load_native_database()
        self.mapping = load_cross_mapping()

    def translate(self, native: str) -> tuple[int, int]:
        """Канонический хеш и хеш целевой сборки. Бросает, если натива нет."""
        if native not in self.canonical:
            raise KeyError(f"{native}: нет в открытой базе имён")

        canonical = self.canonical[native]

        if canonical not in self.mapping:
            raise KeyError(f"{native}: нет в таблице соответствий")

        return canonical, self.mapping[canonical]


def constant_name(native: str) -> str:
    """Имя константы, каким его принято писать в native_hashes.hpp."""
    return "k" + "".join(part.capitalize() for part in native.split("_"))


def emit(translator: Translator, natives: list[str]) -> int:
    """Печатает готовые записи для native_hashes.hpp."""
    failed = 0

    for native in natives:
        if not native or native.startswith("#"):
            print(native)
            continue

        try:
            canonical, build = translator.translate(native)
        except KeyError as error:
            print(f"// НЕ НАЙДЕНО: {error}", file=sys.stderr)
            failed += 1
            continue

        print(f"/// {native}, канонический хеш 0X{canonical:016X}.")
        print(f"inline constexpr std::uint64_t {constant_name(native)} = 0X{build:016X};")
        print()

    return failed


def verify(translator: Translator) -> int:
    """Сверяет перевод с уже выверенными по живой игре значениями."""
    source = HASHES_HEADER.read_text(encoding="utf-8")

    entry = re.compile(
        r"/// (?P<native>[A-Z0-9_]+), канонический хеш (?P<canonical>0X[0-9A-F]+)\."
        r"(?:\n///[^\n]*)*"
        r"\ninline constexpr std::uint64_t k\w+ = (?P<build>0X[0-9A-F]+);"
    )

    checked = 0
    mismatched = 0

    for match in entry.finditer(source):
        native = match["native"]
        expected_canonical = int(match["canonical"], 16)
        expected_build = int(match["build"], 16)

        try:
            canonical, build = translator.translate(native)
        except KeyError as error:
            print(f"{error}", file=sys.stderr)
            mismatched += 1
            continue

        checked += 1

        if canonical != expected_canonical:
            print(f"{native}: канонический 0X{canonical:016X}, "
                  f"в заголовке 0X{expected_canonical:016X}", file=sys.stderr)
            mismatched += 1
        elif build != expected_build:
            print(f"{native}: сборка 0X{build:016X}, "
                  f"в заголовке 0X{expected_build:016X}", file=sys.stderr)
            mismatched += 1

    print(f"сверено {checked}, расхождений {mismatched}")
    return mismatched


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("natives", nargs="*", help="имена нативов")
    parser.add_argument("--list", type=pathlib.Path,
                        help="файл со списком имён, по одному на строку")
    parser.add_argument("--verify", action="store_true",
                        help="сверить перевод с native_hashes.hpp")

    # Вывод пойдёт в исходный файл на C++, а он в UTF-8. Кодировка консоли
    # Windows здесь не годится: русские комментарии в ней превращаются в мусор,
    # который компилятор прочитает как что угодно.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

    arguments = parser.parse_args()
    translator = Translator()

    if arguments.verify:
        return 1 if verify(translator) else 0

    natives = list(arguments.natives)

    if arguments.list is not None:
        natives += arguments.list.read_text(encoding="utf-8").splitlines()

    if not natives:
        parser.error("нечего переводить")

    return 1 if emit(translator, natives) else 0


if __name__ == "__main__":
    sys.exit(main())
