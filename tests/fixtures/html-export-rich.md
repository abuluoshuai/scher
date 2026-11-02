---
title: Rich HTML export
author: Tinta tests
tags: [export, html]
---

# Heading level one

## Heading level two

### Heading level three

Entities decode and re-escape: 1 &lt; 2 &amp;&amp; 3 &gt; 2.

A paragraph with **strong text**, *emphasis*, `inline code`, ==highlight==,
~~struck-through~~, H~2~O subscript, E = mc^2^ superscript, a
[link with a title](https://example.com/page "Example title"), a
[wiki link](wiki:Other%20note), and a plain reference to notes.md.

A hard break follows here  
and this line starts after it. A soft
break wraps this one.

> A plain blockquote
> spanning two lines.

> [!NOTE]
> A note alert body.

> [!TIP]
> A tip alert body.

> [!IMPORTANT]
> An important alert body.

> [!WARNING]
> A warning alert body.

> [!CAUTION]
> A caution alert body.

- first bullet
- [ ] unchecked task
- [x] checked task

3. third item
4. fourth item

```c
int main(void) { return 0; }
```

```
a fence with no language
```

```mermaid
flowchart TD
    classDef hot fill:#abc,stroke:#123456,color:#456789,stroke-width:2px
    A[Rectangle] --> B{Diamond}
    B -->|Yes| C([Stadium])
    B -->|No| D((Circle))
    D -.-> E[[Subroutine]]
    E ==> F[(Cylinder)]
    F --> G{{Hexagon}}
    G --> I[/Parallelogram/]
    I --> J[\\Trapezoid\\]
    class A,B hot
    subgraph Group
        M[Inner] --> N[Other]
    end
    J --> M
    linkStyle 0 stroke:#ff0000
```

```mermaid
flowchart LR
    A[Start] --> B[Middle]
    B --> C[End]
    A --> C
    A --> D[Side]
    subgraph Outer
        subgraph Inner
            E[Deep] --> F[Deeper]
        end
        F --> G[Shallow]
    end
    D --> E
    G --> C
```

```mermaid
flowchart BT
    P[Bottom] --> Q[Middle]
    Q --> R[Top]
    P --> R
```

```mermaid
sequenceDiagram
    participant A
    participant B
    A->>B: ping
    B-->>A: pong
    B-)A: async
    A-xB: cross
    A-->B: dotted
    A->B: plain
```

```mermaid
classDiagram
    Animal <|-- Duck : inherits
    Animal *-- Feather
    Animal o-- Pond
    Duck ..> Pond : visits
    class Duck {
        +quack()
    }
```

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Run : start
    Run --> [*]
```

```mermaid
erDiagram
    CUSTOMER ||--o{ ORDER : places
    CUSTOMER {
        string name PK
    }
```

```mermaid
flowchart LR
    W[An extremely long node label that certainly exceeds the wrapping width of the exported box] --> X[Short]
```

```mermaid
pie showData title Split
    "A" : 60
    "B" : 40
```

```mermaid
gitGraph
    commit
    branch dev
    commit
    checkout main
    merge dev
```

```mermaid
gantt
    dateFormat YYYY-MM-DD
    section Build
        Compile :a1, 2026-01-01, 5d
        Test :after a1, 2d
```

```mermaid
journey
    title Day
    section Work
        Code: 5: Me
        Review: 3: Team
```

```mermaid
quadrantChart
    x-axis Low --> High
    y-axis Low --> High
    quadrant-1 Do
    A: [0.3, 0.6]
```

```mermaid
xychart-beta
    x-axis [jan, feb]
    y-axis 0 --> 10
    bar [4, 6]
    line [3, 7]
```

```mermaid
packet-beta
0-15: "Source"
16-31: "Destination"
```

```mermaid
kanban
  Todo
    [Write docs]
  Done
    [Ship]
```

```mermaid
treemap-beta
"Section"
    "Leaf": 12
```

```mermaid
radar-beta
axis a["A"], b["B"], c["C"]
curve one["One"]{80, 60, 70}
max 100
```

```mermaid
sankey-beta
A,B,10
B,C,5
```

```mermaid
block-beta
    columns 2
    a["A"] b["B"]
    a --> b
```

```mermaid
mindmap
  root((center))
    Child
      Leaf
```

```mermaid
timeline
    title History
    2020 : one
    2021 : two
```

```mermaid
C4Context
    Person(user, "User")
    System(sys, "System", "Does things")
    Rel(user, sys, "Uses")
```

```mermaid
requirementDiagram
    requirement R1 {
        text: Must render
    }
    element R2 {
        text: Other
    }
    R1 - satisfies -> R2
```

```mermaid
architecture-beta
    service a(server)[A]
    service b(database)[B]
    a:R -- L:b
```

```mermaid
not a diagram at all
```

Inline math $a^2 + b^2 = c^2$ inside a sentence.

$$
\int_0^1 x \, dx = \frac{1}{2}
$$

Unsupported math follows: $\notarealcommand{$.

| Left | Center | Right |
| :--- | :----: | ----: |
| a    | b      | c     |
| 1    | 2      | 3     |

---

<div class="raw">raw HTML block</div>

<a id="anchor-target"></a>Anchored text follows the anchor.

<div>
<ruby>漢<rt>kan</rt></ruby> is ruby text inside an HTML block.
</div>

![embedded dot](dot.png)

![jpeg](photo.jpg)

![gif](anim.gif)

![webp](picture.webp)

![bmp](scan.bmp)

![svg](vector.svg)

![icon](favicon.ico)

![no extension](asset)

![unknown type](notes.txt)

![second dot](dot2.png)

![padding probe](pad.bin)

![missing file](missing.png)

![remote image](https://example.com/remote.png)

![drive path](C:/nonexistent/thing.png)

Footnote reference[^1] and a second one[^2].

[^1]: The first footnote body.

[^2]: The second footnote body, with a [link](https://example.com).
