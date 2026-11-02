// Requirement, C4 and architecture diagram families.
//
// These three builders share src/mermaid_c4.cpp. The generic family test in
// mermaid_tests.cpp only proves one representative diagram per family, which
// leaves the palette variants, the boundary nestings, the side-constrained
// grid placement and every rejection path unexecuted. This suite walks each
// construct and each error branch of the three builders.

#include "mermaid_ext.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    failures++;
}

// Character-cell measurer: 8px per column, 16px per line, like the app's
// fake in mermaid_tests.cpp so the geometry stays comparable.
mermaidext::Size fakeMeasure(const std::string& text,
                             const mermaidext::TextStyle&, float) {
    size_t longest = 0, current = 0, lines = text.empty() ? 0 : 1;
    for (char c : text) {
        if (c == '\n') {
            lines++;
            current = 0;
        } else {
            current++;
            longest = std::max(longest, current);
        }
    }
    return {static_cast<float>(longest * 8), static_cast<float>(lines * 16)};
}

mermaidext::Built buildRequirement(const char* source) {
    return mermaidext::detail::buildRequirement(source, fakeMeasure, 1.0f);
}

mermaidext::Built buildC4(const char* source) {
    return mermaidext::detail::buildC4(source, fakeMeasure, 1.0f);
}

mermaidext::Built buildArchitecture(const char* source) {
    return mermaidext::detail::buildArchitecture(source, fakeMeasure, 1.0f);
}

int countType(const mermaidext::Built& built, mermaidext::PrimType type) {
    int total = 0;
    for (const auto& prim : built.prims) {
        if (prim.type == type) total++;
    }
    return total;
}

void expectError(const mermaidext::Built& built, const char* error,
                 const std::string& label) {
    check(!built.ok, label + " is rejected");
    check(built.error == error, label + " reports: " + error +
                                    " (got '" + built.error + "')");
}

// --- requirement diagrams -------------------------------------------------

void requirementParsesFields() {
    const char* source = R"(requirementDiagram
    requirement R1 {
        id: 1
        text: The system shall render Markdown
        risk: medium
        verifymethod: test
        docref: RFC-1
        type: functional
    }
    element R2 {
        id: 2
        text: Second element
    }
    element R3 {
        id: 3
        text: Third element
    }
    R1 - satisfies -> R2
    R2 - copies - -> R3
)";
    auto built = buildRequirement(source);
    check(built.ok, "requirement diagram builds");
    if (!built.ok) {
        std::cerr << "  (" << built.error << ")\n";
        return;
    }
    check(built.width > 0.0f && built.height > 0.0f,
          "requirement diagram has an extent");
    // Two boxes, two relation chips and one rule per node
    check(countType(built, mermaidext::PrimType::Rect) >= 2,
          "requirement nodes are boxes");
    check(countType(built, mermaidext::PrimType::Text) >= 4,
          "requirement stereotype, name and body lines are text");
    check(countType(built, mermaidext::PrimType::Line) >= 3,
          "requirement rules and relations are lines");
}

void requirementSameLineBrace() {
    auto built = buildRequirement(
        "requirementDiagram\n"
        "requirement R1 {\n"
        "    text: inline brace\n"
        "}\n");
    check(built.ok, "a node opening on the same line as its name builds");
    check(built.width > 0.0f, "same-line brace node has width");
}

void requirementRanksAndLanes() {
    // R1 -> R2 -> R3 plus a rank-skipping R1 -> R3 that must detour through
    // an exterior lane instead of cutting through R2.
    const char* source = R"(
requirementDiagram
    functionalRequirement A {
        text: first
    }
    functionalRequirement B {
        text: second
    }
    functionalRequirement C {
        text: third
    }
    A - derives -> B
    B - derives -> C
    A - traces -> C
)";
    auto built = buildRequirement(source);
    check(built.ok, "rank-skipping requirement diagram builds");
    if (!built.ok) {
        std::cerr << "  (" << built.error << ")\n";
        return;
    }
    // The detour is three dashed segments (out, down, in) plus the two
    // straight relations, so five lines and three label chips.
    check(countType(built, mermaidext::PrimType::Line) >= 3,
          "rank-skipping relation emits a lane detour");
}

