// Construct coverage for the sequence and gantt families.
//
// The generic per-family test builds one representative diagram; these
// cases walk each statement the parsers understand (participant aliases,
// autonumbering, activations, note placements, frame dividers, task tags,
// id references, "after" chains, numeric date formats) and each rejection,
// so both the drawing columns and the error paths are executed.

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

mermaidext::Built build(mermaidext::Kind kind, const char* source) {
    return mermaidext::build(kind, source, fakeMeasure, 1.0f);
}

mermaidext::Built sequence(const char* source) {
    return build(mermaidext::Kind::Sequence, source);
}

mermaidext::Built gantt(const char* source) {
    return build(mermaidext::Kind::Gantt, source);
}

void expectError(const mermaidext::Built& built, const char* error,
                 const std::string& label) {
    check(!built.ok, label + " is rejected");
    check(built.error == error,
          label + " reports '" + error + "' (got '" + built.error + "')");
}

// --- sequence -------------------------------------------------------------

void sequenceParticipants() {
    auto built = sequence(
        "sequenceDiagram\n"
        "    participant A\n"
        "    participant B as Bob\n"
        "    actor C as Carol\n"
        "    participant B as Robert\n"   // later alias wins
        "    A->>B: hi\n"
        "    C-->>A: reply\n"
        "    B->>C: forward\n");
    check(built.ok, "participants with and without aliases build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void sequenceNumberingAndTitle() {
    auto built = sequence(
        "sequenceDiagram\n"
        "    title 订单流程\n"
        "    autonumber\n"
        "    participant A\n"
        "    participant B\n"
        "    A->>B: one\n"
        "    B->>A: two\n"
        "    autonumber off\n"
        "    A->>B: three\n"
        "    link A: Dashboard @ https://example.com\n"
        "    links A: {\"Docs\": \"https://example.com\"}\n"
        "    box Aqua Group\n"
        "    end\n");
    check(built.ok, "numbering, a title and ignored metadata build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void sequenceActivationsAndNotes() {
    auto built = sequence(
        "sequenceDiagram\n"
        "    participant A\n"
        "    participant B\n"
        "    participant C\n"
        "    activate A\n"
        "    A->>+B: request\n"
        "    B-->>-A: reply\n"
        "    deactivate A\n"
        "    note over A: alone\n"
        "    note over C,B: spanning\n"       // reversed pair is normalised
        "    note left of A: left side\n"
        "    note right of C: right side\n"
        "    A->>A: self loop\n");
    check(built.ok, "activations, notes and a self message build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    check(built.width > 0.0f, "the sequence has a width");
}

void sequenceFrames() {
    auto built = sequence(
        "sequenceDiagram\n"
        "    participant A\n"
        "    participant B\n"
        "    loop every minute\n"
        "        A->>B: ping\n"
        "        alt healthy\n"
        "            B-->>A: pong\n"
        "        else unhealthy\n"
        "            B-->>A: error\n"
        "        and unknown\n"
        "            B-->>A: retry\n"
        "        end\n"
        "    end\n"
        "    opt optional\n"
        "        A-)B: async\n"
        "    end\n"
        "    par parallel\n"
        "        A-xB: cross\n"
        "    end\n"
        "    critical must\n"
        "        A-->B: dotted\n"
        "    end\n"
        "    break when broken\n"
        "        A->B: plain\n"
        "    end\n"
        "    rect rgb(200, 220, 255)\n"
        "        A->>B: shaded\n"
        "    end\n");
    check(built.ok, "every frame keyword and divider builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void sequenceRejects() {
    expectError(sequence("flowchart TD\nA-->B"),
                "Expected sequenceDiagram header", "wrong header");
    // Declared participants draw even without a message between them
    auto empty = sequence("sequenceDiagram\n    participant A\n    participant B\n");
    check(empty.ok, "participants without messages still build");
    expectError(sequence("sequenceDiagram\n    title only\n"),
                "No participants", "a diagram without participants");
    expectError(sequence("sequenceDiagram\n    note sideways A: x\n"),
                "Unsupported note position", "unknown note position");
    expectError(sequence("sequenceDiagram\n    note over A\n"),
                "Note without text", "note without a colon");
    expectError(sequence("sequenceDiagram\n    else nobody\n"),
                "Divider outside a frame", "divider outside a frame");
    expectError(sequence("sequenceDiagram\n    A->>B: hi\n    end now\n"),
                "end without an open frame", "end without a frame");
    expectError(sequence("sequenceDiagram\n    A->>B: hi\n    ->B: later\n"),
                "Unsupported sequence statement", "arrow with no sender");
    expectError(sequence("sequenceDiagram\n    A->>B: hi\n    A->>: text\n"),
                "Message needs two participants", "message without a receiver");
    expectError(sequence("sequenceDiagram\n    A->>B: hi\n    nonsense\n"),
                "Unsupported sequence statement", "unknown statement");
}

// --- gantt ----------------------------------------------------------------

void ganttTasks() {
    auto built = gantt(
        "gantt\n"
        "    title 发布计划\n"
        "    dateFormat YYYY-MM-DD\n"
        "    axisFormat %m-%d\n"
        "    excludes weekends\n"
        "    todayMarker off\n"
        "    section 设计\n"
        "        Design :done, d1, 2026-01-01, 5d\n"
        "        Review :active, 2026-01-06, 2d\n"
        "    section 开发\n"
        "        Build :crit, after d1, 3w\n"
        "        Ship :milestone, after Review, 0d\n"
        "        Extend :2026-02-01, 4h\n"
        "        Month :2026-03-01, 2m\n");
    check(built.ok, "tags, ids, after chains and durations build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    check(built.width > 0.0f && built.height > 0.0f, "the gantt has an extent");
}

void ganttDateFormats() {
    auto dayFirst = gantt(
        "gantt\n"
        "    dateFormat DD/MM/YYYY\n"
        "    Task A :01/02/2026, 3d\n"
        "    Task B :05/02/2026, 1d\n");
    check(dayFirst.ok, "a day-first date format builds");
    if (!dayFirst.ok) std::cerr << "  (" << dayFirst.error << ")\n";

    auto numeric = gantt(
        "gantt\n"
        "    dateFormat X\n"
        "    Task A :100, 5d\n"
        "    Task B :after A, 2d\n"
        "    Task C :200\n");
    check(numeric.ok, "numeric dates build");
    if (!numeric.ok) std::cerr << "  (" << numeric.error << ")\n";

    auto shifted = gantt(
        "gantt\n"
        "    dateFormat MM-DD-YYYY\n"
        "    Task :12-31-2026, 1d\n");
    check(shifted.ok, "a month-first date format builds");
    if (!shifted.ok) std::cerr << "  (" << shifted.error << ")\n";
}

void ganttRejects() {
    expectError(gantt("flowchart TD\nA-->B"), "Expected gantt header",
                "wrong header");
    expectError(gantt("gantt\n    dateFormat YYYY\n    Task :2026-01-01, 1d\n"),
                "Unsupported dateFormat", "a format without month and day");
    expectError(gantt("gantt\n    no colon here\n"),
                "Unsupported gantt statement", "a statement without a colon");
    expectError(gantt("gantt\n    Task :no-start, 1d\n"),
                "First task needs a start date",
                "the first task without a start");
    expectError(gantt("gantt\n    Task :2026-01-01, nonsense\n"),
                "Bad task end", "an unparsable end");
    expectError(gantt("gantt\n    title only\n"), "No tasks",
                "a header without tasks");
}

}  // namespace

int runFamilySuite() {
    failures = 0;
    sequenceParticipants();
    sequenceNumberingAndTitle();
    sequenceActivationsAndNotes();
    sequenceFrames();
    sequenceRejects();
    ganttTasks();
    ganttDateFormats();
    ganttRejects();
    if (failures != 0) {
        std::cerr << failures << " family construct test(s) failed\n";
    }
    return failures;
}
