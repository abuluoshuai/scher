# Window dragging and the icon menu

Open this document in a normal and a narrow window. A click on the top-left icon
opens the menu; dragging that icon moves the window. The empty space beside the
tab controls remains available for dragging with one tab or many tabs.

## Reading and editing

This **bold text**, *emphasis*, and `inline code` should remain selectable.
Try selecting this paragraph in the rendered preview after moving the window.

| Check | Expected result |
| --- | --- |
| Click the icon | Menu opens on release |
| Drag the icon | Window moves without opening the menu |
| Drag the empty title area | Window moves |
| Many tabs | Active tab and overflow switcher remain accessible |
| Inline math | $a^2+b^2=c^2$ |

## Surrounding content

> Moving the window should not change this quote or the document's selection.

- [A regular link](https://example.com)
- [ ] A task below the table
- Ordinary list text

```cpp
// Fenced code remains separate from title-bar interactions.
const int unchanged = 238;
```

## Final heading

Switch between reading mode and the split editor, open several copies as tabs,
and check the menu, tab selection, reordering, and overflow switcher.
