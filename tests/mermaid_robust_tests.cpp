// Malformed-input sweep for every native diagram family.
//
// A diagram block in a document is untrusted: it is whatever the author
// typed. Every parser is supposed to answer with either a laid-out diagram
// or a readable error string so the reader can fall back to the source, and
// never to crash, hang or return a degenerate extent. This suite walks all
// families through a battery of broken sources (truncated statements,
// unbalanced delimiters, unknown keywords, extreme numbers, bare headers)
// and asserts that contract. The sweeping also reaches the rejection paths
// that a per-family happy-path test never touches.

#include "mermaid_ext.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    failures++;
}

mermaidext::Size fakeMeasure(const std::string& text,
                             const mermaidext::TextStyle&, float) {
    size_t longest = 0, current = 0, lines = text.empty() ? 0 : 1;
    for (char c : text) {
        if (c == '\n') {
            lines++;
            current = 0;
        } else {
            current++;
            longest = (current > longest) ? current : longest;
        }
    }
    return {static_cast<float>(longest * 8), static_cast<float>(lines * 16)};
}

struct Family {
    mermaidext::Kind kind;
    const char* name;
    const char* source;
};

std::vector<std::string> splitLines(const std::string& source) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : source) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

// Sources that every family must survive
std::vector<std::string> hostileSources(const Family& family) {
    std::vector<std::string> out;
    const std::string seed = family.source;
    std::vector<std::string> lines = splitLines(seed);
    const std::string header = lines.empty() ? std::string() : lines[0];

    out.push_back("");
    out.push_back("\n\n");
    out.push_back(header);
    out.push_back(header + "\n");
    out.push_back("%% only a comment\n");
    out.push_back(header + "\n%% trailing comment\n");
    out.push_back(seed + "\nunknownKeywordInTheMiddle\n");
    out.push_back(seed + "\n}\n");
    out.push_back(seed + "\n{\n");
    out.push_back(seed + "\n(\n");
    out.push_back(seed + "\n[\n");
    out.push_back(seed + "\n\"unterminated\n");
    out.push_back(seed + "\nn1 --> n2\n");
    out.push_back(seed + "\nn1 : -1\n");
    out.push_back(seed + "\nn1 : 0\n");
    out.push_back(seed + "\nn1 : 1e9\n");
    out.push_back(seed + "\nn1 : abc\n");
    out.push_back(seed + "\n:\n");
    out.push_back(seed + "\n---\n");
    out.push_back(std::string("  \n\t\n") + seed);
    out.push_back(seed + "\n" + seed);
    // Truncations: every prefix of the seed exercises mid-statement states
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string partial;
        for (size_t j = 0; j < i; ++j) {
            partial += lines[j];
            partial += '\n';
        }
        out.push_back(partial);
    }
    return out;
}

bool validExtent(const mermaidext::Built& built) {
    if (!built.ok) return false;
    if (!(built.width > 0.0f) || !(built.height > 0.0f)) return false;
    for (const auto& prim : built.prims) {
        if (prim.x1 != prim.x1 || prim.y1 != prim.y1) return false;  // NaN
    }
    return true;
}

void sweep(const Family& family) {
    const auto sources = hostileSources(family);
    for (size_t i = 0; i < sources.size(); ++i) {
        auto built = mermaidext::build(family.kind, sources[i], fakeMeasure,
                                       1.0f);
        const std::string where = std::string(family.name) + " case " +
                                  std::to_string(i);
        if (built.ok) {
            check(validExtent(built),
                  where + ": a successful build has a finite positive extent");
        } else {
            check(!built.error.empty(), where + ": a rejection explains itself");
        }
    }
}

void sweepScaled(const Family& family) {
    // The app passes contentScale * zoom, so a tiny and a large scale must
    // both stay finite.
    for (float scale : {0.05f, 0.5f, 3.0f}) {
        auto built = mermaidext::build(family.kind, family.source, fakeMeasure,
                                       scale);
        check(built.ok || !built.error.empty(),
              std::string(family.name) + " scales without breaking");
        if (built.ok) {
            check(validExtent(built),
                  std::string(family.name) + " keeps a finite extent when scaled");
        }
    }
}

void sweepUnknownKind() {
    auto built = mermaidext::build(mermaidext::Kind::None, "anything",
                                   fakeMeasure, 1.0f);
    check(!built.ok, "Kind::None never builds");
    auto flow = mermaidext::build(mermaidext::Kind::Flowchart,
                                  "flowchart TD\nA-->B", fakeMeasure, 1.0f);
    check(!flow.ok, "the ext library defers flowcharts to the app engine");
}

