#!/usr/bin/env python3
"""MinGW/libstdc++ compatibility patch.

MSVC accepts `std::ofstream(std::wstring)` / `std::ifstream(std::wstring)` as a
private extension. libstdc++ does not: it tries the C++17
`basic_fstream(const std::filesystem::path&)` template, which then fails on
`std::wstring::make_preferred()`. mingw-w64's libstdc++ does provide the wide
overloads taking `const wchar_t*`, so appending `.c_str()` restores the same
behaviour the upstream code gets from MSVC.

Each entry below is a `path:line` reported by the compiler. Re-run after a
build to confirm nothing is left, and drop this script once the port is done.
"""

import io
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))

SITES = """src/annotations.cpp:180
src/drafts.cpp:42
src/drafts.cpp:131
src/editor.cpp:1500
src/editor.cpp:1588
src/editor.cpp:1931
src/editor.cpp:1957
src/export_docx.cpp:171
src/export_docx.cpp:1531
src/export_docx.cpp:1633
src/export_html.cpp:114
src/export_html.cpp:1454
src/i18n.cpp:1532
src/i18n.cpp:1571
src/input.cpp:252
src/input.cpp:574
src/input.cpp:2561
src/input.cpp:2581
src/input.cpp:4416
src/main_d2d.cpp:2230
src/pandoc.cpp:200
src/print.cpp:943
src/render.cpp:2516
src/settings.cpp:33
src/settings.cpp:262
src/themes.cpp:350
src/themes.cpp:408"""

MARKERS = ("std::ofstream", "std::ifstream", "std::fstream")


def patch_site(text, line_no):
    """Append `.c_str()` to the first constructor argument on `line_no` (1-based)."""
    start = 0
    for _ in range(line_no - 1):
        start = text.index("\n", start) + 1
    end = text.index("\n", start)
    line = text[start:end]

    marker_at = -1
    for marker in MARKERS:
        at = line.find(marker)
        if at != -1 and (marker_at == -1 or at < marker_at):
            marker_at = at
    if marker_at == -1:
        return None, None
    open_at = text.index("(", start + marker_at)

    depth = 0
    i = open_at
    while i < len(text):
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i, "paren"
        elif ch == "," and depth == 1:
            return i, "comma"
        i += 1
    return None, None


def main():
    grouped = {}
    for entry in SITES.split():
        path, _, lineno = entry.rpartition(":")
        grouped.setdefault(path, []).append(int(lineno))

    total = 0
    for relpath, lines in grouped.items():
        full = os.path.join(ROOT, relpath.replace("/", os.sep))
        with io.open(full, "r", encoding="utf-8", newline="") as handle:
            text = handle.read()

        # Patch from the last line backwards so earlier offsets stay valid.
        for line_no in sorted(lines, reverse=True):
            at, kind = patch_site(text, line_no)
            if at is None:
                print("SKIP (no fstream call found): %s:%d" % (relpath, line_no))
                continue
            snippet = text[max(0, at - 40):at]
            if snippet.rstrip().endswith(".c_str()"):
                print("already patched: %s:%d" % (relpath, line_no))
                continue
            text = text[:at] + ".c_str()" + text[at:]
            total += 1

        with io.open(full, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)

    print("patched %d call site(s) across %d file(s)" % (total, len(grouped)))


if __name__ == "__main__":
    sys.exit(main())
