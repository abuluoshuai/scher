"""Second pass: session restore of remote tabs, directory navigation in the
browser, and the local section of the recent-documents menu."""

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
EXE = os.path.join(BUILD, "tinta.exe")
INI = os.path.join(BUILD, "settings.ini")
TMP = os.environ["TEMP"]
FAKE_ROOT = os.path.join(TMP, "fake-remote")
FAKE_CFG = os.path.join(TMP, "fake-ssh-config")
LOCAL = os.path.join(TMP, "本地样例.md")

failures = []


def check(ok, what, extra=""):
    print(("PASS " if ok else "FAIL ") + what + (" " + extra if extra else ""))
    if not ok:
        failures.append(what)


def procs():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq tinta.exe",
                          "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, errors="replace").stdout
    return [l for l in out.splitlines() if l.lower().startswith('"tinta.exe"')]


def kill_all():
    for _ in range(25):
        if not procs():
            return
        subprocess.run(["taskkill", "/IM", "tinta.exe", "/F"], capture_output=True)
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


def click(hwnd, x, y):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0200, 0, lp)
    time.sleep(0.08)
    user32.PostMessageW(hwnd, 0x0201, 1, lp)
    time.sleep(0.06)
    user32.PostMessageW(hwnd, 0x0202, 0, lp)
    time.sleep(0.45)


def launch(env):
    subprocess.Popen([EXE], cwd=BUILD, env=env)
    deadline = time.time() + 15
    while time.time() < deadline:
        h = hwnd_of()
        if h:
            time.sleep(3.0)   # session restore + ssh fetch
            return h
        time.sleep(0.4)
    return None


def main():
    kill_all()
    io.open(LOCAL, "w", encoding="utf-8").write("# 本地样例\n\n本地最近文件。\n")

    # Settings: two remote session tabs from the previous run plus a local
    # recent entry, so all three sections of the menu have content
    io.open(INI, "w", encoding="utf-8").write(
        "[Settings]\nthemeIndex=5\nhasAskedFileAssociation=1\nwindowX=80\n"
        "windowY=80\nwindowWidth=1200\nwindowHeight=820\nsessionActive=0\n"
        "tab=ssh://testbox/plain.md\ntab=ssh://testbox/笔记 一.md\n"
        "[Recent]\nrecent=1|%s\nremoteRecent=2|ssh://testbox/笔记 一.md\n"
        "remoteRecent=3|ssh://testbox/plain.md\n" % LOCAL)

    env = dict(os.environ, TINTA_SSH_EXE=os.path.join(BUILD, "fake-ssh.exe"),
               TINTA_SSH_CONFIG=FAKE_CFG, FAKE_SSH_ROOT=FAKE_ROOT)
    hwnd = launch(env)
    if not hwnd:
        print("FAIL: no window")
        return 1
    t = title(hwnd)
    check(t == "Tinta - plain.md", "session restored a remote tab", t)

    # Local row in the recent-documents menu (first section)
    click(hwnd, 1200 - 3 * 46 - 34 - 17, 20)
    time.sleep(0.5)
    click(hwnd, 700, 87)
    time.sleep(1.2)
    t = title(hwnd)
    check(t == "Tinta - 本地样例.md", "menu opened a local recent file", t)

    # Browser: navigate into the Chinese-named subdirectory and open its file
    click(hwnd, 20, 20)
    click(hwnd, 120, 121)
    time.sleep(1.2)
    click(hwnd, 400, 210)          # row 0 = 子目录
    time.sleep(1.2)
    click(hwnd, 400, 210)          # the only entry inside: 深层.md
    click(hwnd, 1000, 704)         # Open
    time.sleep(1.8)
    t = title(hwnd)
    check(t == "Tinta - 深层.md", "browser navigated into a CJK directory", t)

    user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(1.5)
    kill_all()
    print("failures:", len(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
