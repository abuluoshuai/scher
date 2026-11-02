"""Capture a specific window's content even when occluded (PrintWindow).

Rendered target: prints the tinta window with PW_RENDERFULLCONTENT so
Direct2D content shows. Also reports GetDpiForWindow so test coordinates
can be scaled correctly.
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys

from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
user32.SetProcessDPIAware()

PROC = "tinta.exe"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "tinta-window.png")


def hwnd_of():
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
    hwnd = hwnd_of()
    if not hwnd:
        print("tinta not running")
        return 1
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, h = rect.right - rect.left, rect.bottom - rect.top
    try:
        dpi = user32.GetDpiForWindow(hwnd)
    except AttributeError:
        dpi = 96
    print("hwnd=%d rect=%s size=%dx%d dpi=%d (scale=%.2f)"
          % (hwnd, (rect.left, rect.top, rect.right, rect.bottom), w, h,
             dpi, dpi / 96.0))

    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mem, bmp)
    # PW_RENDERFULLCONTENT = 2 (captures D2D/DWM content)
    user32.PrintWindow(hwnd, mem, 2)

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
    bmi.biCompression = 0  # BI_RGB

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)

    img = Image.frombytes("RGBA", (w, h), buf.raw)
    img = Image.frombytes("RGB", (w, h),
                          img.tobytes()) if False else img.convert("RGB")
    img.save(OUT)
    print("saved", OUT)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
