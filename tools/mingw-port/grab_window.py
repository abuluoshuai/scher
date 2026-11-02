"""Grab a screenshot of the running Tinta window (diagnostic helper).

Enumerates top-level windows, keeps the visible ones owned by the named
process, and saves the largest as a PNG so the build can be eyeballed.
"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

from PIL import ImageGrab

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

PROC_NAME = "tinta.exe"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tinta-window.png")


def tinta_pids():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq " + PROC_NAME, "/FO", "CSV", "/NH"],
        capture_output=True, text=True, errors="replace",
    ).stdout
    pids = set()
    for line in out.splitlines():
        cells = [c.strip('"') for c in line.split('","')]
        if len(cells) > 1 and cells[0].lower() == PROC_NAME and cells[1].isdigit():
            pids.add(int(cells[1]))
    return pids


def windows_of(pids):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(hwnd, _lparam):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            rect = wt.RECT()
            if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                area = (rect.right - rect.left) * (rect.bottom - rect.top)
                if area > 10000:
                    found.append((area, hwnd, rect))
        return True

    user32.EnumWindows(visit, 0)
    return sorted(found, reverse=True)


def title_of(hwnd):
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def main():
    pids = tinta_pids()
    if not pids:
        print("tinta.exe is not running")
        return 1

    wins = windows_of(pids)
    if not wins:
        print("no visible window for pid(s) %s" % sorted(pids))
        return 1

    hwnd = wins[0][1]
    rect = wins[0][2]
    box = (rect.left, rect.top, rect.right, rect.bottom)
    print("pids=%s windows=%d" % (sorted(pids), len(wins)))
    for area, h, r in wins:
        print("  hwnd=%d area=%d rect=%s title=%r"
              % (h, area, (r.left, r.top, r.right, r.bottom), title_of(h)))

    # The window must be in front, otherwise the grab just photographs
    # whatever happens to be on top of that screen region.
    user32.ShowWindow(hwnd, 9)          # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    time.sleep(1.2)
    if user32.GetForegroundWindow() != hwnd:
        print("warning: could not raise the window, the shot may show another app")

    ImageGrab.grab(bbox=box, all_screens=True).save(OUT)
    print("saved %s (%dx%d)" % (OUT, box[2] - box[0], box[3] - box[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
