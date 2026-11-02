"""Drive the SSH remote UI end to end (no screenshots: the desktop is
locked, so every step asserts on the window title or settings.ini).

Flow: app menu -> Open remote file -> browser lists the fake server ->
pick two files (one with a Chinese name) -> Open -> two read-only tabs ->
recent-documents menu -> its "More" dialog -> open from there.
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

ROOT = r"E:\WorkBuddy\翠花工具包\Markdown阅读器"
BUILD = os.path.join(ROOT, "build")
EXE = os.path.join(BUILD, "scher.exe")
INI = os.path.join(BUILD, "settings.ini")
TMP = os.environ["TEMP"]
FAKE_ROOT = os.path.join(TMP, "fake-remote")
FAKE_CFG = os.path.join(TMP, "fake-ssh-config")

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_CLOSE = 0x0010

failures = []


def check(ok, what, extra=""):
    print(("PASS " if ok else "FAIL ") + what + (" " + extra if extra else ""))
    if not ok:
        failures.append(what)


def procs():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq scher.exe",
                          "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, errors="replace").stdout
    return [l for l in out.splitlines() if l.lower().startswith('"scher.exe"')]


def kill_all():
    for _ in range(25):
        if not procs():
            return
        subprocess.run(["taskkill", "/IM", "scher.exe", "/F"], capture_output=True)
        time.sleep(0.3)


def hwnd_of():
    pids = set()
    for line in procs():
        c = [x.strip('"') for x in line.split('","')]
        if len(c) > 1 and c[1].isdigit():
            pids.add(int(c[1]))
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


def title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def click(hwnd, x, y, right=False):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp)
    time.sleep(0.08)
    if right:
        user32.PostMessageW(hwnd, 0x0204, 2, lp)
        time.sleep(0.08)
        user32.PostMessageW(hwnd, 0x0205, 0, lp)
    else:
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
        time.sleep(0.06)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    time.sleep(0.45)


def main():
    kill_all()
    io.open(INI, "w", encoding="utf-8").write(
        "[Settings]\nthemeIndex=5\nhasAskedFileAssociation=1\nwindowX=80\n"
        "windowY=80\nwindowWidth=1200\nwindowHeight=820\n")
    env = dict(os.environ, TINTA_SSH_EXE=os.path.join(BUILD, "fake-ssh.exe"),
               TINTA_SSH_CONFIG=FAKE_CFG, FAKE_SSH_ROOT=FAKE_ROOT)
    subprocess.Popen([EXE], cwd=BUILD, env=env)
    deadline = time.time() + 15
    hwnd = None
    while time.time() < deadline:
        hwnd = hwnd_of()
        if hwnd:
            break
        time.sleep(0.4)
    if not hwnd:
        print("FAIL: no window")
        return 1
    time.sleep(2.0)
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    print("window", rect.right - rect.left, "x", rect.bottom - rect.top,
          "title:", title(hwnd))
    t0 = title(hwnd)

    # 1. Application menu -> Open remote file
    click(hwnd, 20, 20)
    click(hwnd, 120, 121)          # third entry
    time.sleep(1.2)
    t1 = title(hwnd)
    check("Scher" in t1, "remote browser did not disturb the title", t1)

    # 2. Browser listing: dirs first, then two files
    click(hwnd, 400, 236)          # row 1 = 笔记 一.md
    click(hwnd, 400, 262)          # row 2 = plain.md
    click(hwnd, 1000, 704)         # Open
    time.sleep(1.8)
    t2 = title(hwnd)
    check(t2 != t0 and "Scher - " in t2, "multi-select opened a remote tab", t2)

    stored = io.open(INI, encoding="utf-8", errors="replace").read()
    remotes = [l for l in stored.splitlines() if l.startswith("remoteRecent=")]
    check(len(remotes) == 2, "both remote documents recorded", str(len(remotes)))
    check(any("笔记 一.md" in r for r in remotes),
          "Chinese remote name recorded intact")

    # 3. Recent-documents menu (hover/click button right of the pin)
    btn_x = (rect.right - rect.left) - 3 * 46 - 34 - 17
    click(hwnd, btn_x, 20)
    time.sleep(0.6)
    click(hwnd, 700, 144)          # first remote row in the menu
    time.sleep(1.0)
    t3 = title(hwnd)
    check("Scher - " in t3, "history menu opened a recent remote document", t3)

    # 4. "More" dialog and a row inside it
    click(hwnd, btn_x, 20)
    time.sleep(0.6)
    click(hwnd, 700, 207)          # More...
    time.sleep(0.6)
    click(hwnd, 700, 164)          # first row of the remote column
    time.sleep(1.0)
    t4 = title(hwnd)
    check("Scher - " in t4, "history dialog opened a remote document", t4)

    # 5. Read-only: Ctrl+E must not switch into the editor
    user32.PostMessageW(hwnd, 0x0100, 0x11, 0)      # Ctrl down
    user32.PostMessageW(hwnd, 0x0100, 0x45, 0)      # E
    user32.PostMessageW(hwnd, 0x0101, 0x45, 0)
    user32.PostMessageW(hwnd, 0x0101, 0x11, 0)
    time.sleep(0.8)
    t5 = title(hwnd)
    check(not t5.startswith("Scher - *") and t5 == t4,
          "Ctrl+E stayed read-only", t5)

    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    time.sleep(1.5)
    kill_all()
    print("failures:", len(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
