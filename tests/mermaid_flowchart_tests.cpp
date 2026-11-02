// Flowchart parser: shapes, styles, edges, subgraphs and rejections.
//
// The native flowchart engine is the only family with its own syntax for
// node shapes, class styles, labelled edges and subgraphs. The generic
// family test covers one diagram; this suite walks every delimiter, every
// edge token, the style/classDef error paths, entity decoding and the
// subgraph bookkeeping that the layout ranks depend on.

#include "mermaid.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    failures++;
}

mermaid::ParseResult parse(const std::string& source) {
    return mermaid::parse(source);
}

bool rejects(const std::string& source, const char* expected,
             const std::string& label) {
    auto result = parse(source);
    if (result.success) {
        check(false, label + " is rejected");
        return false;
    }
    if (!expected) return true;
    check(result.error == expected,
          label + " reports '" + expected + "' (got '" + result.error + "')");
    return true;
}

const mermaid::Node* node(const mermaid::Diagram& diagram, const std::string& id) {
    for (const auto& item : diagram.nodes) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

// --- shapes ---------------------------------------------------------------

void nodeShapes() {
    const char* source = R"(flowchart TD
A[Rectangle]
B(Rounded)
C([Stadium])
D[[Subroutine]]
E[(Cylinder)]
F((Circle))
H{Diamond}
I{{Hexagon}}
J[/Parallelogram/]
K[\Trapezoid\]
L[#quot;quoted#quot;]
)";
    auto result = parse(source);
    check(result.success, "every node shape parses");
    if (!result.success) {
        std::cerr << "  (" << result.error << ")\n";
        return;
    }
    check(result.diagram.nodes.size() == 11, "all eleven shapes are nodes");
    const auto* rect = node(result.diagram, "A");
    const auto* diamond = node(result.diagram, "H");
    const auto* circle = node(result.diagram, "F");
    check(rect && rect->shape == mermaid::NodeShape::Rectangle,
          "brackets give a rectangle");
    check(diamond && diamond->shape == mermaid::NodeShape::Diamond,
          "braces give a diamond");
    check(circle && circle->shape == mermaid::NodeShape::Circle,
          "double parentheses give a circle");
}

void labelsAndEntities() {
    auto result = parse(
        "flowchart LR\n"
        "A[\"Quoted \\\"label\\\" here\"]\n"
        "B[Line<br/>break]\n"
        "C[\"1 &lt; 2 &amp;&amp; 3 &gt; 2\"]\n"
        "D[\"a &quot;b&quot; &nbsp; c\"]\n"
        "E[escaped \\# hash]\n");
    check(result.success, "quoted and escaped labels parse");
    if (!result.success) {
        std::cerr << "  (" << result.error << ")\n";
        return;
    }
    const auto* quoted = node(result.diagram, "A");
    check(quoted && quoted->label.find("\"label\"") != std::string::npos,
          "an escaped quote survives in the label");
    const auto* entities = node(result.diagram, "C");
    check(entities && entities->label.find("1 < 2 && 3 > 2") != std::string::npos,
          "entities are decoded (" + (entities ? entities->label : std::string()) + ")");
    const auto* breaks = node(result.diagram, "B");
    check(breaks && breaks->label.find('\n') != std::string::npos,
          "<br/> becomes a line break");
}

// --- edges ----------------------------------------------------------------

void edges() {
    auto result = parse(
        "flowchart TD\n"
        "A --> B\n"
        "B --- C\n"
        "C -.-> D\n"
        "D ==> E\n"
        "E -- text label --> F\n");
    check(result.success, "every edge token parses");
    if (!result.success) {
        std::cerr << "  (" << result.error << ")\n";
        return;
    }
    check(result.diagram.edges.size() == 5, "each statement makes an edge");
    bool sawDashed = false, sawThick = false, sawLabelled = false;
    for (const auto& edge : result.diagram.edges) {
        if (edge.dashed) sawDashed = true;
        if (edge.strokeScale > 1.5f) sawThick = true;
        if (edge.label == "text label") sawLabelled = true;
    }
    check(sawDashed, "dashed edges are marked dashed");
    check(sawThick, "thick edges are marked thick");
    check(sawLabelled, "an edge label between dashes is captured");

    // An edge label that never closes, and an edge id before the arrow
    rejects("flowchart TD\nA -->|unclosed B\n", "Unterminated edge label",
            "an unterminated edge label");
    auto edgeId = parse("flowchart TD\nA e1@--> B\nA --> B;\n");
    check(edgeId.success, "an edge id and a trailing semicolon parse");
    if (!edgeId.success) std::cerr << "  (" << edgeId.error << ")\n";

    rejects("flowchart TD\nA -- B\n", "Unsupported edge syntax",
            "a dash pair without an arrow");
    rejects("flowchart TD\nA - B\n", "Expected a Mermaid edge",
            "a single dash");
}

void nodeChainingAndClasses() {
    auto result = parse(
        "flowchart LR\n"
        "A --> B --> C\n"
        "A --> C\n"
        "class A,B emphasised\n"
        "classDef emphasised fill:#ffeeaa,stroke:#336699,stroke-width:3px,color:#222222\n"
        "D:::emphasised\n"
        "A:::emphasised --> D\n");
    check(result.success, "chaining, classes and classDef parse");
    if (!result.success) {
        std::cerr << "  (" << result.error << ")\n";
        return;
    }
    check(result.diagram.nodes.size() == 4, "chained nodes are created once");
    auto style = result.diagram.classStyles.find("emphasised");
    check(style != result.diagram.classStyles.end(), "classDef is recorded");
    if (style != result.diagram.classStyles.end()) {
        check(style->second.hasFill && style->second.fill.rgb == 0xFFEEAA,
              "the class fill is parsed");
        check(style->second.hasStrokeWidth && style->second.strokeWidth == 3.0f,
              "the class stroke width is parsed");
        check(style->second.hasText, "the class text colour is parsed");
    }
}

void styleStatements() {
    auto result = parse(
        "flowchart TD\n"
        "A --> B\n"
        "style A fill:#ff0000,stroke:#00ff00,color:#0000ff,stroke-width:4px\n"
        "style B fill:red,stroke:grey\n");
    check(result.success, "style statements parse");
    if (!result.success) std::cerr << "  (" << result.error << ")\n";

    // "none"/"transparent" are valid fills, and a short hex is not
    auto transparent = parse(
        "flowchart TD\n"
        "A --> B\n"
        "style A fill:none,stroke:transparent\n"
        "style B fill:#12345678\n");
    check(transparent.success, "none and transparent colours parse");
    if (!transparent.success) std::cerr << "  (" << transparent.error << ")\n";

    rejects("flowchart TD\nA\nstyle A fill:zzz\n", "Invalid fill color",
            "an unparsable fill");
    rejects("flowchart TD\nA\nstyle A stroke:#12345\n", "Invalid stroke color",
            "a hex colour of the wrong length");
    rejects("flowchart TD\nA\nstyle A stroke:zzz\n", "Invalid stroke color",
            "an unparsable stroke");
    rejects("flowchart TD\nA\nstyle A color:zzz\n", "Invalid text color",
            "an unparsable text colour");
    rejects("flowchart TD\nA\nstyle A stroke-width:wide\n",
            "Invalid stroke width", "an unparsable stroke width");
}

// --- directions and structure --------------------------------------------

void directions() {
    struct Case { const char* token; mermaid::Direction expected; };
    const Case kCases[] = {
        {"TB", mermaid::Direction::TopToBottom},
        {"TD", mermaid::Direction::TopToBottom},
        {"BT", mermaid::Direction::BottomToTop},
        {"LR", mermaid::Direction::LeftToRight},
        {"RL", mermaid::Direction::RightToLeft},
    };
    for (const auto& testCase : kCases) {
        std::string source = "flowchart ";
        source += testCase.token;
        source += "\nA --> B\n";
        auto result = parse(source);
        check(result.success, std::string("direction ") + testCase.token + " parses");
        check(result.diagram.direction == testCase.expected,
              std::string("direction ") + testCase.token + " is captured");
    }
    rejects("flowchart XY\nA --> B\n", "Unsupported flowchart direction",
            "an unknown direction");

    // The header keyword is matched case-insensitively
    auto upper = parse("FLOWCHART TD\nA --> B\n");
    check(upper.success, "an upper-case header parses");
    auto mixed = parse("Graph LR\nA --> B\n");
    check(mixed.success, "a mixed-case graph header parses");
}

void subgraphs() {
    auto result = parse(
        "flowchart TB\n"
        "subgraph one [First group]\n"
        "A --> B\n"
        "end\n"
        "subgraph \"Second group\"\n"
        "C --> D\n"
        "subgraph nested\n"
        "E\n"
        "end\n"
        "end\n"
        "A --> C\n");
    check(result.success, "nested subgraphs parse");
    if (!result.success) {
        std::cerr << "  (" << result.error << ")\n";
        return;
    }
    check(result.diagram.subgraphs.size() >= 2, "both subgraphs are recorded");

    rejects("flowchart TB\nsubgraph one\nA\nend\nend\n",
            "Unmatched subgraph end", "one end too many");
    rejects("flowchart TB\nend\n", "Unmatched subgraph end",
            "an end without a subgraph");
}

void presentationDirectives() {
    // click handlers, linkStyle and in-body direction hints have nothing to
    // draw natively; the diagram must still parse
    auto result = parse(
        "flowchart TD\n"
        "A --> B\n"
        "direction LR\n"
        "click A \"https://example.com\"\n"
        "linkStyle 0 stroke:#333\n");
    check(result.success, "presentation directives parse");
    if (!result.success) std::cerr << "  (" << result.error << ")\n";

    // Mermaid v11 node config: the node survives with its label, images and
    // fancy shapes degrade to a plain box
    auto configured = parse("flowchart TD\nA@{ shape: rounded, label: \"Configured\" }\nA --> B\n");
    check(configured.success, "a node config keeps its node");
    if (configured.success) {
        const auto* a = node(configured.diagram, "A");
        check(a && a->label == "Configured", "the config label is used");
    }
    // An unterminated config cannot be understood and falls back
    rejects("flowchart TD\nA@{ shape: rounded\n",
            "Mermaid '@{ }' attribute syntax is not supported",
            "an unterminated v11 attribute");
}

void rejections() {
    rejects("sequenceDiagram\nA->>B: hi\n", nullptr,
            "a non-flowchart diagram");
    rejects("flowchart TD\n", "The Mermaid flowchart has no nodes",
            "a flowchart without nodes");
    rejects("flowchart TD\n[label]\n", "Expected a node identifier",
            "a statement that starts with a delimiter");
    rejects("flowchart TD\nA[\"unterminated\n",
            "Unterminated quoted node label", "an unterminated quoted label");
    rejects("flowchart TD\nA[\"quoted\" tail\n",
            "Expected closing node delimiter",
            "a quoted label without its delimiter");
    rejects("flowchart TD\nA[label\n", "Unterminated node label",
            "a missing closing bracket");
    rejects("flowchart TD\nA[open\n", "Unterminated node label",
            "an unterminated label");
    rejects("flowchart TD\nA::: \n", "Expected a class name after ':::'",
            "an empty class reference");
    rejects("flowchart TD\nclassDef\n", "Expected a class name after classDef",
            "a classDef without a name");
    rejects("flowchart TD\nclass\n", "Expected node IDs and a class name",
            "a class statement without ids");
    rejects("flowchart TD\nstyle\n", "Expected a node ID after style",
            "a style statement without an id");
    rejects("flowchart TD\nsubgraph\n", "Expected a subgraph name",
            "a subgraph without a name");
}

}  // namespace

int runFlowchartSuite() {
    failures = 0;
    nodeShapes();
    labelsAndEntities();
    edges();
    nodeChainingAndClasses();
    styleStatements();
    directions();
    subgraphs();
    presentationDirectives();
    rejections();
    if (failures != 0) {
        std::cerr << failures << " flowchart test(s) failed\n";
    }
    return failures;
}
