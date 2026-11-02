// Construct coverage for the chart-like families: quadrant, xychart, radar,
// sankey, block, git, pie and the grid families (packet, kanban, treemap).
//
// Each of these parsers has its own small statement set, and the axes,
// legends, series and node shapes they emit are untouched by the generic
// per-family test. This suite walks their statements and their rejections.

#include "mermaid_ext.h"

#include <iostream>
#include <string>

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

mermaidext::Built build(mermaidext::Kind kind, const std::string& source) {
    return mermaidext::build(kind, source, fakeMeasure, 1.0f);
}

void expectError(const mermaidext::Built& built, const char* error,
                 const std::string& label) {
    check(!built.ok, label + " is rejected");
    check(built.error == error,
          label + " reports '" + error + "' (got '" + built.error + "')");
}

void quadrant() {
    auto built = build(mermaidext::Kind::Quadrant,
                       "quadrantChart\n"
                       "    title 优先级\n"
                       "    x-axis \"Low effort\" --> \"High effort\"\n"
                       "    y-axis Low impact --> High impact\n"
                       "    quadrant-1 Do first\n"
                       "    quadrant-2 Schedule\n"
                       "    quadrant-3 Drop\n"
                       "    quadrant-4 Delegate\n"
                       "    Task A: [0.2, 0.8]\n"
                       "    \"Task B\": [0.9, 0.9]\n");
    check(built.ok, "a titled quadrant chart builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";

    expectError(build(mermaidext::Kind::Quadrant, "pie\n\"A\" : 1\n"),
                "Expected quadrantChart header", "a wrong header");
    expectError(build(mermaidext::Kind::Quadrant,
                      "quadrantChart\n    nope nope\n"),
                "Unsupported quadrant statement", "an unknown statement");
}

void xychart() {
    auto built = build(mermaidext::Kind::XyChart,
                       "xychart-beta\n"
                       "    title \"Sales by month\"\n"
                       "    x-axis \"Month\" [jan, feb, mar]\n"
                       "    y-axis \"Units\" 0 --> 100\n"
                       "    bar \"Bars\" [10, 20, 30]\n"
                       "    line \"Line\" [30, 20, 10]\n"
                       "    bar [1, 2, 3]\n");
    check(built.ok, "a titled xy chart with two axes and series builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";

    auto bare = build(mermaidext::Kind::XyChart,
                      "xychart-beta\n    bar [1, 2]\n");
    check(bare.ok, "a chart without axes builds");
    if (!bare.ok) std::cerr << "  (" << bare.error << ")\n";

    expectError(build(mermaidext::Kind::XyChart, "pie\n\"A\" : 1\n"),
                "Expected xychart header", "a wrong header");
    expectError(build(mermaidext::Kind::XyChart,
                      "xychart-beta horizontal\n    bar [1]\n"),
                "Horizontal xy charts are not supported", "a horizontal chart");
    expectError(build(mermaidext::Kind::XyChart,
                      "xychart-beta\n    x-axis jan fe\rb\n"),
                "Bad x-axis", "an axis without an arrow");
    expectError(build(mermaidext::Kind::XyChart,
                      "xychart-beta\n    bar oops\n"),
                "Bad series data", "series without brackets");
    expectError(build(mermaidext::Kind::XyChart,
                      "xychart-beta\n    bar [1, oops]\n"),
                "Bad series value", "a non-numeric series value");
    expectError(build(mermaidext::Kind::XyChart,
                      "xychart-beta\n    title only\n"),
                "No series", "a chart without series");
}

void radar() {
    auto built = build(mermaidext::Kind::Radar,
                       "radar-beta\n"
                       "    title Grades\n"
                       "    axis m[\"Math\"], s[\"Science\"], e[\"English\"]\n"
                       "    curve a[\"Alice\"]{85, 90, 80}\n"
                       "    curve b{70, 75, 85}\n"
                       "    max 100\n"
                       "    min 0\n"
                       "    ticks 4\n"
                       "    showLegend true\n");
    check(built.ok, "a radar chart with two curves builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void sankey() {
    auto built = build(mermaidext::Kind::Sankey,
                       "sankey-beta\n"
                       "A,B,10\n"
                       "A,C,5.5\n"
                       "\"Quoted, node\",D,2\n"
                       "B,D,8\n"
                       "C,D,5\n");
    check(built.ok, "a sankey graph with quoted names builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";

    expectError(build(mermaidext::Kind::Sankey, "sankey-beta\nA,B\n"),
                "Bad sankey value", "a row without a value");
}

void block() {
    auto built = build(mermaidext::Kind::Block,
                       "block-beta\n"
                       "    columns 3\n"
                       "    a[\"A\"] b[\"B\"] c[\"C\"]\n"
                       "    d space:2\n"
                       "    a --> b\n"
                       "    b --> c\n"
                       "    block:group\n"
                       "        e[\"E\"]\n"
                       "    end\n");
    check(built.ok, "blocks, spaces and a nested group build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void git() {
    auto built = build(mermaidext::Kind::Git,
                       "gitGraph\n"
                       "    commit id: \"a1\"\n"
                       "    commit tag: \"v1\" type: HIGHLIGHT\n"
                       "    branch develop order: 2\n"
                       "    checkout develop\n"
                       "    commit id: \"b1\"\n"
                       "    checkout main\n"
                       "    merge develop id: \"m1\" type: REVERSE\n"
                       "    cherry-pick id: \"b1\"\n"
                       "    commit type: NORMAL\n");
    check(built.ok, "commits, branches, merges and cherry-picks build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";

    expectError(build(mermaidext::Kind::Git, "gitGraph\n    nope\n"),
                "Unsupported gitGraph statement", "an unknown statement");
}

void pie() {
    auto built = build(mermaidext::Kind::Pie,
                       "pie showData\n"
                       "    title Browser share\n"
                       "    \"Chrome\" : 65.5\n"
                       "    \"Safari\" : 20\n"
                       "    \"Other\" : 14.5\n");
    check(built.ok, "a titled pie chart builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";

    auto bare = build(mermaidext::Kind::Pie, "pie\n    \"A\" : 1\n");
    check(bare.ok, "a pie chart without showData builds");
    if (!bare.ok) std::cerr << "  (" << bare.error << ")\n";

    expectError(build(mermaidext::Kind::Pie, "pie\n    \"A\" : 0\n"),
                "No slice data", "a zero slice");
    expectError(build(mermaidext::Kind::Pie, "pie\n    \"A\" : zero\n"),
                "Bad slice value", "a non-numeric slice");
}

void grids() {
    auto packet = build(mermaidext::Kind::Packet,
                        "packet-beta\n"
                        "    title TCP\n"
                        "    0-3: \"Flags\"\n"
                        "    4-15: \"Source port\"\n"
                        "    16-31: \"Destination\"\n"
                        "    32-47: \"Sequence\"\n"
                        "+16: \"Payload\"\n");
    check(packet.ok, "a packet diagram with a relative field builds");
    if (!packet.ok) std::cerr << "  (" << packet.error << ")\n";

    auto kanban = build(mermaidext::Kind::Kanban,
                        "kanban\n"
                        "    Todo\n"
                        "        [Write docs]\n"
                        "        t2[Fix bug]@{ ticket: 'A-1', priority: 'High' }\n"
                        "        t3[Small]@{ priority: 'low', assigned: 'ann', "
                        "ticket: 'A-2' }\n"
                        "        t4[Tiny]@{ priority: 'very low' }\n"
                        "        t5[Urgent]@{ priority: 'Very High' }\n"
                        "        t6[Medium]@{ priority: 'medium' }\n"
                        "    In Progress\n"
                        "        [Review]\n"
                        "    Done\n"
                        "        [Ship]\n");
    check(kanban.ok, "a kanban board with metadata builds");
    if (!kanban.ok) std::cerr << "  (" << kanban.error << ")\n";

    auto treemap = build(mermaidext::Kind::Treemap,
                         "treemap-beta\n"
                         "    title Sizes\n"
                         "    \"Section 1\"\n"
                         "        \"Leaf 1\": 12\n"
                         "        \"Leaf 2\": 8\n"
                         "    \"Section 2\"\n"
                         "        \"Leaf 3\": 10\n");
    check(treemap.ok, "a nested treemap builds");
    if (!treemap.ok) std::cerr << "  (" << treemap.error << ")\n";
}

void narrative() {
    auto mindmap = build(mermaidext::Kind::Mindmap,
                         "mindmap\n"
                         "  root((Center))\n"
                         "    Square\n"
                         "    Round(Round)\n"
                         "      Deep\n"
                         "    Cloud)Cloud(\n"
                         "    Bang))Bang((\n");
    check(mindmap.ok, "a mindmap with every node shape builds");
    if (!mindmap.ok) std::cerr << "  (" << mindmap.error << ")\n";

    auto timeline = build(mermaidext::Kind::Timeline,
                          "timeline\n"
                          "    title History\n"
                          "    section Early\n"
                          "        2020 : first : second\n"
                          "        2021 : third\n"
                          "    section Late\n"
                          "        2022 : fourth\n");
    check(timeline.ok, "a sectioned timeline builds");
    if (!timeline.ok) std::cerr << "  (" << timeline.error << ")\n";

    auto journey = build(mermaidext::Kind::Journey,
                         "journey\n"
                         "    title Day\n"
                         "    section Work\n"
                         "      Code: 5: Me\n"
                         "      Review: 3: Team\n"
                         "    section Rest\n"
                         "      Sleep: 1: Me\n");
    check(journey.ok, "a journey with sections builds");
    if (!journey.ok) std::cerr << "  (" << journey.error << ")\n";
}

void rejections() {
    // radar
    expectError(build(mermaidext::Kind::Radar, "pie\n\"A\" : 1\n"),
                "Expected radar header", "a wrong radar header");
    expectError(build(mermaidext::Kind::Radar,
                      "radar-beta\n    axis a[\"A\"], b[\"B\"], c[\"C\"]\n"
                      "    curve x{1}\n    curve y{2}\n"),
                "Curve values do not match the axes", "a short curve");
    expectError(build(mermaidext::Kind::Radar,
                      "radar-beta\n    axis a[\"A\"]\n"),
                "Radar needs at least three axes", "a radar with one axis");
    expectError(build(mermaidext::Kind::Radar,
                      "radar-beta\n    axis a[\"A\"], b[\"B\"], c[\"C\"]\n"),
                "Radar has no curves", "a radar without curves");
    expectError(build(mermaidext::Kind::Radar, "radar-beta\n    nope\n"),
                "Unknown radar statement", "an unknown radar statement");
    expectError(build(mermaidext::Kind::Radar, "radar-beta\n    max lots\n"),
                "Bad max", "a non-numeric maximum");
    expectError(build(mermaidext::Kind::Radar, "radar-beta\n    min lots\n"),
                "Bad min", "a non-numeric minimum");
    expectError(build(mermaidext::Kind::Radar, "radar-beta\n    ticks lots\n"),
                "Bad ticks", "a non-numeric tick count");
    expectError(build(mermaidext::Kind::Radar, "radar-beta\n    curve a\n"),
                "Expected curve values", "a curve without values");
    expectError(build(mermaidext::Kind::Radar, "radar-beta\n    curve a{x}\n"),
                "Bad curve value", "a non-numeric curve value");

    // sankey
    expectError(build(mermaidext::Kind::Sankey, "pie\n\"A\" : 1\n"),
                "Expected sankey header", "a wrong sankey header");
    expectError(build(mermaidext::Kind::Sankey, "sankey-beta\nA\n"),
                "Bad sankey row", "a row with one column");
    expectError(build(mermaidext::Kind::Sankey, "sankey-beta\nA,B,0\n"),
                "Sankey values must be positive", "a zero flow");
    expectError(build(mermaidext::Kind::Sankey, "sankey-beta\n"),
                "No sankey links", "a header without links");
    expectError(build(mermaidext::Kind::Sankey,
                      "sankey-beta\nA,B,1\nB,A,1\n"),
                "Sankey links form a cycle", "a cyclic flow");

    // block
    expectError(build(mermaidext::Kind::Block, "pie\n\"A\" : 1\n"),
                "Expected block header", "a wrong block header");

    // packet
    expectError(build(mermaidext::Kind::Packet, "pie\n\"A\" : 1\n"),
                "Expected packet header", "a wrong packet header");
    expectError(build(mermaidext::Kind::Packet, "packet-beta\n0-15\n"),
                "Expected bits: label", "a field without a label");
    expectError(build(mermaidext::Kind::Packet, "packet-beta\n0-15: \"a\"\n"
                                                 "40-47: \"b\"\n"),
                "Packet fields must be contiguous", "a gap between fields");
    expectError(build(mermaidext::Kind::Packet, "packet-beta\n0-x: \"a\"\n"),
                "Bad packet bit range", "a non-numeric range");
    expectError(build(mermaidext::Kind::Packet, "packet-beta\ntitle only\n"),
                "No packet fields", "a header without fields");

    // kanban
    expectError(build(mermaidext::Kind::Kanban, "pie\n\"A\" : 1\n"),
                "Expected kanban header", "a wrong kanban header");
    // The first line always opens a column, so a card can only follow one
    auto board = build(mermaidext::Kind::Kanban,
                       "kanban\n    Todo\n        [Card]\n    Done\n");
    check(board.ok, "a board whose first line is a column builds");
    if (!board.ok) std::cerr << "  (" << board.error << ")\n";
    expectError(build(mermaidext::Kind::Kanban, "kanban\n"),
                "No kanban columns", "a board without columns");

    // treemap
    expectError(build(mermaidext::Kind::Treemap, "pie\n\"A\" : 1\n"),
                "Expected treemap header", "a wrong treemap header");
    expectError(build(mermaidext::Kind::Treemap, "treemap-beta\n\"unclosed\n"),
                "Unterminated treemap label", "an unterminated label");
    expectError(build(mermaidext::Kind::Treemap, "treemap-beta\n\"A\" 5\n"),
                "Expected : after treemap label", "a label without a value");
    expectError(build(mermaidext::Kind::Treemap, "treemap-beta\n\"A\": lots\n"),
                "Bad treemap value", "a non-numeric value");
    expectError(build(mermaidext::Kind::Treemap, "treemap-beta\n\"A\": -5\n"),
                "Negative treemap value", "a negative value");
    expectError(build(mermaidext::Kind::Treemap, "treemap-beta\ntitle only\n"),
                "Empty treemap", "a header without entries");
    expectError(build(mermaidext::Kind::Treemap, "treemap-beta\n\"A\"\n"),
                "Treemap has no values", "a label without a number");
}

}  // namespace

int runChartSuite() {
    failures = 0;
    quadrant();
    xychart();
    radar();
    sankey();
    block();
    git();
    pie();
    grids();
    narrative();
    rejections();
    if (failures != 0) {
        std::cerr << failures << " chart family test(s) failed\n";
    }
    return failures;
}
