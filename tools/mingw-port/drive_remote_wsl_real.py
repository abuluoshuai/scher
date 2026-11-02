"""Real-WSL check of the remote browser wheel fix.

Opens the remote browser through the document context menu (no hover timers
involved), sends real WM_MOUSEWHEEL messages with screen coordinates - the
form Windows actually delivers - and proves the overlay survived by clicking
a row afterwards: a title change means the click reached a browser row rather
than dead page space.
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

BUILD = r"E:/WorkBuddy/翠花工具包/Markdown阅读器/build"
EXE = os.path.join(BUILD, "scher.exe")
INI = os.path.join(BUILD, "settings.ini")

failures = 0


def check(ok, what, detail=""):
    global failures
    print(("PASS " if ok else "FAIL ") + what + ((" | " + str(detail)) if detail else ""))
    if not ok:
        failures += 1
    return ok


def kill():
    subprocess.run(["taskkill", "/IM", "scher.exe", "/F"], capture_output=True)
    time.sleep(1.2)


def window():
    pids = set()
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq scher.exe", "/FO", "CSV", "/NH"],
                         capture_output=True).stdout.decode("gbk", "replace")
    for line in out.splitlines():
        c = line.strip()
        if c.startswith('"scher'):
            pids.add(int(c.split('","')[1].strip('"')))
    hits = []
    W = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def visit(h, _):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(h):
            r = wt.RECT()
            if user32.GetWindowRect(h, ctypes.byref(r)):
                area = (r.right - r.left) * (r.bottom - r.top)
                if area > 50000:
                    hits.append((area, h))
        return True

    user32.EnumWindows(W(visit), 0)
    hits.sort(reverse=True)
    return hits[0][1] if hits else None


def title(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def click(hwnd, x, y, right=False):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    user32.PostMessageW(hwnd, 0x0200, 0, lp)
    time.sleep(0.06)
    down, up = (0x0204, 0x0205) if right else (0x0201, 0x0202)
    user32.PostMessageW(hwnd, down, 1, lp)
    time.sleep(0.05)
    user32.PostMessageW(hwnd, up, 0, lp)
    time.sleep(0.45)


def wheel(hwnd, cx, cy, notches):
    p = wt.POINT(int(cx), int(cy))
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    wparam = ((-120 * abs(notches)) & 0xFFFF) << 16
    lparam = ((p.y & 0xFFFF) << 16) | (p.x & 0xFFFF)
    user32.PostMessageW(hwnd, 0x020A, wparam, lparam)
    time.sleep(0.12)


def client_size(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    return r.right - r.left, r.bottom - r.top


def main():
    kill()
    with open(INI, "w", encoding="utf-8") as f:
        f.write("[Settings]\nthemeIndex=5\nhasAskedFileAssociation=1\n"
                "windowX=80\nwindowY=80\nwindowWidth=1200\nwindowHeight=820\n")
    subprocess.Popen([EXE, "ssh://wsl/~/scher-test/远程测试.md"], cwd=BUILD)
    time.sleep(6)
    hwnd = window()
    if not check(hwnd is not None, "the app window is up"):
        return 1
    cw, ch = client_size(hwnd)
    t0 = title(hwnd)
    check(t0 == "Scher - 远程测试.md",
          "a remote document opened over real ssh (~ expanded)", t0)

    cx, cy = cw // 2, ch // 2
    # Context menu: "Open remote file..." is the 8th document entry.
    for menu_y in (540, 530, 550, 520, 560, 510, 570):
        click(hwnd, 400, 300, right=True)          # document context menu
        click(hwnd, 450, menu_y)                   # Open remote file...
        time.sleep(3.2)                            # real ssh listing
        before = title(hwnd)
        for _ in range(6):
            wheel(hwnd, cx, cy, -1)                # the regression
        time.sleep(0.5)
        click(hwnd, cx - 300, 210)                 # a row in the browser
        time.sleep(2.5)
        after = title(hwnd)
        if after != before and after.startswith("Scher - "):
            check(True,
                  "the browser survived wheel scrolling and a row opened",
                  "menu_y=%d -> %s" % (menu_y, after))
            kill()
            print("failures: %d" % failures)
            return 0 if failures == 0 else 1

    check(False, "the remote browser could not be driven open", title(hwnd))
    kill()
    print("failures: %d" % failures)
    return 1


if __name__ == "__main__":
    sys.exit(main())
