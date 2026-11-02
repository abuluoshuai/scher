"""Verify the reading-progress ring without screenshots.

The workstation desktop is locked (no screen grab possible), so this
drives the app by posted messages and asserts on two observable outputs:

  * %TEMP%\\tinta-progress-probe.txt -- the temporary probe in
    drawProgressRing logs every ring draw (x, y, radius, progress), so
    the render path, per-tab values and strip placement are verifiable.
  * build/settings.ini [Positions] -- pos=scrollY|zoom|progress|path
    rows show what persisted on switch/exit, including manual marks.

Run 1, top strip: b-long seeded at 0.4 (parse + high-water), a-short
completes on activation, c-mid marked read from the menu, b-long
scrolled to the end.

Run 2, left rail: a-short seeded at 1.0, b-long at 0.3, then a-short is
marked unread from the menu (proves assignment beats the high-water
merge).
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
EXE = os.path.join(BUILD, "scher.exe")
INI = os.path.join(BUILD, "settings.ini")
DOCS = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"),
                    "tinta-progress", "docs")
PROBE = os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"),
                     "tinta-progress-probe.txt")

WM_LBUTTONDOWN, WM_LBUTTONUP = 0x0201, 0x0202
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
WM_MOUSEMOVE = 0x0200
WM_MOUSEWHEEL = 0x020A
WM_CLOSE = 0x0010


def doc(name):
    return os.path.join(DOCS, name)


def procs():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq scher.exe",
                          "/FO", "CSV", "/NH"],
                         capture_output=True, text=True,
                         errors="replace").stdout
    return [l for l in out.splitlines() if l.lower().startswith('"scher.exe"')]


def kill_all():
    for _ in range(20):
        if not procs():
            break
        subprocess.run(["taskkill", "/IM", "scher.exe", "/F"],
                       capture_output=True)
        time.sleep(0.3)


def write_docs():
    os.makedirs(DOCS, exist_ok=True)
    with open(doc("a-short.md"), "w", encoding="utf-8") as f:
        f.write("# 短文档\n\n一屏就能读完的小文档。\n\n- 项目一\n- 项目二\n\n完。\n")
    with open(doc("b-long.md"), "w", encoding="utf-8") as f:
        f.write("# 长文档\n\n")
        for i in range(400):
            f.write("第 %d 段。这是一段用来把文档撑长的正文内容，行数足够多才能让阅读进度环有意义。\n\n" % i)
    with open(doc("c-mid.md"), "w", encoding="utf-8") as f:
        f.write("# 中文档\n\n")
        for i in range(60):
            f.write("中等长度文档的第 %d 行。\n\n" % i)


def write_ini(tabs_on_left, positions, label):
    lines = [
        "[Settings]",
        "themeIndex=5",
        "openInTabs=1",
        "hasAskedFileAssociation=1",
        "windowX=80",
        "windowY=80",
        "windowWidth=1200",
        "windowHeight=820",
        "sessionActive=1",
        "tab=" + doc("a-short.md"),
        "tab=" + doc("b-long.md"),
        "tab=" + doc("c-mid.md"),
        "tabsOnLeft=" + ("1" if tabs_on_left else "0"),
        "[Positions]",
    ]
    for path, prog in positions:
        lines.append("pos=0|0|%.4f|%s" % (prog, path))
    lines.append("")
    with open(INI, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("=== ini written (%s) ===" % label)
    show_positions()


def show_positions():
    if not os.path.exists(INI):
        print("   (no settings.ini)")
        return
    with io.open(INI, "r", encoding="utf-8", errors="replace") as f:
        rows = [l.rstrip() for l in f if l.startswith("pos=")]
    if not rows:
        print("   (no pos rows)")
    for r in rows:
        print("   ", r)


def hwnd_of():
    pids = set()
    for line in procs():
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


def wait_window(timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = hwnd_of()
        if h:
            time.sleep(2.0)
            return h
        time.sleep(0.4)
    return None


def click(hwnd, x, y, right=False):
    lp = (y << 16) | (x & 0xFFFF)
    user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp)
    time.sleep(0.1)
    if right:
        user32.PostMessageW(hwnd, WM_RBUTTONDOWN, 0x0002, lp)
        time.sleep(0.1)
        user32.PostMessageW(hwnd, WM_RBUTTONUP, 0, lp)
    else:
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, 1, lp)
        time.sleep(0.08)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    time.sleep(0.5)


def wheel(hwnd, cx, cy, times):
    pt = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    sp = ((pt.y + cy) << 16) | ((pt.x + cx) & 0xFFFF)
    for _ in range(times):
        user32.PostMessageW(hwnd, WM_MOUSEWHEEL, 0xFF880000, sp)
        time.sleep(0.015)
    time.sleep(1.2)


def wiggle(hwnd, points):
    """Hover moves force repaints (the locked desktop swallows some)."""
    for x, y in points:
        user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, (y << 16) | (x & 0xFFFF))
        time.sleep(0.15)
    time.sleep(0.4)


def probe_lines():
    if not os.path.exists(PROBE):
        return []
    with io.open(PROBE, "r", encoding="utf-8", errors="replace") as f:
        return [l.strip() for l in f if l.strip()]


def snapshot(from_line, label):
    lines = probe_lines()
    fresh = lines[from_line:]
    uniq = []
    for l in fresh:
        if not uniq or uniq[-1] != l:
            uniq.append(l)
    print("--- rings [%s] (+%d draws, %d value changes) ---"
          % (label, len(fresh), len(uniq)))
    for l in uniq[-10:]:
        print("   ", l)
    return len(lines)


def main():
    write_docs()

    # ---------------- run 1: top strip ----------------
    write_ini(False, [(doc("b-long.md"), 0.4)], "run 1 seed")
    if os.path.exists(PROBE):
        os.remove(PROBE)
    kill_all()
    subprocess.Popen([EXE], cwd=BUILD)
    hwnd = wait_window()
    if not hwnd:
        print("FAIL: no window (run 1)")
        return 1
    print("hwnd", hwnd, "scale", (user32.GetDpiForWindow(hwnd) or 96) / 96.0)
    n = snapshot(0, "initial: b-long seeded 0.400")

    click(hwnd, 135, 20)                # a-short
    n = snapshot(n, "a-short active (fully visible -> 1.000)")

    click(hwnd, 519, 20, right=True)    # c-mid menu
    click(hwnd, 519 + 117, 20 + 21)     # item 1: mark as read
    wiggle(hwnd, [(200, 20), (300, 20), (400, 20), (519, 20)])
    n = snapshot(n, "c-mid marked read")

    click(hwnd, 327, 20)                # b-long
    wheel(hwnd, 600, 400, times=220)    # to the end
    wiggle(hwnd, [(200, 20), (327, 20), (400, 20)])
    n = snapshot(n, "b-long scrolled to the end")

    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    time.sleep(2.5)
    kill_all()
    print("=== positions after run 1 ===")
    show_positions()

    # ---------------- run 2: left rail, mark unread ----------------
    write_ini(True, [(doc("a-short.md"), 1.0), (doc("b-long.md"), 0.3)],
              "run 2 seed")
    if os.path.exists(PROBE):
        os.remove(PROBE)
    kill_all()
    subprocess.Popen([EXE], cwd=BUILD)
    hwnd = wait_window()
    if not hwnd:
        print("FAIL: no window (run 2)")
        return 1
    n = snapshot(0, "left rail: a-short 1.000, b-long 0.300, c-mid 0.000")

    click(hwnd, 130, 57, right=True)     # a-short row menu
    click(hwnd, 130, 57 + 51)            # item 2: mark as unread
    wiggle(hwnd, [(130, 57), (130, 90), (130, 124)])
    n = snapshot(n, "a-short marked unread")

    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    time.sleep(2.5)
    kill_all()
    print("=== positions after run 2 ===")
    show_positions()
    return 0


if __name__ == "__main__":
    sys.exit(main())