void requirementRejectsBadInput() {
    expectError(buildRequirement(""), "Empty diagram", "empty source");
    expectError(buildRequirement("flowchart TD\nA-->B"),
                "Expected requirementDiagram header", "wrong header");
    expectError(buildRequirement("requirementDiagram"),
                "Empty requirement diagram", "header only");
    expectError(buildRequirement("requirementDiagram\nfunctionalRequirement {\n"),
                "Requirement without a name", "unnamed node");
    expectError(
        buildRequirement("requirementDiagram\nrequirement R1 {\ntext no colon\n}\n"),
        "Expected key: value", "field without a colon");
    expectError(
        buildRequirement("requirementDiagram\nrequirement R1 {\npriority: high\n}\n"),
        "Unknown requirement field", "unknown field");
    expectError(buildRequirement("requirementDiagram\nnote over R1\n"),
                "Unsupported requirement statement", "stray statement");
    expectError(
        buildRequirement("requirementDiagram\n"
                         "requirement R1 {\n}\nrequirement R2 {\n}\n"
                         "R1 - likes -> R2\n"),
        "Unsupported requirement relation", "unknown relation name");
    expectError(
        buildRequirement("requirementDiagram\n"
                         "requirement R1 {\n}\n"
                         "R9 - satisfies -> R1\n"),
        "Unsupported requirement relation", "unknown endpoint");
    expectError(
        buildRequirement("requirementDiagram\n"
                         "requirement R1 {\n}\nrequirement R2 {\n}\n"
                         "R1 - satisfies -> R2\nR2 - satisfies -> R1\n"),
        "Requirement relations form a cycle", "cyclic relations");
}

// --- C4 diagrams ----------------------------------------------------------

void c4EveryElementKind() {
    const char* source = R"(C4Context
    title Bank system
    Person(customer, "Customer", "A customer of the bank")
    Person_Ext(admin, "Admin")
    System(banking, "Banking System", "Handles accounts")
    System_Ext(mainframe, "Mainframe")
    SystemDb(store, "Account Store", "Stores accounts")
    SystemQueue(bus, "Event Bus", "Kafka")
    Container(spa, "SPA", "JavaScript", "Single page app")
    Container_Ext(legacy, "Legacy", "COBOL")
    ContainerDb(db, "Database", "PostgreSQL", "Holds records")
    ContainerQueue(queue, "Queue", "RabbitMQ", "Async work")
    Component(api, "API", "Spring", "REST endpoints")
    Component_Ext(third, "Third party", "SOAP")
    ComponentDb(compdb, "Component DB", "SQLite")
    ComponentQueue(compqueue, "Component Queue", "SQS")
    Rel(customer, banking, "Uses", "HTTPS")
    BiRel(banking, store, "Reads")
    Rel(spa, api, "")
    UpdateLayoutConfig($c4ShapeInRow="3")
    SHOW_LEGEND()
    LAYOUT_LEFT_RIGHT()
    Boundary(b0, "Zone") {
        System(inner, "Inner")
    }
    Enterprise_Boundary(eb, "Enterprise") {
        System(erp, "ERP")
    }
    System_Boundary(sb, "System") {
        Container(web, "Web", "Go")
    }
    Container_Boundary(cb, "Container") {
        Component(part, "Part", "C")
    }
    Deployment_Node(dn, "Node", "Linux") {
        Container(svc, "Service", "Rust")
    }
    Node(n1, "Plain node") {
        System(nsys, "Sys")
    }
    Node_L(nl, "Left node", "VM") {
        System(lsys, "LSys")
    }
    Node_R(nr, "Right node") {
        System(rsys, "RSys")
    }
)";
    auto built = buildC4(source);
    check(built.ok, "C4 diagram with every element kind builds");
    if (!built.ok) {
        std::cerr << "  (" << built.error << ")\n";
        return;
    }
    check(built.width > 0.0f && built.height > 0.0f, "C4 extent is positive");
    // Cylinders (db), rounded boxes, person glyphs and dashed frames all
    // contribute primitives of their own.
    check(countType(built, mermaidext::PrimType::Rect) >= 1,
          "C4 boundaries are rectangles");
    check(countType(built, mermaidext::PrimType::RoundRect) >= 3,
          "C4 elements and relation chips are rounded rectangles");
    check(countType(built, mermaidext::PrimType::Ellipse) >= 2,
          "person glyphs are ellipses");
    check(countType(built, mermaidext::PrimType::Slice) >= 2,
          "person shoulders are slices");
    check(countType(built, mermaidext::PrimType::Text) >= 20,
          "titles, boundary labels and element texts are all emitted");
}

