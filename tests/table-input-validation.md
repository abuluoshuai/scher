# Shortcut modifiers and rendered table input (#235, #236)

Validated on Windows with the Release build on 2026-09-18.

## Fixtures and automated checks

`fixtures/table-input-236.md` reproduces the reported Chinese table, including
empty cells, wrapped text, Unicode, and escaped pipes. It also includes a second
table, headings, lists, tasks, emphasis, a link, a quote, code, and inline math.

Run from the repository root:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
python tests/validate_table_input.py --baseline ../tinta-3.7.2.exe
```

- Full Release build succeeded; no compiler warnings or errors were reported.
- All 26 CTest tests passed, including the new `table_and_modifier_input` test
  and the existing shortcut, search focus, selection, layout, parser, tab,
  frontmatter, image, localization, and configuration regressions.
- Native input tests exercise the public mouse and keyboard handlers with
  DirectWrite layouts in Paper and Midnight at widths of 650 and 1050 pixels.
  Checks cover caret placement, drag/Shift selection, copy/cut/paste, empty-cell
  paste, cell undo, Unicode cluster deletion, wrapped caret visibility, Tab,
  cancel, escaped pipes, atomic document undo, and transfer of focus to Find.
- Modifier tests simulate AltGr as Ctrl+Alt and verify that text reaches both
  the document and Find without invoking Ctrl shortcuts. They also check
  Ctrl+Shift exclusions and supported selection/preview shortcuts.
- Live reading preview checks verify unsaved content, unchanged disk contents,
  disabled typing into the hidden source pane, preserved caret and undo,
  return-to-edit without inserting the shortcut character, tab parking,
  close confirmation, and saving while reading.
- Export checks against published v3.7.2 passed in both themes: HTML bytes and
  all DOCX archive members matched; all four rendered page images had zero
  changed pixels; PDF files were produced with valid PDF headers. The fixture
  on disk remained unchanged. PDF content equality was not asserted.

## Desktop check and limits

The built application displayed the mixed fixture and its split editor. A
click in an empty table cell showed a visible insertion caret, and typed text
appeared in that cell. Desktop testing stopped when the user pressed Escape;
clipboard selection and full reading preview were subsequently covered by
the native automated checks, not a completed manual desktop walkthrough.

AltGr modifier routing was tested with simulated keyboard state, not every
physical keyboard layout. The tests check IME caret geometry and Unicode text,
but do not constitute an end-to-end IME composition test.

Generated logs, images, and exports are kept under ignored `out/` directories.

## Draft replies (not posted)

### #235

Thanks for reporting this. I fixed the modifier handling so AltGr/Ctrl+Alt
typing no longer triggers Ctrl shortcuts, and unintended Ctrl+Shift variants
are ignored. Explicit shortcuts such as Save As and word selection still
work. Regression tests cover AltGr input in the editor, table cells, and Find.
The fix is committed for the next release.

### #236

Thanks for the detailed report and screenshot. I fixed caret placement and
selection in rendered table cells, added copy/cut/paste and undo support, and
corrected the caret position for wrapped text. You can also use the Read button
or Ctrl+Shift+E to preview unsaved edits at full width, then return to editing
without losing them. Closing a document with unsaved changes still asks for
confirmation. These fixes are committed for the next release.
