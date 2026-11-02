"""UI test against the real WSL sshd (no fake ssh, no config override).

Row positions are derived from the server's own listing, replicating the
browser's sort (directories first, then case-insensitive names), so the
clicks stay correct regardless of how many hidden entries the home
directory holds.
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
EXE = os.path.join(BUILD, "tinta.exe")
INI = os.path.join(BUILD, "settings.ini")
SSH = r"C:\Windows\System32\OpenSSH\ssh.exe"
KNOWN = os.path.join(os.environ["TEMP"], "tinta_test_known_hosts")
KEY = r"C:\Users\sch\.ssh\id_ed25519"
DIR_NAME = "测试目录"

failures = []


def check(ok, what, extra=""):
    print(("PASS " if ok else "FAIL ") + what + (" " + extra if extra else ""))
    if not ok:
        failures.append(what)


def remote_ls(path):
    """Listing as the app sees it: dirs first, then case-insensitive name."""
    cmd = [SSH, "-o", "BatchMode=yes", "-o",
           "UserKnownHostsFile=" + KNOWN, "-i", KEY, "testbox",
           "LC_ALL=C.UTF-8 ls -1Ap -- " + ("'" + path + "'" if path else '"$HOME"')]
    out = subprocess.run(cmd, capture_output=True).stdout.decode("utf-8", "replace")
    rows = []
    for line in out.splitlines():
        line = line.strip("\r")
        if not line or line in (".", ".."):
            continue
        is_dir = line.endswith("/")
        rows.append((line.rstrip("/"), is_dir))
    rows.sort(key=lambda r: (not r[1], r[0].lower()))
    return rows


def row_of(rows, name):
    for i, (n, _d) in enumerate(rows):
        if n == name:
            return i
    return -1


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
    time.sleep(0.5)


def row_y(index):
    # list starts at y=197 with 26 px rows at a 1200x820 window
    return 210 + 26 * index


def wheel(hwnd, notches, cx=400, cy=400):
    """One notch scrolls dpi(46) px in the browser list."""
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    sp = ((pt.y + cy) << 16) | ((pt.x + cx) & 0xFFFF)
    delta = 0xFF88_0000 if notches > 0 else 0x0078_0000   # -120 / +120
    for _ in range(abs(notches)):
        user32.PostMessageW(hwnd, 0x020A, delta, sp)
        time.sleep(0.05)
    time.sleep(0.5)


def click_row(hwnd, index):
    """Click a list row, scrolling it into view first when it is below."""
    y = row_y(index)
    if y > 640:
        notches = -(-(y - 640) // 46)
        wheel(hwnd, notches)
        y -= notches * 46
    click(hwnd, 400, y)


def main():
    kill_all()
    io.open(INI, "w", encoding="utf-8").write(
        "[Settings]\nthemeIndex=5\nhasAskedFileAssociation=1\nwindowX=80\n"
        "windowY=80\nwindowWidth=1200\nwindowHeight=820\n")

    home = remote_ls("")
    dir_index = row_of(home, DIR_NAME)
    check(dir_index >= 0, "server listing contains the CJK directory",
          str(dir_index))

    env = dict(os.environ)
    env.pop("TINTA_SSH_EXE", None)
    env.pop("TINTA_SSH_CONFIG", None)
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
    time.sleep(2.5)

    # Application menu -> Open remote file (real ssh, ~/.ssh/config host)
    click(hwnd, 20, 20)
    click(hwnd, 120, 121)
    time.sleep(2.0)

    # Into the Chinese-named directory
    click_row(hwnd, dir_index)
    time.sleep(1.5)

    inner = remote_ls("/home/sch/" + DIR_NAME)
    note_i = row_of(inner, "笔记 一.md")
    plain_i = row_of(inner, "plain.md")
    check(note_i >= 0 and plain_i >= 0, "inner listing parsed", str(inner))

    click_row(hwnd, plain_i)
    click_row(hwnd, note_i)
    click(hwnd, 1000, 704)          # Open
    time.sleep(2.5)
    t = title(hwnd)
    check(t.startswith("Tinta - "), "real-ssh multi-select opened tabs", t)

    stored = io.open(INI, encoding="utf-8", errors="replace").read()
    remotes = [l for l in stored.splitlines() if l.startswith("remoteRecent=")]
    check(len(remotes) == 2, "both remote documents recorded", str(len(remotes)))
    check(any("笔记 一.md" in r for r in remotes),
          "CJK name recorded intact over real ssh")

    # Read-only guard
    user32.PostMessageW(hwnd, 0x0100, 0x11, 0)
    user32.PostMessageW(hwnd, 0x0100, 0x45, 0)
    user32.PostMessageW(hwnd, 0x0101, 0x45, 0)
    user32.PostMessageW(hwnd, 0x0101, 0x11, 0)
    time.sleep(0.8)
    check(title(hwnd) == t, "Ctrl+E stayed read-only", title(hwnd))

    user32.PostMessageW(hwnd, 0x0010, 0, 0)
    time.sleep(1.5)
    kill_all()
    print("failures:", len(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
