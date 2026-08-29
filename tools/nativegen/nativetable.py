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

    # Обратный счёт к тому же выравниванию: «куда на самом деле попадёт точка,
    # если применить SET_SCRIPT_GFX_ALIGN и его параметры». Имени в базе у него
    # нет вовсе — только хеш, — и потому его здесь легко не найти.
    #
    # Опознан не по догадке: подпись из @altv/types-natives
    # (`(x, y, calculatedX, calculatedY) -> [void, number, number]`) сошлась с
    # доводами из базы (float, float, floatPtr, floatPtr, returns void), а стоит
    # он в GRAPHICS сразу за остальными тремя из этой же семьи.
    "getScriptGfxAlignPosition": "0x6DD8F5AA635EB4B2",

    # Двери зданий: у alt:V это «состояние двери», у базы — «предел ускорения».
    # Имена разошлись потому, что довод у натива один и тот же, а толкуют его
    # по-разному: ноль запирает дверь, прочее задаёт, насколько быстро она
    # открывается. Опознаны по подписи (Hash, int, BOOL, BOOL -> void) — среди
    # соседей по OBJECT это единственный с целым доводом, у прочих дробный.
    "doorSystemSetDoorState": "_SET_DOOR_ACCELERATION_LIMIT",
    "doorSystemGetDoorState": "0x160AA1B32F6139B8",

    # Внешность персонажа: цвет волос, цвет слоя и цвет глаз.
    #
    # У alt:V все три названы «tint» — оттенком, — у базы «color». Смысл один и
    # тот же: номер в палитре игры, а не цвет тремя байтами. Опознаны по
    # подписям, сошедшимся довод в довод:
    #   (Ped, int, int) -> void
    #   (Ped, int, int, int, int) -> void
    #   (Ped, int) -> void
    #
    # Эти три — не просто пропущенные имена. Без них не собрать редактор
    # внешности, а он есть у всякого ролевого режима: цвет волос, бровей,
    # бороды и глаз задаются только ими.
    "setPedHairTint": "_SET_PED_HAIR_COLOR",
    "setPedHeadOverlayTint": "_SET_PED_HEAD_OVERLAY_COLOR",
    "setHeadBlendEyeColor": "_SET_PED_EYE_COLOR",

    # Настоящее разрешение экрана — то, в котором игра рисует, а не то, которое
    # объявлено в настройках. У базы имя говорит «действующее», у alt:V —
    # «фактическое». Подпись: (intPtr, intPtr) -> void.
    "getActualScreenResolution": "_GET_ACTIVE_SCREEN_RESOLUTION",

    # Ширина строки на экране, двумя вызовами. Имена у alt:V длинные и
    # описывают, что именно меряется; у базы короткие. Подписи: (charPtr) ->
    # void и (BOOL) -> float.
    #
    # Без них нельзя разместить свой текст в кадре: ширину строки игра называет
    # только так, и всякий интерфейс, считающий её сам, промахнётся.
    "beginTextCommandGetScreenWidthOfDisplayText": "_BEGIN_TEXT_COMMAND_WIDTH",
    "endTextCommandGetScreenWidthOfDisplayText": "_END_TEXT_COMMAND_GET_WIDTH",

    # Полноэкранные эффекты. У alt:V имена от RAGE — animpostfx, — у базы от
    # того, что они делают. Подписи: (charPtr, int, BOOL) -> void, () -> void,
    # (charPtr) -> void.
    #
    # `animpostfxIsRunning` сюда намеренно не вписан: база объявляет доводом
    # `_GET_SCREEN_EFFECT_IS_ACTIVE` логическое, а alt:V — строку. Одно из
    # объявлений неверно, и какое именно — отсюда не видно. Ширина ячейки у них
    # разная (байт против указателя), и ошибка здесь стоила бы вылета игры, а не
    # строки в журнале. Пусть лучше натива не будет вовсе.
    "animpostfxPlay": "_START_SCREEN_EFFECT",
    "animpostfxStopAll": "_STOP_ALL_SCREEN_EFFECTS",
    "animpostfxStop": "_STOP_SCREEN_EFFECT",

    # Указатель мыши: у alt:V «положение», у базы «место». (float, float) -> BOOL.
    "setCursorPosition": "_SET_CURSOR_LOCATION",

    # Гасит городское освещение. У alt:V имя описывает, чем управляют, у базы —
    # как это называется у самой игры. (BOOL) -> void.
    "setArtificialLightsState": "_SET_BLACKOUT",

    # Татуировка на персонаже. У базы имя описывает, что ставят «украшение», у
    # alt:V — что берут его из пары хешей. Подпись: (Ped, Hash, Hash) -> void.
    "addPedDecorationFromHashes": "_SET_PED_DECORATION",

    # Набор частиц для следующего вызова. У базы имя длиннее и договаривает, что
    # действует он ровно один раз. (charPtr) -> void.
    "useParticleFxAsset": "_USE_PARTICLE_FX_ASSET_NEXT_CALL",
}


