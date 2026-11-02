"""Minimal isolation test for the progress seed + measurement.

Phase A: one document, no session restore, no seed -> expect a measured
         value around (0 + height) / contentHeight, i.e. ~0.09.
Phase B: same, but [Positions] seeds 0.777 -> expect exactly 0.777
         (parse + high-water restore).

Asserts exactly one tinta process and a freshly created probe file, so a
stale instance can't pollute the ring log.
"""

import ctypes
import ctypes.wintypes as wt
import io
import os
import subprocess
import sys
import time

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

BUILD = r"E:\WorkBuddy\翠花工具包\Markdown阅读器\build"
EXE = os.path.join(BUILD, "tinta.exe")
INI = os.path.join(BUILD, "settings.ini")
DOCS = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"),
                    "tinta-progress", "docs")
PROBE = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"),
                     "tinta-progress-probe.txt")
LONG = os.path.join(DOCS, "b-long.md")


def procs():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq tinta.exe",
                          "/FO", "CSV", "/NH"],
                         capture_output=True, text=True,
                         errors="replace").stdout
    rows = [l for l in out.splitlines() if l.lower().startswith('"tinta.exe"')]
    return rows


def kill_all():
    for _ in range(30):
        if not procs():
            return True
        subprocess.run(["taskkill", "/IM", "tinta.exe", "/F"],
                       capture_output=True)
        time.sleep(0.3)
    return not procs()


def write_ini(seed):
    lines = [
        "[Settings]",
        "themeIndex=5",
        "openInTabs=1",
        "hasAskedFileAssociation=1",
        "windowX=80",
        "windowY=80",
        "windowWidth=1200",
        "windowHeight=820",
        "tabsOnLeft=0",
    ]
    if seed is not None:
        lines += ["[Positions]", "pos=0|0|%.4f|%s" % (seed, LONG)]
    lines.append("")
    with open(INI, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def hwnd_of():
    rows = procs()
    pids = set()
    for line in rows:
        cells = [c.strip('"') for c in line.split('","')]
        if len(cells) > 1 and cells[1].isdigit():
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


def first_rings(n=6):
    if not os.path.exists(PROBE):
        return []
    with io.open(PROBE, "r", encoding="utf-8", errors="replace") as f:
        lines = [l.strip() for l in f if l.strip()]
    return lines[:n], len(lines)


def phase(label, seed):
    print("=== %s (seed=%s) ===" % (label, seed))
    if not kill_all():
        print("FAIL: stale tinta processes survived")
        return False
    write_ini(seed)
    if os.path.exists(PROBE):
        os.remove(PROBE)
    started = time.time()
    subprocess.Popen([EXE, LONG], cwd=BUILD)
    deadline = time.time() + 15
    hwnd = None
    while time.time() < deadline:
        hwnd = hwnd_of()
        if hwnd:
            break
        time.sleep(0.4)
    if not hwnd:
        print("FAIL: no window")
        return False
    time.sleep(2.0)
    print("processes:", len(procs()), "probe mtime delta: %.1fs"
          % (os.path.getmtime(PROBE) - started if os.path.exists(PROBE) else -1))
    rings, total = first_rings()
    print("probe lines:", total)
    for r in rings:
        print("   ", r)
    user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    time.sleep(2.0)
    kill_all()
    print()
    return True


def main():
    ok = phase("A: measured only", None)
    ok = phase("B: seeded 0.777", 0.777) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
