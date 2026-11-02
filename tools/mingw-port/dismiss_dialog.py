"""Dismiss the first-run "file association" prompt with No.

Picks the button whose caption means "No" and clicks it, leaving the user's
file associations untouched. Prints the dialog's button captions for the
record so it is clear what was clicked.
"""

import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

PROC_NAME = "scher.exe"
NO_LABELS = ("\u5426", "No", "\u4e0d")
YES_LABELS = ("\u662f", "Yes")


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


def text_of(hwnd):
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def top_windows(pids):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(hwnd, _lparam):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(visit, 0)
    return found


def children(hwnd):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def visit(child, _lparam):
        found.append(child)
        return True

    user32.EnumChildWindows(hwnd, visit, 0)
    return found


def main():
    pids = tinta_pids()
    if not pids:
        print("scher.exe is not running")
        return 1

    target = None
    for hwnd in top_windows(pids):
        if text_of(hwnd).startswith("Scher - ") and text_of(hwnd) not in ("Scher",):
            target = hwnd
    if target is None:
        print("no association dialog found")
        return 1

    print("dialog hwnd=%d title=%r" % (target, text_of(target)))

    clicked = False
    for child in children(target):
        label = text_of(child)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(child, cls, 256)
        if cls.value != "Button":
            continue
        print("  button hwnd=%d class=%s label=%r" % (child, cls.value, label))
        if label.startswith(NO_LABELS) and not clicked:
            user32.SendMessageW(child, 0x00F5, 0, 0)  # BM_CLICK
            print("  -> clicked %r" % label)
            clicked = True

    if not clicked:
        user32.PostMessageW(target, 0x0111, 7, 0)  # WM_COMMAND / IDNO
        print("  -> no button matched, posted WM_COMMAND IDNO")

    return 0


if __name__ == "__main__":
    sys.exit(main())