# Доводы, которых недостаёт в открытой базе имён.
#
# База CitizenFX местами отстала от игры, и отстала молча: у `TASK_JUMP` в ней
# два довода, а в её же описании к нему написано «Definition is wrong. This has
# 4 parameters». Таких нативов сто двадцать шесть.
#
# Наружу это выходит так: натив объявлен принимающим меньше, чем принимает, и
# лишние доводы отбрасываются на границе — **молча**. Режим, передавший их, не
# получает ни ошибки, ни строки в журнале; натив просто делает не то. Место в
# ячейке при этом обнуляется, так что игра читает там ноль, — вот почему это
# годами не всплывало.
#
# Перечень собран сверкой с объявлениями alt:V (`@altv/types-natives`) и
# проверяется ею же: `tools/altvparity/native_arity.py`. Вид довода взят оттуда
# же; там, где alt:V говорит просто «число», он выбран по имени довода —
# `blendOutOverride` дробный, `warpTimerMS` целый.
EXTRA_ARGUMENTS = {
    "_activateRockstarEditor": "i",  # p0: number
    "_getPosixTime": "LLLLLL",  # year?: number | null, month?: number | null, day?: number | null, hour?: number | null, minute?: number | null, second?: number | null
    "_networkCheckDataManagerForHandle": "a",  # gamerHandle?: any | null
    "_networkSpentRequestHeist": "a",  # p3: any
    "_taskStopPhoneGestureAnimation": "f",  # blendOutOverride: number
    "addExplosion": "b",  # noDamage: boolean
    "addScenarioBlockingArea": "a",  # p10: any
    "addStuntJump": "i",  # p17: number
    "addStuntJumpAngled": "i",  # p19: number
    "applyDamageToPed": "ai",  # p3: any, weaponType: number
    "attachEntityToEntity": "a",  # p15: any
    "clearAllPedProps": "a",  # p1: any
    "clearAngledAreaOfVehicles": "aa",  # p12: any, p13: any
    "clearAreaOfVehicles": "ba",  # p9: boolean, p10: any
    "clearPedProp": "a",  # p2: any
    "createIncident": "aa",  # p7: any, p8: any
    "createIncidentWithEntity": "aa",  # p5: any, p6: any
    "createMissionTrain": "aa",  # p5: any, p6: any
    "createVehicle": "b",  # p7: boolean
    "createWeaponObject": "aa",  # p8: any, p9: any
    "datafileCreate": "i",  # p0: number
    "datafileDelete": "i",  # p0: number
    "datafileGetFileDict": "i",  # p0: number
    "drawRect": "b",  # p8: boolean
    "drawSprite": "ba",  # p11: boolean, p12: any
    "enableSpecialAbility": "a",  # p2: any
    "endTextCommandDisplayText": "i",  # p2: number
    "getCurrentPedWeaponEntityIndex": "a",  # p1: any
    "getGroundZFor3dCoord": "b",  # p5: boolean
    "getNumReservedMissionObjects": "a",  # p1: any
    "getNumReservedMissionPeds": "a",  # p1: any
    "getNumReservedMissionVehicles": "a",  # p1: any
    "getPedInVehicleSeat": "b",  # p2: boolean
    "getPedPropIndex": "a",  # p2: any
    "getVehicleNumberOfPassengers": "bb",  # includeDriver: boolean, includeDeadOccupants: boolean
    "hasObjectBeenBroken": "a",  # p1: any
    "hintAmbientAudioBank": "a",  # p2: any
    "hintScriptAudioBank": "a",  # p2: any
    "isEntityDead": "b",  # p1: boolean
    "isSpecialAbilityActive": "a",  # p1: any
    "isSpecialAbilityEnabled": "a",  # p1: any
    "isSpecialAbilityMeterFull": "a",  # p1: any
    "isVehicleSeatFree": "b",  # isTaskRunning: boolean
    "networkBail": "iii",  # p0: number, p1: number, p2: number
    "networkBailTransition": "iii",  # p0: number, p1: number, p2: number
    "networkBuyAirstrike": "a",  # p3: any
    "networkBuyBounty": "a",  # p4: any
    "networkBuyFairgroundRide": "a",  # p4: any
    "networkBuyHeliStrike": "a",  # p3: any
    "networkCanSpendMoney": "a",  # p5: any
    "networkCreateSynchronisedScene": "fi",  # animTime: number, p11: number
    "networkDoTransitionQuickmatch": "aa",  # p4: any, p5: any
    "networkDoTransitionQuickmatchAsync": "aa",  # p4: any, p5: any
    "networkDoTransitionQuickmatchWithGroup": "aa",  # p6: any, p7: any
    "networkFadeInEntity": "a",  # p2: any
    "networkHostTransition": "biai",  # p6: boolean, p7: number, p8: any, p9: number
    "networkRegisterHostBroadcastVariables": "s",  # debugName: string | null
    "networkRegisterPlayerBroadcastVariables": "s",  # debugName: string | null
    "networkResurrectLocalPlayer": "bii",  # p6: boolean, p7: number, p8: number
    "networkSpentAmmoDrop": "a",  # p3: any
    "networkSpentBoatPickup": "a",  # p3: any
    "networkSpentBullShark": "a",  # p3: any
    "networkSpentBuyOfftheradar": "a",  # p3: any
    "networkSpentBuyPassiveMode": "a",  # p3: any
    "networkSpentBuyRevealPlayers": "a",  # p3: any
    "networkSpentBuyWantedlevel": "a",  # p4: any
    "networkSpentHeliPickup": "a",  # p3: any
    "networkSpentHireMercenary": "a",  # p3: any
    "networkSpentHireMugger": "a",  # p3: any
    "networkSpentNoCops": "a",  # p3: any
    "networkSpentRequestJob": "a",  # p3: any
    "networkSpentRobbedByMugger": "a",  # p3: any
    "networkSpentTaxi": "aa",  # p3: any, p4: any
    "playPain": "a",  # p3: any
    "removeDoorFromSystem": "a",  # p1: any
    "renderScriptCams": "a",  # p5: any
    "requestAmbientAudioBank": "a",  # p2: any
    "requestMissionAudioBank": "a",  # p2: any
    "requestScriptAudioBank": "a",  # p2: any
    "setBlipShowCone": "i",  # hudColorIndex: number
    "setEmitterRadioStation": "a",  # p2: any
    "setEntityHealth": "ii",  # instigator: Entity | number, weaponType: number
    "setEntityLoadCollisionFlag": "a",  # p2: any
    "setMountedWeaponTarget": "ib",  # taskMode: number, ignoreTargetVehDeadCheck: boolean
    "setMpGamerTagVisibility": "a",  # p3: any
    "setNetworkVehicleRespotTimer": "aa",  # p2: any, p3: any
    "setObjectTargettable": "a",  # p2: any
    "setPedAmmo": "b",  # p3: boolean
    "setPedHelmetPropIndex": "b",  # p2: boolean
    "setPedPathsBackToOriginal": "a",  # p6: any
    "setPedPathsInArea": "a",  # p7: any
    "setPedPropIndex": "a",  # p5: any
    "setPlayerMeleeWeaponDamageModifier": "b",  # p2: boolean
    "setRoadsBackToOriginal": "a",  # p6: any
    "setRoadsBackToOriginalInAngledArea": "a",  # p7: any
    "setVehicleExclusiveDriver": "i",  # index: number
    "setVehicleOnGroundProperly": "i",  # p1: number
    "setWarningMessage": "i",  # errorCode: number
    "setWarningMessageWithHeader": "a",  # p9: any
    "simulatePlayerInputGait": "a",  # p6: any
    "specialAbilityChargeAbsolute": "a",  # p3: any
    "specialAbilityChargeContinuous": "a",  # p2: any
    "specialAbilityChargeLarge": "a",  # p3: any
    "specialAbilityChargeMedium": "a",  # p3: any
    "specialAbilityChargeNormalized": "a",  # p3: any
    "specialAbilityChargeSmall": "a",  # p3: any
    "specialAbilityDeactivate": "a",  # p1: any
    "specialAbilityDeactivateFast": "a",  # p1: any
    "specialAbilityDepleteMeter": "a",  # p2: any
    "specialAbilityFillMeter": "a",  # p2: any
    "specialAbilityLock": "a",  # p1: any
    "specialAbilityReset": "a",  # p1: any
    "specialAbilityUnlock": "a",  # p1: any
    "statSave": "b",  # p3: boolean
    "stopCutsceneCamShaking": "a",  # p0: any
    "taskEnterVehicle": "a",  # p7: any
    "taskGoToCoordAnyMeansExtraParams": "i",  # warpTimerMS: number
    "taskGoToCoordAnyMeansExtraParamsWithCruiseSpeed": "f",  # targetArriveDist: number
    "taskJump": "bb",  # doSuperJump: boolean, useFullSuperJumpForce: boolean
    "taskParachute": "b",  # instant: boolean
    "taskPlaneMission": "b",  # precise: boolean
    "taskShuffleToNextVehicleSeat": "b",  # useAlternateShuffle: boolean
    "taskSkyDive": "b",  # instant: boolean
    "taskThrowProjectile": "ib",  # ignoreCollisionEntityIndex: number, createInvincibleProjectile: boolean
    "taskUseMobilePhone": "i",  # desiredPhoneMode: number
    "taskWrithe": "bi",  # forceShootOnGround: boolean, shootFromGroundTimer: number
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

        name = camel_case(native)

        # Недостающие доводы дописываются здесь, а не правятся в готовой
        # таблице: правка в таблице пропала бы при первой же пересборке.
        letters.append(EXTRA_ARGUMENTS.get(name, EXTRA_ARGUMENTS.get(name.lstrip('_'), '')))

        signature = f"{build:X}|{''.join(letters)}:{result}"

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
