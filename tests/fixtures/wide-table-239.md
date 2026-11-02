# Wide table with CJK content

This regression sample follows issue #239: thirteen columns, two data rows,
inline-code values, long dates, and Chinese/Japanese text.

| Fixture ID | コード | 分類 | 名称 | 説明 | 単価（税込） | LCサイト公開 | 公開範囲（所属） | 公開範囲（グループ） | 公開開始日時 | 公開終了日時 | 计划类别 | 用途 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `FX-SGTL-GOODS-001` | `10001` | `制服` | 名称A（全角100文字） | 説明A（1000文字、3行） | `1000` | `オン` | `S001 ブレザー` 同一值（准备时读回并设置同一值） | `S001 ブレザー` 同一值（未设置则保持未设置） | `2026/04/01 00:00` | `2030/03/31 23:59` | `预置` | 上限值文本的保存后再显示（`SGTL-C02`）、一览显示（`SGTL-C03`）、LC一览／详细显示（`SGTL-C06`） |
| `FX-SGTL-GOODS-002` | `10002` | `制服` | `S001 ブレザー` | `准备时任一览读回` | `准备时任一览读回` | `オン` | `已在 LC 显示的既存值` | `已在 LC 显示的既存值` | `准备时任一览读回` | `准备时任一览读回` | `试验复用` | 短文侧的对照行（布局、检索、导出的比较对象） |

## Aligned cells and links

| Left | Center | Right |
| :--- | :---: | ---: |
| **`ABCDEFGHIJKLMNO0123456789ABCDEFGHIJKLMNO`** | [Long `linked_code_value_0123456789_ABCDEFGHIJKLMNO`](https://example.com) | `中文😀é全角非常长的测试字符列测试测试测试测试测试测试测试测试测试测试` |
| Short `code` after text | `1000` | Plain end |

## Surrounding content

> A quote with **bold**, *emphasis*, `inline code`, and $a^2+b^2=c^2$.

- A list item with [a regular link](https://example.com).
- [ ] A task stays below the table.

```cpp
// Code blocks retain their own horizontal scrolling behavior.
const char* sample = "unchanged";
```

Normal inline code stays on one line when it fits: `short code`.

## Final heading

This paragraph must remain below both tables without overlapping their last rows.
