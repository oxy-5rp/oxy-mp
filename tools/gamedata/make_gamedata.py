"""Собирает один файл справочников для сервера oxyMP.

    python make_gamedata.py <каталог с .bin от alt:V> <куда положить gamedata.bin>

Читает четыре двоичных справочника alt:V и складывает из них **один** файл
своего вида. Своего, а не чужого, по двум причинам, и обе важны:

- чужие `.bin` в репозиторий не кладут, а порождённый файл раздаётся вместе с
  сервером, как `libnode.dll`;
- раскладка alt:V может смениться с его очередной сборкой, а раскладка нашего
  файла — наша, и чтение на стороне сервера от чужих перемен не зависит.

**Строки лежат общим словарём, и это не украшение.** Имена костей повторяются у
каждой модели: `SKEL_Head` встречается 1079 раз, по разу на человека. Сложенные
как есть, они занимают три четверти файла; словарём — по разу.
"""

from __future__ import annotations

import pathlib
import struct
import sys

import altv_lists


class Pool:
    """Словарь строк. Отдаёт номер, кладёт по разу."""

    def __init__(self) -> None:
        self.order: list = []
        self.seen: dict = {}

    def index(self, text: str) -> int:
        found = self.seen.get(text)
        if found is not None:
            return found

        found = len(self.order)
        self.seen[text] = found
        self.order.append(text)
        return found


class Writer:
    def __init__(self) -> None:
        self.parts: list = []

    def u8(self, value: int) -> None:
        self.parts.append(struct.pack("<B", value))

    def u16(self, value: int) -> None:
        self.parts.append(struct.pack("<H", value))

    def u32(self, value: int) -> None:
        self.parts.append(struct.pack("<I", value))

    def raw(self, data: bytes) -> None:
        self.parts.append(data)

    def bytes(self) -> bytes:
        return b"".join(self.parts)


def build(source: pathlib.Path) -> bytes:
    vehicles = altv_lists.read_vehicles((source / "vehmodels.bin").read_bytes())
    peds = altv_lists.read_peds((source / "pedmodels.bin").read_bytes())
    weapons = altv_lists.read_weapons((source / "weaponmodels.bin").read_bytes())
    kits = altv_lists.read_modkits((source / "vehmods.bin").read_bytes())

    pool = Pool()
    body = Writer()

    # Порядок разделов — часть формата: читатель идёт по файлу подряд, без
    # оглавления. Оглавление здесь и не нужно — читается он целиком и один раз,
    # при запуске сервера.
    body.u32(len(vehicles))
    for one in vehicles:
        body.u32(one.hash)
        body.u32(pool.index(one.name))
        body.u8(one.type)
        body.u8(one.wheels)
        body.u8(1 if one.armoured_windows else 0)
        body.u8(one.primary_colour)
        body.u8(one.secondary_colour)
        body.u8(one.pearl_colour)
        body.u8(one.wheel_colour)
        body.u8(one.interior_colour)
        body.u8(one.dashboard_colour)
        body.u16(one.modkit)
        body.u16(one.modkit2)
        body.u16(one.extras)
        body.u16(one.default_extras)
        body.u8(1 if one.auto_attach_trailer else 0)
        body.u8(1 if one.can_attach_cars else 0)
        body.u32(one.handling_hash)
        body.u32(pool.index(one.dlc))
        body.u16(len(one.bones))
        for bone in one.bones:
            body.u32(bone.id)
            body.u32(pool.index(bone.name))

    body.u32(len(peds))
    for one in peds:
        body.u32(one.hash)
        body.u32(pool.index(one.name))
        body.u32(pool.index(one.type))
        body.u32(pool.index(one.dlc))
        body.u32(pool.index(one.weapon))
        body.u32(pool.index(one.movement))
        body.u16(len(one.bones))
        for bone in one.bones:
            body.u16(bone.id)
            body.u16(bone.index)
            body.u32(pool.index(bone.name))

    body.u32(len(weapons))
    for one in weapons:
        body.u32(one.hash)
        body.u32(pool.index(one.name))
        body.u32(pool.index(one.model_name))
        body.u32(one.model_hash)
        body.u32(one.ammo_type_hash)
        body.u32(pool.index(one.ammo_type))
        body.u32(pool.index(one.ammo_model_name))
        body.u32(one.ammo_model_hash)
        body.u32(one.default_max_ammo)
        body.u32(one.skill_50_max_ammo)
        body.u32(one.max_skill_max_ammo)
        body.u32(one.bonus_max_ammo)
        body.u32(pool.index(one.damage_type))

    body.u32(len(kits))
    for one in kits:
        body.u16(one.id)
        body.u32(pool.index(one.name))
        body.u8(len(one.mods))
        for slot, ids in sorted(one.mods.items()):
            body.u8(slot)
            body.u8(len(ids))
            for mod in ids:
                body.u16(mod)

    # Словарь пишется впереди тела, а собирается вместе с ним: номер строки
    # нужен телу раньше, чем словарь готов.
    head = Writer()
    head.raw(b"OXGD")
    head.u16(1)
    head.u32(len(pool.order))

    for text in pool.order:
        encoded = text.encode("utf-8")
        head.u16(len(encoded))
        head.raw(encoded)

    return head.bytes() + body.bytes()


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    source = pathlib.Path(sys.argv[1])
    target = pathlib.Path(sys.argv[2])

    missing = [name for name in ("vehmodels.bin", "pedmodels.bin",
                                 "weaponmodels.bin", "vehmods.bin")
               if not (source / name).is_file()]

    if missing:
        print("не хватает справочников alt:V: " + ", ".join(missing))
        return 1

    data = build(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)

    print("%s: %d байт" % (target, len(data)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
