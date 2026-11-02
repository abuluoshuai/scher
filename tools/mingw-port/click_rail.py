"""Click the vertical rail rows and report title changes + probe lines."""

import ctypes
import ctypes.wintypes as wt
import io
import subprocess
import time

user32 = ctypes.windll.user32
PROBE = r"C:/Users/sch/AppData/Local/Temp/temp_tinta_probe.txt"


def tinta_pid():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq scher.exe", "/FO", "CSV", "/NH"],
        capture_output=True, text=True, errors="replace").stdout
    for line in out.splitlines():
        c = [x.strip('"') for x in line.split('","')]
        if len(c) > 1 and c[0].lower() == "scher.exe" and c[1].isdigit():
            return int(c[1])
    return None


def hwnd_of():
    pid = tinta_pid()
    if pid is None:
        return None
    target = pid
    found = []
    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(h, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == target and user32.IsWindowVisible(h):
            found.append(h)
        return True
    user32.EnumWindows(visit, 0)
    return found[0] if found else None


def title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def click(hwnd, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0200, 0, lp)
    time.sleep(0.15)
    user32.PostMessageW(hwnd, 0x0201, 1, lp)
    time.sleep(0.1)
    user32.PostMessageW(hwnd, 0x0202, 0, lp)


def probe_tail(n=6):
    try:
        lines = [l.strip() for l in io.open(PROBE, encoding="utf-8",
                                            errors="replace") if l.strip()]
        return lines[-n:]
    except OSError:
        return ["<no probe file>"]


def main():
    hwnd = hwnd_of()
    if not hwnd:
        print("tinta not running")
        return 1
    print("title:", title(hwnd))
    print("probe before:", probe_tail(2))

    click(hwnd, 60, 57)      # row 1 (listTop=40, rowH=34)
    time.sleep(1.2)
    t1 = title(hwnd)
    print("after row1:", t1)

    click(hwnd, 60, 125)     # row 3
    time.sleep(1.2)
    t3 = title(hwnd)
    print("after row3:", t3)
    print("probe after:", probe_tail(6))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