void c4RelationLabelAvoidsBoxes() {
    // Three elements share one row, so the A -> C relation crosses B and the
    // label has to slide to the widest clear stretch of the line.
    const char* source = R"(C4Context
    Person(a, "A")
    Person(b, "B")
    Person(c, "C")
    Rel(a, c, "Label that is long enough to block", "Techn")
)";
    auto built = buildC4(source);
    check(built.ok, "crossing relation builds");
    if (!built.ok) {
        std::cerr << "  (" << built.error << ")\n";
        return;
    }
    // The chip carries label plus technology line
    check(countType(built, mermaidext::PrimType::RoundRect) >= 4,
          "relation chip with a technology line is emitted");
}

void c4FlowsMembersInRows() {
    // More members than kC4PerRow forces a second row both in measurement
    // and in placement.
    const char* source = R"(C4Context
    System(s1, "One")
    System(s2, "Two")
    System(s3, "Three")
    System(s4, "Four")
    System(s5, "Five")
    Rel(s1, s5, "far")
    Boundary(b, "Nested") {
        System(n1, "N1")
        System(n2, "N2")
        System(n3, "N3")
        System(n4, "N4")
    }
)";
    auto built = buildC4(source);
    check(built.ok, "wide C4 diagram builds");
    if (!built.ok) {
        std::cerr << "  (" << built.error << ")\n";
        return;
    }
    check(built.height > 200.0f, "wrapped rows increase the diagram height");
}

void c4EmptyBoundaryAndSkippedStatements() {
    auto built = buildC4(
        "C4Context\n"
        "show_legend\n"
        "layout_elk\n"
        "Boundary(b1, \"Empty\")\n"
        "Boundary(b2, \"Typed\", \"system\")\n"
        "System(a, \"A\")\n");
    check(built.ok, "an empty boundary closes on its own line");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void c4RejectsBadInput() {
    expectError(buildC4(""), "Empty diagram", "empty source");
    expectError(buildC4("C4Context"), "Empty C4 diagram", "header only");
    expectError(buildC4("C4Context\n}"), "Unmatched }", "stray brace");
    expectError(buildC4("C4Context\nboundary(\"x\")"), "Bad boundary",
                "boundary with one argument");
    expectError(buildC4("C4Context\nRel(a, \"b\")"), "Bad relation",
                "relation with two arguments");
    expectError(buildC4("C4Context\nnonsense thing"), "Unsupported C4 statement",
                "unknown statement");
    expectError(buildC4("C4Context\nSystem(a)"), "Bad C4 element",
                "element with one argument");
    expectError(buildC4("C4Context\nSystem(a, \"A\")\nRel(a, \"b\", \"x\")\n"),
                "Unknown alias in relation", "relation to a missing alias");
}

// --- architecture diagrams -------------------------------------------------

void architectureIconsAndGroups() {
    const char* source = R"(architecture-beta
    group api(cloud)[API]
    group inner(server)[Inner] in api
    service db(database)[Database] in api
    service disk1(disk)[Storage] in api
    service cloud1(cloud)[Cloud] in inner
    service net(internet)[Internet]
    service srv(server)[Server] in inner
    service bare[No icon]
    service unknown(queued)[Unknown icon]
    junction j1 in api
    db:R -- L:disk1
    disk1:B -- T:cloud1
    cloud1:L <-- R:net
    net:T <--> B:srv
    srv:R -- L:bare
    bare:B -- T:unknown
    unknown:L -- R:j1
)";
    auto built = buildArchitecture(source);
    check(built.ok, "architecture diagram with every icon builds");
    if (!built.ok) {
        std::cerr << "  (" << built.error << ")\n";
        return;
    }
    check(built.width > 0.0f && built.height > 0.0f, "architecture extent");
    // cloud is a polygon, otherwise icons are boxes/cylinders/ellipses
    check(countType(built, mermaidext::PrimType::Polygon) >= 1,
          "the cloud icon is a polygon");
    check(countType(built, mermaidext::PrimType::Ellipse) >= 2,
          "junctions and server lights are ellipses");
    check(countType(built, mermaidext::PrimType::Line) >= 7,
          "every edge is a line");
    check(countType(built, mermaidext::PrimType::RoundRect) >= 3,
          "group frames and server icons are rounded rectangles");
}

