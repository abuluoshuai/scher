# Local PDF links

[Open report](<../pdf/report 2026.pdf>)

[Unicode filename](<../pdf/отчёт 中文 café.pdf>)

[Encoded filename](../pdf/report%202026.pdf)

[Missing PDF](<../pdf/missing report.PDF>)

The first three links open a PDF in the default viewer. The missing link
shows a notification and does not offer to create an empty PDF.

## Surrounding content

| Document | Expected behavior |
| --- | --- |
| [PDF in a table](<../pdf/report 2026.pdf>) | **Open externally** |
| [Markdown](notes.md#destination) | Open inside Tinta |
| [Text file](notes.txt) | Open in the default text application |

> A quote with *emphasis*, `inline code`, and $a^2+b^2=c^2$.

- Existing lists still render.
- [Remote PDF](https://example.com/report.pdf) stays a web link.
- [ ] Check an empty or missing target without creating a file.

```markdown
[This example stays literal](<../pdf/report 2026.pdf>)
```

Hover a PDF link: no text preview should appear. Hover the Markdown link:
its preview should still work. Absolute paths and uppercase extensions are
also exercised by the native regression tests.
