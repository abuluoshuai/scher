"""Aggregate gcov line coverage for the MinGW test build.

Why this script exists: lcov is not part of the MinGW toolchain here, and
both .gcda writing and .gcov file naming break when the build path
contains non-ASCII characters (this workspace is "翠花工具包"). The tests
therefore run with GCOV_PREFIX pointing at an ASCII directory and this
script imports that data back, then asks `gcov -t` for annotated source on
stdout (no file names involved) and merges every run per source line.

Usage:
    python tools/mingw-port/coverage.py [build dir] [--logic-only]

--logic-only restricts the totals to the modules that are unit-testable
without a window/Direct2D surface, which is the scope the 95% target is
measured against.
"""

import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
BUILD = ARGS[0] if ARGS else os.path.join(ROOT, "build-test")
# The coverage build is made through an ASCII junction
# (C:/Users/sch/tinta-src) because gcov cannot open sources under a
# non-ASCII path; tell the report which source root the .gcno files record.
SOURCE_ROOT = next((a.split("=", 1)[1] for a in sys.argv[1:]
                    if a.startswith("--root=")), ROOT)
LOGIC_ONLY = "--logic-only" in sys.argv
GCOV = os.environ.get("GCOV", r"D:\Qt\Tools\mingw1310_64\bin\gcov.exe")

LINE_RE = re.compile(r"^\s*([^:]+):\s*(\d+):(.*)$")
SOURCE_PREFIX = "Source:"

# Only the project's own production sources count: the first path segment
# below the source root must be "src" or "include". Test translation units
# inflate the total (they are written to be executed), md4c is vendored under
# third_party/, and a mirror whose root happens to be called "src" must not
# pull tests/ or D:/Qt/.../include/ into the report.
INCLUDE_DIRS = ("src", "include")

# Window/render-bound translation units: they need a Direct2D target and a
# real message loop. The interaction tests (tests/*_tests.cpp) do drive
# them, but their line coverage is dominated by drawing calls.
GUI_TUS = {
    "main_d2d.cpp", "render.cpp", "overlays.cpp", "editor.cpp", "input.cpp",
    "tabs.cpp", "sidepanels.cpp", "signals.cpp", "editrail.cpp",
    "context_menu.cpp", "d2d_init.cpp", "print.cpp", "image_loader.cpp",
    "frontmatter_ui.cpp", "tableedit.cpp", "selection.cpp", "search_panel.cpp",
    "remote_ui.cpp", "startpage.cpp", "annotations.cpp", "search_input.cpp",
    "math_render.cpp", "mermaid_block.cpp", "export_docx.cpp",
}


def import_prefix(build, prefix):
    moved = 0
    for dirpath, _dirs, files in os.walk(prefix):
        for name in files:
            if not name.endswith(".gcda"):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), prefix)
            marker = "build-test" + os.sep
            idx = rel.find(marker)
            if idx < 0:
                continue
            dest = os.path.join(build, rel[idx + len(marker):])
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.exists(dest):
                os.remove(dest)
            shutil.copyfile(os.path.join(dirpath, name), dest)
            moved += 1
    return moved


def find_gcda(base):
    out = []
    for dirpath, _dirs, files in os.walk(base):
        if "gcov-out" in dirpath:
            continue
        for name in files:
            if name.endswith(".gcda"):
                out.append(os.path.join(dirpath, name))
    return out


def parse_gcov_stream(text, covered, executable, prefix_root):
    """Split `gcov -t` stdout into sources and collect their line states."""
    current = None
    for raw in text.splitlines():
        m = LINE_RE.match(raw)
        if not m:
            continue
        count, lineno, rest = m.group(1).strip(), int(m.group(2)), m.group(3)
        if lineno == 0 and rest.startswith(SOURCE_PREFIX):
            path = os.path.normpath(rest[len(SOURCE_PREFIX):]).replace("\\", "/")
            current = path if path.startswith(prefix_root) else None
            continue
        if lineno == 0 or not current:
            continue
        if count == "-":
            continue
        executable[current].add(lineno)
        if count == "#####" or count.startswith("====="):
            continue
        try:
            if int(count) > 0 or count.endswith("*"):
                covered[current].add(lineno)
        except ValueError:
            if "k" in count or "*" in count:
                covered[current].add(lineno)


def main():
    if not os.path.isdir(BUILD):
        print("no build dir:", BUILD)
        return 1
    prefix = os.environ.get("GCOV_PREFIX")
    if prefix and os.path.isdir(prefix):
        moved = import_prefix(BUILD, prefix)
        if moved:
            print(f"imported {moved} .gcda files from {prefix}\n")

    gcda = find_gcda(BUILD)
    if not gcda:
        print("no .gcda found - run the tests first")
        return 1

    covered = defaultdict(set)
    executable = defaultdict(set)
    prefix_root = os.path.normpath(SOURCE_ROOT).replace("\\", "/") + "/"
    for path in gcda:
        proc = subprocess.run([GCOV, "-b", "-t", path],
                              cwd=os.path.dirname(path), capture_output=True,
                              text=True, errors="replace")
        parse_gcov_stream(proc.stdout, covered, executable, prefix_root)

    rows = []
    for src in sorted(executable):
        rel = src[len(prefix_root):] if src.startswith(prefix_root) else src
        top = rel.split("/", 1)[0]
        if top not in ("src", "include"):
            continue
        name = os.path.basename(src)
        if name in GUI_TUS and LOGIC_ONLY:
            continue
        total = len(executable[src])
        hit = len(covered[src] & executable[src])
        rows.append((name, total, hit, 100.0 * hit / total if total else 0.0))

    if not rows:
        print("no source rows parsed")
        return 1

    width = max(len(r[0]) for r in rows)
    print(f"{'file':<{width}}  {'lines':>6} {'hit':>6} {'cover':>7}")
    total_lines = total_hit = 0
    gui_lines = gui_hit = 0
    for name, tot, hit, pct in sorted(rows, key=lambda r: r[3]):
        print(f"{name:<{width}}  {tot:>6} {hit:>6} {pct:>6.1f}%")
        total_lines += tot
        total_hit += hit
        if name in GUI_TUS:
            gui_lines += tot
            gui_hit += hit
    print("-" * (width + 26))
    logic_lines, logic_hit = total_lines - gui_lines, total_hit - gui_hit
    print(f"{'TOTAL':<{width}}  {total_lines:>6} {total_hit:>6} "
          f"{100.0 * total_hit / max(1, total_lines):>6.1f}%   "
          f"(all project sources)")
    if not LOGIC_ONLY:
        print(f"{'logic':<{width}}  {logic_lines:>6} {logic_hit:>6} "
              f"{100.0 * logic_hit / max(1, logic_lines):>6.1f}%   "
              f"(window/Direct2D units excluded)")
    print(f"{'gui':<{width}}  {gui_lines:>6} {gui_hit:>6} "
          f"{100.0 * gui_hit / max(1, gui_lines):>6.1f}%   "
          f"(driven by the interaction tests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
