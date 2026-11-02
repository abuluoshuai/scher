"""Drive tinta into a real multi-tab state, then verify the left rail.

Simulates a shell drag-drop of several .md files onto the window
(WM_DROPFILES with a fake HDROP), which the app handles by opening each
file as a new tab. Then reads the probe log to confirm the vertical-rail
render branch runs with the right tab count, and clicks a row to check
tab switching (window title changes).
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()
shell32 = ctypes.windll.shell32
kernel32 = ctypes.windll.kernel32

GHND = 0x0042
WM_DROPFILES = 0x0233
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_MOUSEMOVE = 0x0200


class DROPFILES(ctypes.Structure):
    _fields_ = [("pFiles", wt.DWORD), ("pt", wt.POINT),
                ("fNC", ctypes.c_int), ("fWide", ctypes.c_int)]

DOCS = r"C:\Users\sch\AppData\Local\Temp\tinta-test\docs"
PROBE = r"C:\temp_tinta_probe.txt"


def hwnd_of():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq tinta.exe", "/FO", "CSV", "/NH"],
        capture_output=True, text=True, errors="replace").stdout
    pids = set()
    for line in out.splitlines():
        cells = [c.strip('"') for c in line.split('","')]
        if len(cells) > 1 and cells[0].lower() == "tinta.exe" and cells[1].isdigit():
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


def make_hdrop(paths):
    """Build a global HDROP for the given file paths."""
    # DROPFILES header + double-null-terminated wide list
    total = ctypes.sizeof(DROPFILES)  # header
    for p in paths:
        total += (len(p) + 1) * 2
    total += 2
    h = kernel32.GlobalAlloc(GHND, total)
    mem = kernel32.GlobalLock(h)
    df = DROPFILES.from_address(mem)
    df.pFiles = ctypes.sizeof(DROPFILES)
    df.fWide = 1
    off = ctypes.sizeof(DROPFILES)
    for p in paths:
        enc = p.encode("utf-16-le") + b"\x00\x00"
        ctypes.memmove(mem + off, enc, len(enc))
        off += len(enc)
    ctypes.memmove(mem + off, b"\x00\x00", 2)
    kernel32.GlobalUnlock(h)
    return h


def drop(hwnd, paths):
    h = make_hdrop(paths)
    user32.SendMessageW(hwnd, WM_DROPFILES, h, 0)
    # the app DragFinish's the handle


def title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def click(hwnd, x, y):
    lp = (y << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp)
    time.sleep(0.12)
    user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
    time.sleep(0.08)
    user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)


def main():
    hwnd = hwnd_of()
    if not hwnd:
        print("tinta not running")
        return 1
    print("hwnd:", hwnd, "title:", title(hwnd))

    files = [os.path.join(DOCS, n) for n in sorted(os.listdir(DOCS))]
    files = [f for f in files if f.endswith(".md")]
    print("dropping %d files" % len(files))
    drop(hwnd, files)
    time.sleep(2.5)

    print("title after drop:", title(hwnd))
    if os.path.exists(PROBE):
        with io_open(PROBE) as f:
            lines = [l.rstrip() for l in f.readlines() if l.strip()]
        print("probe lines: %d, last 3:" % len(lines))
        for l in lines[-3:]:
            print("  ", l)
    else:
        print("probe file missing")

    # Click rail rows: row1 y=57, row3 y=125 (rowH=34, top=40)
    t0 = title(hwnd)
    click(hwnd, 60, 57)
    time.sleep(1.0)
    t1 = title(hwnd)
    click(hwnd, 60, 125)
    time.sleep(1.0)
    t3 = title(hwnd)
    print("before:", t0)
    print("after row1:", t1)
    print("after row3:", t3)
    return 0


def io_open(path):
    import io
    return io.open(path, "r", encoding="utf-16-le", errors="replace")


if __name__ == "__main__":
    sys.exit(main())