// The shared parsing helpers are part of the library's contract, so they are
// driven directly rather than only through a diagram.
void detailHelpers() {
    using namespace mermaidext;
    check(detail::trimView("  x  ") == "x", "trimView strips both sides");
    check(detail::trimView("   ") == "", "trimView yields an empty view");
    check(detail::cleanLabel("a<br>b<br/>c<br />d") == "a\nb\nc\nd",
          "every <br> spelling becomes a newline");
    check(detail::cleanLabel("1 &lt; 2 &amp;&amp; 3 &gt; 2") == "1 < 2 && 3 > 2",
          "entities are decoded");
    const std::string quoted = detail::cleanLabel("&quot;q&quot; &nbsp;x");
    check(quoted.find("\"q\"") != std::string::npos &&
              quoted.find('x') != std::string::npos,
          "quote and non-breaking space entities are decoded");
    check(detail::cleanLabel("it&#39;s").find("it's") != std::string::npos,
          "the numeric apostrophe entity is decoded");

    const auto lines = detail::diagramLines(
        "%% comment\n\n  sequenceDiagram  \nA->>B: hi\n%% trailing\n");
    check(lines.size() == 2, "blank and comment lines are dropped");
    check(lines[0] == "sequenceDiagram", "lines are trimmed");
    check(lines[1] == "A->>B: hi", "statements keep their text");

    std::string_view rest;
    check(detail::startsWithWord("loop retry", "loop", &rest) && rest == "retry",
          "a keyword returns its remainder");
    check(!detail::startsWithWord("looping", "loop", &rest),
          "a longer word does not match the keyword");
    check(!detail::startsWithWord(" lo", "loop", &rest),
          "a leading space does not match the keyword");

    // The cylinder silhouette is shared by ER entities and the architecture
    // database icon: two arcs, a lid seam and straight sides.
    std::vector<Prim> prims;
    Prim paint;
    paint.fill = Role::Fill;
    paint.stroke = Role::Stroke;
    detail::emitCylinder(prims, paint, 0.0f, 0.0f, 60.0f, 40.0f, 12.0f);
    check(prims.size() >= 3, "a cylinder emits its silhouette and lid seam");
    check(!prims.empty() && prims.front().type == PrimType::Polygon,
          "the cylinder body is a polygon");

    Built shifted;
    Prim rect;
    rect.type = PrimType::Rect;
    rect.x1 = -10.0f;
    rect.x2 = 20.0f;
    shifted.prims.push_back(rect);
    shifted.width = 20.0f;
    detail::normalizeLeft(shifted);
    check(shifted.prims[0].x1 == 0.0f && shifted.prims[0].x2 == 30.0f,
          "normalizeLeft shifts negative coordinates right");
    check(shifted.width >= 30.0f, "normalizeLeft grows the width to match");
}

void detectKindAliases() {
    using mermaidext::Kind;
    check(mermaidext::detectKind("C4Component\nx") == Kind::C4,
          "C4Component is a C4 diagram");
    check(mermaidext::detectKind("C4Deployment\nx") == Kind::C4,
          "C4Deployment is a C4 diagram");
    check(mermaidext::detectKind("c4dynamic\nx") == Kind::C4,
          "c4dynamic is a C4 diagram");
    check(mermaidext::detectKind("") == Kind::None, "an empty source is no kind");
    check(mermaidext::detectKind("just some text") == Kind::None,
          "plain text is no kind");
}

}  // namespace

