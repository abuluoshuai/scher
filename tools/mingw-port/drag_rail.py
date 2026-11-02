"""Regression 2: drag the rail's right edge and verify the width persists.

Sends a press on the edge (x=300, the configured width), moves to x=420,
releases, then closes the app gracefully and checks settings.ini for the
new tabsWidth.
"""

import ctypes
import ctypes.wintypes as wt
import io
import subprocess
import time

user32 = ctypes.windll.user32
INI = r"C:/Users/sch/AppData/Local/Temp/tinta-test/settings.ini"


def tinta_pid():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq tinta.exe", "/FO", "CSV", "/NH"],
        capture_output=True, text=True, errors="replace").stdout
    for line in out.splitlines():
        c = [x.strip('"') for x in line.split('","')]
        if len(c) > 1 and c[0].lower() == "tinta.exe" and c[1].isdigit():
            return int(c[1])
    return None


def hwnd_of():
    pid = tinta_pid()
    found = []
    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(h, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h):
            found.append(h)
        return True
    user32.EnumWindows(visit, 0)
    return found[0] if found else None


def read_width():
    for line in io.open(INI, encoding="utf-8", errors="replace"):
        if line.startswith("tabsWidth="):
            return float(line.split("=", 1)[1])
    return None


def main():
    hwnd = hwnd_of()
    assert hwnd, "tinta not running"
    print("before drag: tabsWidth =", read_width())

    # press on the edge, drag right by ~120px, release
    user32.PostMessageW(hwnd, 0x0200, 0, (400 << 16) | 300)
    time.sleep(0.15)
    user32.PostMessageW(hwnd, 0x0201, 1, (400 << 16) | 300)
    time.sleep(0.15)
    for x in range(300, 421, 15):
        user32.PostMessageW(hwnd, 0x0200, 1, (400 << 16) | x)
        time.sleep(0.03)
    user32.PostMessageW(hwnd, 0x0202, 0, (400 << 16) | 420)
    time.sleep(0.8)
    print("after drag (in-app):", read_width() if read_width() else "(not yet saved)")

    # graceful close flushes settings
    user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(2.5)
    print("after close: tabsWidth =", read_width())
    w = read_width()
    ok = w is not None and 400 <= w <= 440
    print("RESULT:", "OK" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
