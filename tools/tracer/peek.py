# -*- coding: utf-8 -*-
import ctypes, ctypes.wintypes as wt, subprocess, re, sys, time
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
номер = int(re.findall(r'"GTA5.exe","(\d+)"', subprocess.run(
    ['tasklist','/FI','IMAGENAME eq GTA5.exe','/FO','CSV','/NH'],
    capture_output=True, text=True).stdout)[0])
h = k32.OpenProcess(0x0010 | 0x0400, False, номер)

def dword(куда):
    б = (ctypes.c_uint32 * 1)()
    п = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(куда), б, 4, ctypes.byref(п)):
        return None
    return б[0]

адреса = [int(a, 16) for a in sys.argv[1:]]
for _ in range(3):
    print(time.strftime('%H:%M:%S'), ' '.join('%#x=%d' % (а, dword(а) or -1) for а in адреса))
    time.sleep(5)
