# Local PDF links (#237)

Report and original patch: https://github.com/oipoistar/tinta/issues/237 by
Marat (@msaitov). The extension gate, binary-file preview/creation guards, and
initial regression cases are adopted from their patch.

## Behavior

Open [the mixed sample](fixtures/pdf-links/docs/index.md). Its PDF links resolve
against the Markdown document's folder and open in the registered application.
Two small, valid one-page PDFs are included, with spaces and Cyrillic/CJK/accented
characters in their filenames. No PDF rendering library is added to Tinta.

- Existing PDF links get the live-file appearance and skip text hover previews.
- Missing PDFs show a notification and never offer to create a text file.
- Failed external-file launches show a notification with the filename; the
  notification history retains the full path. Missing associations have their
  own message. Windows error dialogs are suppressed in favor of Tinta's messages.
- Markdown links still open in Tinta, text-file links still use their registered
  application, and missing text files still offer creation.
- Web URLs are unchanged. Local PDF fragments such as `report.pdf#page=3` retain
  their previous behavior; this patch does not add PDF page navigation.

The launcher uses the documented Unicode
[ShellExecuteExW API](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecuteexw)
to obtain launch errors, with `SEE_MASK_FLAG_NO_UI` and `SEE_MASK_NOASYNC`.

## Checks

```powershell
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python tests/validate_pdf_links.py
```

`file_fragment_navigation` covers absolute and relative PDFs with spaces and
Unicode, uppercase extensions, both mouse-release paths for missing links,
hover suppression using a rendered link rectangle, and an actual Windows shell
failure after deleting a previously live target. It verifies that this failure
reports the Unicode path and that PDFs never create a document tab. Existing
Markdown navigation, hover previews, text links and remote URLs are checked too.
The mixed fixture runs at 650 and 1050 pixels in Paper and Midnight.

The export harness checks HTML, DOCX, PDF and page PNG output in both themes,
including preserved link targets, headings, a table, quote, lists, emphasis,
inline math and code. Generated artifacts remain under ignored `out/issue-237/`.

## Validation record (2026-09-18)

- Focused native PDF/navigation regressions passed.
- Both sample PDFs were opened and parsed with PyMuPDF: one page each, expected
  text present. Their binary Git attributes preserve PDF cross-reference offsets.
- Export checks passed in both themes: four PNG pages total, original PDF and
  Markdown link targets preserved in HTML/DOCX, mixed content retained, and PDFs
  generated successfully. Markdown fixture contents remained unchanged.
- Visually inspected the native Paper print rendering, including the live/missing
  link appearance, table, quote, list, code, and inline math.
- Full Release build completed without compiler warnings; all 26 CTests passed.
  CMake emitted its existing md4c minimum-version deprecation warning during
  configuration.
- The first full run exposed a CRLF assumption in the #236 table test's expected
  fixture. Reading that expectation in text mode now matches the editor's newline
  normalization on fresh Windows checkouts. This test-only correction is committed
  separately. The existing clipboard-copy check failed once, then passed both its
  isolated rerun and the final full suite without a code change to that check.

The automated tests deliberately do not launch the user's default PDF viewer.
They cover path resolution and real shell failure handling; successful opening
in an external viewer was manually verified by the reporter for their original
patch. No desktop interaction or registry/association changes were made here.

## Draft reply (not posted)

Thanks for the detailed diagnosis and patch, Marat! I've adopted it and credited
you as a co-author. Local PDF links now resolve relative to the Markdown file
and use Unicode paths when opening the default application. PDFs also skip text
previews and the missing-file creation prompt.

I added notifications for missing files and failed launches, and expanded the
regression coverage with spaces, Unicode filenames, mixed Markdown, and exports.
This is committed for the next release. Thanks for contributing!
