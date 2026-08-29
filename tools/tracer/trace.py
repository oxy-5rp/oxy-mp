# -*- coding: utf-8 -*-
"""Трассировщик обфусцированного кода GTA5 в живом процессе.

Обфускация здесь — расплющенный поток управления: настоящие инструкции разбавлены
безусловными переходами-переходниками. Линейный дизассемблер тонет в них; этот
идёт по переходам, а печатает только то, что делает работу.
"""
import ctypes, ctypes.wintypes as wt, capstone, subprocess, re, sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]

номер = int(re.findall(r'"GTA5.exe","(\d+)"', subprocess.run(
    ['tasklist', '/FI', 'IMAGENAME eq GTA5.exe', '/FO', 'CSV', '/NH'],
    capture_output=True, text=True).stdout)[0])
h = k32.OpenProcess(0x0010 | 0x0400, False, номер)
if not h:
    raise SystemExit('процесс не открылся')

кеш = {}

def читать(куда, сколько=0x40):
    ключ = куда >> 12
    if ключ not in кеш:
        буфер = (ctypes.c_ubyte * 0x1000)()
        прочитано = ctypes.c_size_t(0)
        ok = k32.ReadProcessMemory(h, ctypes.c_void_p(ключ << 12), буфер, 0x1000,
                                   ctypes.byref(прочитано))
        кеш[ключ] = bytes(буфер[:прочитано.value]) if ok else b''
    страница = кеш[ключ]
    начало = куда & 0xFFF
    кусок = страница[начало:начало + сколько]
    if len(кусок) < сколько:
        # Инструкция может пересечь границу страницы — дочитываем следующую.
        кусок += читать((ключ + 1) << 12, сколько - len(кусок))
    return кусок

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

def трасса(старт, предел=400):
    """Идёт от старта, распутывая переходы. Печатает работу, а не переходники."""
    адрес = старт
    видели = set()
    шагов = 0
    ветвления = []

    # Подменённый адрес возврата — главный приём здешней обфускации:
    #
    #     push  rsi                     ; занять место под адрес
    #     lea   rsi, [rip + смещение]   ; куда на самом деле идём
    #     xchg  qword ptr [rsp], rsi    ; положить это вместо адреса возврата
    #     ret                           ; и «вернуться» туда
    #
    # Это обычный `jmp`, написанный четырьмя инструкциями. Не распутав его,
    # трассировка обрывается на `ret` в самом начале работы.
    подменён = None
    последнийLea = {}

    while шагов < предел:
        if адрес in видели:
            print('  ... уже были здесь, %#x' % адрес)
            break
        видели.add(адрес)

        байты = читать(адрес)
        if not байты:
            print('  ... не читается %#x' % адрес)
            break

        try:
            ins = next(md.disasm(байты, адрес))
        except StopIteration:
            print('  ... не разбирается %#x' % адрес)
            break

        шагов += 1

        if ins.mnemonic == 'jmp' and ins.op_str.startswith('0x'):
            адрес = int(ins.op_str, 16)
            continue

        if ins.mnemonic == 'lea' and '[rip' in ins.op_str:
            # lea REG, [rip + d] — запоминаем, куда он указывает.
            цель = ins.op_str.split(',')[0].strip()
            смещение = ins.op_str.split('rip')[1].rstrip(']').strip()
            знак = 1 if смещение.startswith('+') else -1
            число = int(смещение.lstrip('+- '), 16)
            последнийLea[цель] = ins.address + ins.size + знак * число

        if ins.mnemonic == 'xchg' and 'qword ptr [rsp]' in ins.op_str:
            кого = ins.op_str.split(',')[1].strip()
            подменён = последнийLea.get(кого)

        if ins.mnemonic == 'ret':
            if подменён is not None:
                адрес = подменён
                подменён = None
                continue

            print('  ret')
            break

        if ins.mnemonic.startswith('j'):
            ветвления.append((ins.address, ins.mnemonic, ins.op_str))
            print('  %#x  %s %s   <-- ветвление' % (ins.address, ins.mnemonic, ins.op_str))
            адрес = ins.address + ins.size
            continue

        print('  %#x  %s %s' % (ins.address, ins.mnemonic, ins.op_str))
        адрес = ins.address + ins.size

    return ветвления

if __name__ == '__main__':
    старт = int(sys.argv[1], 16)
    предел = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    трасса(старт, предел)