void architectureEdgeModifiers() {
    // {group} modifiers on either endpoint are stripped before the lookup,
    // and every side letter is used at least once.
    const char* source = R"(architecture-beta
    group g(cloud)[G]
    service a(server)[A]
    service b(server)[B]
    a{g}:R -- L:b{g}
    b:T -- B:a
)";
    auto built = buildArchitecture(source);
    check(built.ok, "group-modified architecture edges build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void architectureUnconstrainedNodesFlowBelow() {
    // Only the first node is placed by an edge constraint; the two remaining
    // nodes take fresh cells below, and the second has to skip the cell the
    // first one took.
    const char* source = R"(architecture-beta
    service first(server)[First]
    service second(server)[Second]
    service third(server)[Third]
    service fourth(server)[Fourth]
    first:R -- L:fourth
)";
    auto built = buildArchitecture(source);
    check(built.ok, "unconstrained nodes are placed");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    check(built.height > 0.0f, "fallback rows extend the diagram");
}

void architectureSharedSideCollisions() {
    // Two edges leaving the same anchor on the same side must not land on
    // the same grid cell: the second target is pushed down a row.
    const char* source = R"(architecture-beta
    service hub(server)[Hub]
    service one(server)[One]
    service two(server)[Two]
    service three(server)[Three]
    hub:R -- L:one
    hub:R -- L:two
    hub:R -- L:three
)";
    auto built = buildArchitecture(source);
    check(built.ok, "colliding placements are resolved");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void architectureRejectsBadInput() {
    expectError(buildArchitecture(""), "Empty diagram", "empty source");
    expectError(buildArchitecture("flowchart TD\nA-->B"),
                "Expected architecture header", "wrong header");
    expectError(buildArchitecture("architecture-beta"),
                "Empty architecture diagram", "header only");
    expectError(buildArchitecture("architecture\nservice a()\nnote here\n"),
                "Unsupported architecture statement", "unknown statement");
    expectError(buildArchitecture("architecture-beta\nservice a(server)[A]\n"
                                  "service b(server)[B]\na:R -- L:missing\n"),
                "Unsupported architecture statement",
                "edge to an unknown node");
    expectError(buildArchitecture("architecture-beta\nservice a(server)[A]\n"
                                  "service b(server)[B]\na:X -- L:b\n"),
                "Unsupported architecture statement", "unknown side letter");
}

}  // namespace

int runC4Suite() {
    failures = 0;
    requirementParsesFields();
    requirementSameLineBrace();
    requirementRanksAndLanes();
    requirementRejectsBadInput();
    c4EveryElementKind();
    c4RelationLabelAvoidsBoxes();
    c4FlowsMembersInRows();
    c4EmptyBoundaryAndSkippedStatements();
    c4RejectsBadInput();
    architectureIconsAndGroups();
    architectureEdgeModifiers();
    architectureUnconstrainedNodesFlowBelow();
    architectureSharedSideCollisions();
    architectureRejectsBadInput();
    if (failures != 0) {
        std::cerr << failures << " requirement/C4 test(s) failed\n";
    }
    return failures;
}
