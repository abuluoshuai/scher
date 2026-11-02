# Wide table overlap (#239)

Issue: <https://github.com/oipoistar/tinta/issues/239>

## Cause and fix

Inline code was always laid out as one unbreakable span. Table columns could
shrink below the span's natural width, but the span and its background still
painted at full width, covering neighboring cells. Headers generally contain
ordinary text, so they wrapped correctly.

Oversized inline code now uses the existing Unicode line-breaking and DirectWrite
cluster measurements. Each wrapped line retains the code font, color, background,
link target, and selectable source offsets. Table height measurement uses the same
path, so rows grow to contain all lines. Fitting inline code remains atomic.
Emergency breaks preserve surrogate pairs and combining clusters.

Fit-to-width now reserves the existing minimum column width before distributing
the remaining space. If even those minimums exceed the viewport, the table can
scroll horizontally instead of giving a cell zero or negative text width.

## Reproduction and automated checks

`fixtures/wide-table-239.md` reconstructs the reported pattern from the screenshot
and description: thirteen columns, two data rows, long identifiers and dates,
Chinese/Japanese values, and inline code. The reporter's original Markdown was
not attached. A second table covers left/center/right alignment, linked and bold
code, emoji and combining characters. Headings, a quote, emphasis, lists, a task,
links, inline math and fenced code check the surrounding document.

Run the native regression:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The `wide_table_layout` test covers Paper/Midnight, widths 650/1050/1875,
text scales 100%/150%, and automatic/fit table sizing: 24 combinations. It checks
actual DirectWrite text extents and retained code backgrounds against cell
borders, wrapped row heights, link bounds, point-to-source selection, Unicode
cluster boundaries, complete selectable identifiers/dates, unchanged TSV, and
content following the tables. The pre-fix renderer failed the overlap checks.

To retain native table PNGs for inspection:

```powershell
$env:TINTA_TABLE_RENDER_DIR = "$PWD/out/issue-239/after"
ctest --test-dir build -C Release -R '^wide_table_layout$' --output-on-failure
Remove-Item Env:TINTA_TABLE_RENDER_DIR
```

Export verification (Python with PyMuPDF):

```powershell
python tests/validate_wide_table.py
```

## Validation performed

- All 24 native table combinations passed, including glyph bounds, selection,
  links and TSV checks. Native PNGs were generated for every combination.
- Inspected the pre-fix and fixed 1875px Paper output, the 650px Midnight fit
  output, and the 650px/150% Midnight automatic output. Values and code backgrounds
  stay in their cells, and neighboring rows and the following heading remain clear.
  Narrow fit mode can produce tall rows; automatic mode retains more usable
  widths and scrolls when necessary.
- HTML and DOCX preserve both table dimensions (13x3 and 3x3), cell values and
  surrounding content in Paper/Midnight. PDF export produces two pages with all
  checked identifiers, dates, CJK values and the final heading preserved. Inspected
  both PDF pages; native print PNG export also succeeds.
- Compared the existing `table-input-236.md` mixed document against the pre-fix
  executable. HTML and DOCX package contents match exactly. Three of four print
  PNGs match exactly; one differs at three antialiased pixels (maximum channel
  delta 15), within the existing regression harness tolerance.
- Full Release build completed without compiler warnings; all 27 CTests passed.

Generated PNGs, exports and logs are under ignored `out/issue-239/`. Validation
used native offscreen rendering, native hit testing and application export
commands; no interactive desktop session or external document editor was used.
No dependencies or fonts were added to the application.

## Draft reply (not posted)

Thanks for the detailed report and screenshot! I reproduced the overlap with a
matching 13-column CJK table. Long inline-code values were not wrapping when their
columns became narrower. They now wrap inside the cell, with row heights adjusted
to fit. Very narrow tables also keep a minimum column width to prevent overlap.

I've added regression coverage for long IDs, dates, CJK text, links, and exports.
The fix is committed for the next release.
