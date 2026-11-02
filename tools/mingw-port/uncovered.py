"""List the uncovered lines of one source file from the coverage build.

Usage:
    python tools/mingw-port/uncovered.py settings.cpp [build dir]

Prints every executable line gcov recorded as never executed, in source
order, with its text, so tests can be written against the real gaps
instead of guesses.
"""

import os
import subprocess
import sys

BUILD = (sys.argv[2] if len(sys.argv) > 2 else r"C:\Users\sch\tinta-cov\build")
SOURCE = sys.argv[1] if len(sys.argv) > 1 else "settings.cpp"
GCOV = os.environ.get("GCOV", r"D:\Qt\Tools\mingw1310_64\bin\gcov.exe")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MIRROR = r"C:\Users\sch\tinta-cov\src"


def find_gcda(name):
    hits = []
    for dirpath, _dirs, files in os.walk(BUILD):
        if "gcov-out" in dirpath:
            continue
        for f in files:
            if f == name + ".gcda":
                hits.append(os.path.join(dirpath, f))
    return hits


def main():
    gcda = find_gcda(SOURCE)
    if not gcda:
        print("no gcda for", SOURCE)
        return 1
    # A line is covered when any binary that instrumented it executed it,
    # so the gap is executable minus covered across every .gcda.
    executable = set()
    covered = set()
    for path in gcda:
        proc = subprocess.run([GCOV, "-b", "-t", path],
                              cwd=os.path.dirname(path), capture_output=True,
                              text=True, errors="replace")
        in_file = False
        for line in proc.stdout.splitlines():
            parts = line.split(":", 2)
            if len(parts) < 3:
                continue
            count, lineno, rest = parts[0].strip(), parts[1].strip(), parts[2]
            if lineno == "0":
                if rest.startswith("Source:"):
                    in_file = os.path.basename(rest.strip()) == SOURCE
                continue
            if not in_file or not lineno.isdigit():
                continue
            if count == "-":
                continue
            number = int(lineno)
            executable.add(number)
            if count != "#####" and not count.startswith("====="):
                covered.add(number)
    uncovered = executable - covered
    if not uncovered:
        print(SOURCE + ": every executable line is covered")
        return 0
    lines = []
    for root in (MIRROR, ROOT):
        candidate = os.path.join(root, "src", SOURCE)
        if os.path.isfile(candidate):
            with open(candidate, encoding="utf-8", errors="replace") as handle:
                lines = handle.read().splitlines()
            break
    print(f"{SOURCE}: {len(uncovered)} uncovered lines")
    for number in sorted(uncovered):
        text = lines[number - 1].strip() if 0 < number <= len(lines) else ""
        print(f"{number:5d}  {text}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