int runRobustSuite() {
    failures = 0;
    static const Family kFamilies[] = {
        {mermaidext::Kind::Sequence, "sequence",
         "sequenceDiagram\n"
         "    participant A as Alice\n"
         "    participant B as Bob\n"
         "    A->>+B: request\n"
         "    B-->>-A: reply\n"
         "    Note over A,B: both\n"
         "    loop retry\n"
         "        A-)B: ping\n"
         "    end\n"},
        {mermaidext::Kind::Class, "class",
         "classDiagram\n"
         "    class Animal {\n"
         "        +int age\n"
         "        +swim()\n"
         "    }\n"
         "    Animal <|-- Duck\n"
         "    Animal : +String name\n"
         "    note for Duck \"a bird\"\n"},
        {mermaidext::Kind::State, "state",
         "stateDiagram-v2\n"
         "    [*] --> Idle\n"
         "    Idle --> Run : start\n"
         "    Run --> Idle : stop\n"
         "    Run --> [*]\n"
         "    note right of Idle : waiting\n"},
        {mermaidext::Kind::Er, "er",
         "erDiagram\n"
         "    CUSTOMER ||--o{ ORDER : places\n"
         "    CUSTOMER {\n"
         "        string name PK\n"
         "        int age\n"
         "    }\n"},
        {mermaidext::Kind::Pie, "pie",
         "pie showData title Share\n"
         "    \"A\" : 60\n"
         "    \"B\" : 40\n"},
        {mermaidext::Kind::Git, "git",
         "gitGraph\n"
         "   commit\n"
         "   branch dev\n"
         "   commit\n"
         "   checkout main\n"
         "   merge dev tag: \"v1\"\n"},
        {mermaidext::Kind::Gantt, "gantt",
         "gantt\n"
         "    dateFormat YYYY-MM-DD\n"
         "    section Build\n"
         "        Compile :a1, 2026-01-01, 5d\n"
         "        Test :after a1, 3d\n"},
        {mermaidext::Kind::Mindmap, "mindmap",
         "mindmap\n  root((center))\n    Child\n      Leaf\n    Other\n"},
        {mermaidext::Kind::Timeline, "timeline",
         "timeline\n    title History\n    2020 : one : two\n    2021 : three\n"},
        {mermaidext::Kind::Journey, "journey",
         "journey\n    title Day\n    section Work\n      Code: 5: Me\n"
         "      Review: 3: Team\n"},
        {mermaidext::Kind::Quadrant, "quadrant",
         "quadrantChart\n    x-axis Low --> High\n    y-axis Low --> High\n"
         "    quadrant-1 Do\n    A: [0.3, 0.6]\n    B: [0.7, 0.2]\n"},
        {mermaidext::Kind::XyChart, "xychart",
         "xychart-beta\n    title Sales\n    x-axis [jan, feb]\n"
         "    y-axis 0 --> 100\n    bar [10, 20]\n    line [20, 10]\n"},
        {mermaidext::Kind::Packet, "packet",
         "packet-beta\ntitle UDP\n0-15: \"Source Port\"\n"
         "16-31: \"Destination Port\"\n32-63: \"Data\"\n"},
        {mermaidext::Kind::Kanban, "kanban",
         "kanban\n  Todo\n    [Write docs]\n    id2[Fix bug]@{ ticket: 'A-1', "
         "assigned: 'ann', priority: 'High' }\n  [Done]\n    [Ship]\n"},
        {mermaidext::Kind::Treemap, "treemap",
         "treemap-beta\ntitle Sizes\n\"Section\"\n    \"Leaf\": 12\n"
         "    \"Other\": 8\n"},
        {mermaidext::Kind::Radar, "radar",
         "radar-beta\ntitle Grades\naxis m[\"Math\"], s[\"Science\"]\n"
         "curve a[\"Alice\"]{85, 90}\nmax 100\nmin 0\n"},
        {mermaidext::Kind::Sankey, "sankey",
         "sankey-beta\nA,B,10\nB,C,5\n"},
        {mermaidext::Kind::Block, "block",
         "block-beta\n    columns 2\n    a[\"A\"] b[\"B\"]\n"
         "    a --> b\n"},
        {mermaidext::Kind::Requirement, "requirement",
         "requirementDiagram\n"
         "    requirement R1 {\n"
         "        id: 1\n"
         "        text: Must render\n"
         "        risk: low\n"
         "    }\n"
         "    element R2 {\n"
         "        text: Other\n"
         "    }\n"
         "    R1 - satisfies -> R2\n"},
        {mermaidext::Kind::C4, "c4",
         "C4Context\n"
         "    title Bank\n"
         "    Person(user, \"User\")\n"
         "    System(sys, \"System\", \"Does things\")\n"
         "    Rel(user, sys, \"Uses\")\n"},
        {mermaidext::Kind::Architecture, "architecture",
         "architecture-beta\n"
         "    group api(cloud)[API]\n"
         "    service a(server)[A] in api\n"
         "    service b(database)[B] in api\n"
         "    a:R -- L:b\n"},
    };
    for (const auto& family : kFamilies) {
        sweep(family);
        sweepScaled(family);
    }
    sweepUnknownKind();
    detailHelpers();
    detectKindAliases();
    if (failures != 0) {
        std::cerr << failures << " robustness test(s) failed\n";
    }
    return failures;
}
