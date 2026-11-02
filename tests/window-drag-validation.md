# Window dragging and icon menu (#238)

Issue: <https://github.com/oipoistar/tinta/issues/238>

## Behavior

- The icon arms a click on press. Releasing over it opens or closes the existing
  application menu. Small pointer movements stay clicks.
- Crossing Windows' DPI-aware drag threshold releases capture and requests native
  caption dragging at the original screen grab point. The drag does not open the
  application menu or start document selection.
- Escape, capture loss and cancel mode discard an unfinished icon press. Modal
  confirmations continue to block the icon, and F10 still operates the menu.
- The title strip reserves 32 logical pixels beside its tab controls. At extreme
  widths it reduces that gap enough to preserve the title's context target and
  controls. Overflow uses the existing switcher and keeps the active tab visible.
  A detached single-tab window has no redundant overflow button.
- Insertion and reordering use absolute tab indices even when leading tabs are
  hidden. The document preview retains its normal selection and link behavior.

## Fixture and automated verification

`fixtures/window-drag-238.md` combines the window-interaction checklist with
headings, a table, emphasis, inline code, a link, a quote, a task/list, inline math
and fenced code.

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The `window_drag_and_title_space` regression exercises the real input handlers
and native title-strip renderer in a hidden test window. Its window procedure
intercepts the native move request rather than entering Windows' interactive
move loop. It verifies clicks, jitter, threshold crossings in four directions,
the original screen grab point, capture cleanup, cancellation, modal guards,
F10, and unchanged unsaved text/cursor.

Geometry checks cover Paper/Midnight, 100%/150%/200% scales, 325/500/650/1050
logical-pixel window widths, reading/split-editor modes, 1/3/12 tabs and active
tabs at the beginning/middle/end. They verify the available drag space, reachable
controls and active tab, overflow access and drop insertion indices. The mixed
Markdown is laid out after each gesture set.

The existing `copy_file_path` regression caught a title/context-target collision
at 650 physical pixels and 200% scale. Keeping space for those controls and
removing the one-tab overflow chevron fixed it; its original assertions remain.
The existing tab-drop tests also pass with the visible-tab overflow behavior.

The final Release build completed without compiler warnings and all 28 CTests
passed. The existing editor-context clipboard test read the previous clipboard
value once; it passed both an isolated rerun and the final full suite without
changes to that test or its assertions.

Export comparison:

```powershell
python tests/validate_table_input.py `
  --baseline out/issue-238/baseline-tinta.exe `
  --fixture tests/fixtures/window-drag-238.md `
  --output out/issue-238/exports
```

Paper/Midnight HTML, DOCX package members and all four native print PNGs match the
pre-change executable exactly. PDF generation succeeds. Both Paper print pages
were inspected: the table, math, quote, list, code and following heading remain
intact. Generated files and build/test logs stay under ignored `out/issue-238/`.

## Desktop checks and remaining limitation

Before the user stopped Computer Use, a separate Paper test instance at 650px
with eight tabs was inspected. The menu opened on an icon click, the active tab
and overflow control remained visible, and dragging the reserved title area
moved the actual window by the requested 100x80 pixels.

The first icon-drag desktop check dismissed the menu but did not move the window.
The handoff was then changed to preserve the original grab point. The rebuilt
native tests verify that corrected handoff. Live verification of the updated
icon drag, including maximized restore/snapping, remains pending because Computer
Use was stopped. No further desktop interaction was performed for this commit.

## Draft reply (not posted)

Thanks for the suggestions! I'm implementing the first two: clicking the icon
will open the menu, while dragging it will move the window. I'm also reserving
some empty space beside the tab controls so the window remains easy to drag when
tabs are crowded.

These changes are still being tested and are planned for a future update.
Dragging from the document preview would interfere with text selection and other
interactions, so I'll keep that behavior unchanged.
