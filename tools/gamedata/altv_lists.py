"""Разбор четырёх двоичных справочников alt:V.

Рядом с сервером alt:V держит `vehmodels.bin`, `pedmodels.bin`,
`weaponmodels.bin` и `vehmods.bin`. Из них он отвечает на
`getVehicleModelInfoByHash`, `getPedModelInfoByHash`,
`getWeaponModelInfoByHash` и на `vehicle.getModsCount` — то есть на всё, что
про модели знает не игра, а сервер.

Формат восстановлен по самим файлам и сверен на них же: каждый из четырёх
дочитывается ровно до последнего байта, а не «примерно до конца». Это и есть
проверка правильности — раскладка, ошибшаяся хоть на байт, разъезжается и до
конца не доходит.

Разбор здесь точнее двух ходивших по сети: у одного пропущено поле в оружии —
имя модели патрона, — отчего всё, что за ним, читается со сдвигом; у другого
`vehmods.bin` не разобран вовсе, а найден поиском строки «modkit» в потоке, и
теряет девять наборов из десяти. Здесь оружий 182 из 182, наборов 571 из 571.

Ничего чужого этот файл не содержит и не копирует: он только описывает
раскладку. Сами `.bin` берутся у того, у кого они есть.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field


def joaat(text: str) -> int:
    """Хеш имени, как его считает игра. Строчными — так же считает и alt:V."""
    value = 0
    for symbol in text.lower():
        value = (value + ord(symbol)) & 0xFFFFFFFF
        value = (value + (value << 10)) & 0xFFFFFFFF
        value ^= value >> 6
    value = (value + (value << 3)) & 0xFFFFFFFF
    value ^= value >> 11
    return (value + (value << 15)) & 0xFFFFFFFF


class Reader:
    """Чтение по порядку.

    Своё, а не struct.unpack на каждый вызов: раскладка здесь сплошная, без
    выравнивания и без смещений, и место чтения — это состояние, а не довод.
    """

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.at = 0

    def u8(self) -> int:
        value = self.data[self.at]
        self.at += 1
        return value

    def u16(self) -> int:
        value = struct.unpack_from("<H", self.data, self.at)[0]
        self.at += 2
        return value

    def u32(self) -> int:
        value = struct.unpack_from("<I", self.data, self.at)[0]
        self.at += 4
        return value

    def text8(self) -> str:
        length = self.u8()
        value = self.data[self.at:self.at + length].decode("utf-8", "replace")
        self.at += length
        return value

    def text16(self) -> str:
        length = self.u16()
        value = self.data[self.at:self.at + length].decode("utf-8", "replace")
        self.at += length
        return value

    @property
    def done(self) -> bool:
        return self.at >= len(self.data)


@dataclass
class Bone:
    id: int
    index: int
    name: str


@dataclass
class Vehicle:
    hash: int
    name: str

    # Девять байт подряд, и они ложатся на девять полей `IVehicleModel` один в
    # один — это и есть доказательство, что байты опознаны верно.
    type: int
    wheels: int
    armoured_windows: bool
    primary_colour: int
    secondary_colour: int
    pearl_colour: int
    wheel_colour: int
    interior_colour: int
    dashboard_colour: int

    # Номер набора тюнинга в `vehmods.bin`; 0xFFFF — набора нет.
    #
    # Опознан сверкой имён: sultan даёт 45 — `39_sultan_modkit`, zentorno 90 —
    # `89_zentorno_modkit`, banshee 51 — `46_banshee_modkit`. Совпадением это
    # быть не может.
    modkit: int
    modkit2: int

    # Битовые маски дополнений кузова: какие есть и какие стоят по умолчанию.
    extras: int
    default_extras: int

    # Опознаны списками, а не догадкой. Прицеп сам цепляют сорок машин, и среди
    # них phantom, packer, bison, bobcatxl — то есть тягачи и пикапы с фаркопом.
    # Машину цепляют тринадцать: cargobob, towtruck и родня — то есть эвакуаторы
    # и вертолёт с крюком.
    auto_attach_trailer: bool
    can_attach_cars: bool

    handling_hash: int
    dlc: str
    dlc_hashes: list = field(default_factory=list)
    bones: list = field(default_factory=list)


@dataclass
class Ped:
    hash: int
    name: str
    type: str
    dlc: str
    weapon: str
    movement: str
    bones: list = field(default_factory=list)


@dataclass
class Weapon:
    hash: int
    name: str
    model_name: str
    model_hash: int
    ammo_type_hash: int
    ammo_type: str
    ammo_model_name: str
    ammo_model_hash: int
    default_max_ammo: int
    skill_50_max_ammo: int
    max_skill_max_ammo: int
    bonus_max_ammo: int
    damage_type: str
    dlc: str


@dataclass
class ModKit:
    id: int
    name: str
    # Место тюнинга -> какие детали в нём есть.
    mods: dict = field(default_factory=dict)


def read_vehicles(data: bytes) -> list:
    assert data[:2] == b"VE", "не vehmodels.bin"

    reader = Reader(data)
    reader.at = 4

    out = []

    while not reader.done:
        model_hash = reader.u32()
        name = reader.text8()
        info = [reader.u8() for _ in range(9)]
        kits = reader.u32()
        extras = reader.u16()
        default_extras = reader.u16()
        auto_attach = reader.u8() != 0

        bones = []
        for index in range(reader.u8()):
            bone_id = reader.u32()
            bones.append(Bone(bone_id, index, reader.text8()))

        can_attach = reader.u8() != 0
        handling = reader.u32()
        dlc = reader.text8()
        dlc_hashes = [reader.u32() for _ in range(reader.u8())]

        out.append(Vehicle(
            hash=model_hash, name=name,
            type=info[0], wheels=info[1], armoured_windows=info[2] != 0,
            primary_colour=info[3], secondary_colour=info[4], pearl_colour=info[5],
            wheel_colour=info[6], interior_colour=info[7], dashboard_colour=info[8],
            modkit=kits & 0xFFFF, modkit2=(kits >> 16) & 0xFFFF,
            extras=extras, default_extras=default_extras,
            auto_attach_trailer=auto_attach, can_attach_cars=can_attach,
            handling_hash=handling, dlc=dlc, dlc_hashes=dlc_hashes, bones=bones))

    return out


def read_peds(data: bytes) -> list:
    assert data[:2] == b"PE", "не pedmodels.bin"

    reader = Reader(data)
    reader.at = 4

    out = []

    while not reader.done:
        model_hash = reader.u32()
        name = reader.text8()
        kind = reader.text8()
        dlc = reader.text8()
        weapon = reader.text8()
        movement = reader.text8()

        bones = []
        for _ in range(reader.u16()):
            bone_id = reader.u16()
            index = reader.u16()
            bones.append(Bone(bone_id, index, reader.text8()))

        out.append(Ped(model_hash, name, kind, dlc, weapon, movement, bones))

    return out


def read_weapons(data: bytes) -> list:
    assert data[:2] == b"WE", "не weaponmodels.bin"

    reader = Reader(data)
    reader.at = 4

    out = []

    while not reader.done:
        out.append(Weapon(
            hash=reader.u32(),
            name=reader.text8(),
            model_name=reader.text8(),
            model_hash=reader.u32(),
            ammo_type_hash=reader.u32(),
            ammo_type=reader.text8(),
            # Вот этого поля нет в разборе, ходившем по сети, и без него всё
            # дальнейшее читается со сдвигом.
            ammo_model_name=reader.text8(),
            ammo_model_hash=reader.u32(),
            default_max_ammo=reader.u32(),
            skill_50_max_ammo=reader.u32(),
            max_skill_max_ammo=reader.u32(),
            bonus_max_ammo=reader.u32(),
            damage_type=reader.text8(),
            dlc=reader.text8()))

    return out


def read_modkits(data: bytes) -> list:
    assert data[:2] == b"MO", "не vehmods.bin"

    reader = Reader(data)
    reader.at = 4

    out = []

    while not reader.done:
        kit = ModKit(reader.u16(), reader.text16())

        # Число мест байтом сразу за именем — этого в чужом разборе нет вовсе,
        # оттого он и не мог найти, где кончается набор, и искал следующий
        # поиском строки «modkit» в потоке.
        for _ in range(reader.u8()):
            slot = reader.u8()
            kit.mods[slot] = [reader.u16() for _ in range(reader.u8())]

        out.append(kit)

    return out
