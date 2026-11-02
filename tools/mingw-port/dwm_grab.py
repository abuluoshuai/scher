"""Capture tinta's window via DWM thumbnail (works with D2D flip-model).

PrintWindow cannot see flip-model swap chains; DWM thumbnail redirection
can. We register a thumbnail of the target window into our own layered
window, copy it to a GDI bitmap, and save.
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
dwmapi = ctypes.windll.dwmapi

user32.SetProcessDPIAware()
PROC = "tinta.exe"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "tinta-window.png")

# window messages / structures
WM_CLOSE = 0x0010
DWM_SIT_DISPLAYFRAME = 0x8


class DWM_THUMBNAIL_PROPERTIES(ctypes.Structure):
    _fields_ = [
        ("dwFlags", wt.DWORD),
        ("rcDestination", wt.RECT),
        ("rcSource", wt.RECT),
        ("opacity", wt.BYTE),
        ("fVisible", ctypes.c_int),
        ("fSourceClientAreaOnly", ctypes.c_int),
        ("fAlphaChannel", ctypes.c_int),
    ]


def find_tinta_hwnd():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq " + PROC, "/FO", "CSV", "/NH"],
        capture_output=True, text=True, errors="replace").stdout
    pids = set()
    for line in out.splitlines():
        cells = [c.strip('"') for c in line.split('","')]
        if len(cells) > 1 and cells[0].lower() == PROC and cells[1].isdigit():
            pids.add(int(cells[1]))
    found = []
    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(h, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            found.append(h)
        return True
    user32.EnumWindows(visit, 0)
    return found[0] if found else None


def main():
    target = find_tinta_hwnd()
    if not target:
        print("tinta not running")
        return 1
    rect = wt.RECT()
    user32.GetWindowRect(target, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    print("target=%d %dx%d" % (target, w, h))

    # Own message-only-ish window (overlapped, off-screen)
    WS_POPUP = 0x80000000
    hwnd = user32.CreateWindowExW(
        0, "STATIC", "grab", WS_POPUP,
        -w - 100, 0, w, h, None, None, None, None)
    if not hwnd:
        print("CreateWindow failed")
        return 1

    thumb = ctypes.c_void_p()
    hr = dwmapi.DwmRegisterThumbnail(hwnd, target, ctypes.byref(thumb))
    if hr != 0:
        print("DwmRegisterThumbnail hr=0x%08x" % (hr & 0xFFFFFFFF))
        user32.DestroyWindow(hwnd)
        return 1

    props = DWM_THUMBNAIL_PROPERTIES()
    props.dwFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x10  # dest/src/opacity/visible/sourceclient
    props.rcDestination = wt.RECT(0, 0, w, h)
    props.rcSource = wt.RECT(0, 0, w, h)
    props.opacity = 255
    props.fVisible = 1
    props.fSourceClientAreaOnly = 1
    hr = dwmapi.DwmUpdateThumbnailProperties(thumb, ctypes.byref(props))
    if hr != 0:
        print("DwmUpdateThumbnailProperties hr=0x%08x" % (hr & 0xFFFFFFFF))
        dwmapi.DwmUnregisterThumbnail(thumb)
        user32.DestroyWindow(hwnd)
        return 1

    # let DWM compose a frame or two
    for _ in range(12):
        user32.UpdateWindow(hwnd)
        time.sleep(0.05)

    hdc = user32.GetDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, 0x00CC0020)  # SRCCOPY

    class BMIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                    ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bmi = BMIH()
    bmi.biSize = ctypes.sizeof(BMIH)
    bmi.biWidth = w
    bmi.biHeight = -h
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    Image.frombytes("RGBX", (w, h), buf.raw).convert("RGB").save(OUT)
    print("saved", OUT)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    dwmapi.DwmUnregisterThumbnail(thumb)
    user32.DestroyWindow(hwnd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
