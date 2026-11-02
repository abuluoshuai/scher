// Construct coverage for the class, state and entity-relationship families.
//
// These three share one translation unit and one marker-drawing switch: the
// class arrows (hollow triangle, filled and hollow diamond, open arrow) and
// the ER cardinalities (one, zero-one, crow's foot and its mixes) each draw
// differently. The generic family test only exercises the default arrow, so
// this suite walks every relation token, every member and annotation form,
// the state kinds and the note placements, plus each rejection.

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

mermaidext::Built umlClass(const char* source) {
    return build(mermaidext::Kind::Class, source);
}

mermaidext::Built state(const char* source) {
    return build(mermaidext::Kind::State, source);
}

mermaidext::Built er(const char* source) {
    return build(mermaidext::Kind::Er, source);
}

void expectError(const mermaidext::Built& built, const char* error,
                 const std::string& label) {
    check(!built.ok, label + " is rejected");
    check(built.error == error,
          label + " reports '" + error + "' (got '" + built.error + "')");
}

// --- class diagrams -------------------------------------------------------

void classRelationMarkers() {
    // Every inheritance and composition token, on both ends, solid and
    // dashed, with and without a label.
    const char* source = R"(classDiagram
    class Animal
    class Duck
    class Pond
    class Feather
    Animal <|-- Duck
    Duck <|-- Mallard : extends
    Animal *-- Feather : owns
    Animal o-- Pond : uses
    Animal <-- Pond : observes
    Pond *-- Duck : holds
    Feather o-- Pond
    Duck ..> Pond : flies to
    Duck ..|> Animal
    Duck ..* Feather
    Duck ..o Pond
    Duck <.. Pond
)";
    auto built = umlClass(source);
    check(built.ok, "every class relation token builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    check(built.width > 0.0f && built.height > 0.0f,
          "the class diagram has an extent");
}

void classMembersAndAnnotations() {
    const char* source = R"(classDiagram
    class Animal {
        +int age
        -String name
        #List~int~ legs
        +swim()
        +eat(food) bool
    }
    class Duck
    <<interface>> Duck
    class Shape {
        <<abstract>>
        +area() double
    }
    <<abstract>> Animal
    Animal : +String nickname
    Duck : +quack()
    note for Duck "a bird"
    style Duck fill:#abc
    cssClass Animal
)";
    auto built = umlClass(source);
    check(built.ok, "members, annotations and notes build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void classSkippedRanks() {
    // A -> B -> C plus A -> C: the short cut skips a rank and has to detour
    // through an exterior lane instead of crossing B.
    auto built = umlClass(
        "classDiagram\n"
        "    A <|-- B\n"
        "    B <|-- C\n"
        "    A <|-- C\n"
        "    A -- D\n");   // a plain association draws no end marker
    check(built.ok, "a rank-skipping class relation builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";

    auto reverse = umlClass(
        "classDiagram\n"
        "    A <|-- B\n"
        "    A <|-- C\n"
        "    B <|-- D\n"
        "    C <|-- D\n"
        "    D <|-- A\n");
    check(reverse.ok, "a wide class graph builds");
    if (!reverse.ok) std::cerr << "  (" << reverse.error << ")\n";
}

void classDirections() {
    for (const char* direction : {"LR", "RL", "BT", "sideways"}) {
        std::string source = "classDiagram\n    direction ";
        source += direction;
        source += "\n    A <|-- B\n    B <|-- C\n    A <|-- C\n";
        auto built = umlClass(source.c_str());
        check(built.ok, std::string("class direction ") + direction + " builds");
        if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    }

    // Two relations between the same pair are drawn side by side, and a
    // relation that skips a rank detours around the boxes between them
    auto parallel = umlClass(
        "classDiagram\n"
        "    A <|-- B : first\n"
        "    A <|-- B : second\n"
        "    B <|-- C\n"
        "    A <|-- C\n");
    check(parallel.ok, "parallel relations with a skip build");
    if (!parallel.ok) std::cerr << "  (" << parallel.error << ")\n";
}

void classNoteForms() {
    // A note inside a class body, and an annotation that is not one of the
    // supported class kinds
    expectError(umlClass("classDiagram\n    class A\n    note for A\n"),
                "Bad class note", "a note for a class without text");
    expectError(umlClass("classDiagram\n    class A\n    note\n"),
                "Bad class note", "a note without text");
    auto unsupported = umlClass("classDiagram\n    <<unsupported>>\n    class A\n");
    check(!unsupported.ok, "an unsupported class annotation is rejected");
    if (!unsupported.ok) {
        check(unsupported.error == "Unsupported annotation",
              "the annotation rejection names the reason (got '" +
                  unsupported.error + "')");
    }
}

void stateSkippedRanks() {
    // A transition that skips a rank routes through an exterior lane
    auto built = state(
        "stateDiagram-v2\n"
        "    [*] --> A\n"
        "    A --> B\n"
        "    B --> C\n"
        "    A --> C\n"
        "    A --> D : second edge\n"
        "    D --> C\n");
    check(built.ok, "a state diagram with skipping transitions builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void classMultiplicities() {
    auto built = umlClass(
        "classDiagram\n"
        "    Customer \"1\" --> \"many\" Order : places\n"
        "    Order \"0..1\" -- Customer\n");
    check(built.ok, "quoted multiplicities build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
}

void classRejects() {
    expectError(umlClass("flowchart TD\nA-->B"),
                "Expected classDiagram header", "wrong header");
    expectError(umlClass("classDiagram\n    note for\n"), "Bad class note",
                "an incomplete note");
    expectError(umlClass("classDiagram\n    namespace ns\n"),
                "Namespaces are not supported", "a namespace");
    expectError(umlClass("classDiagram\n    nope nope\n"),
                "Unsupported class statement", "an unknown statement");
    expectError(umlClass("classDiagram\n    --> B\n"),
                "Relation needs two classes", "a relation without a source");
    expectError(umlClass("classDiagram\n"), "No classes",
                "a header without classes");
}

// --- state diagrams -------------------------------------------------------

void stateKinds() {
    const char* source = R"(stateDiagram-v2
    direction BT
    [*] --> Idle
    state "Waiting for input" as Waiting
    state "Only a description"
    state Plain
    Idle --> Waiting : focus
    Plain --> Only a description
    state Pick <<choice>>
    state Fork <<fork>>
    state Join <<join>>
    Waiting --> Pick
    Pick --> Fork
    Fork --> Join
    Join --> [*]
    Waiting : retries up to three times
    note right of Waiting : keeps the cursor
    note left of Idle : idle note
)";
    auto built = state(source);
    check(built.ok, "state kinds, descriptions and notes build");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    check(built.width > 0.0f && built.height > 0.0f,
          "the state diagram has an extent");
}

void stateDirections() {
    for (const char* direction : {"LR", "RL", "TB", "sideways"}) {
        std::string source = "stateDiagram-v2\n    direction ";
        source += direction;
        source += "\n    [*] --> A\n    A --> B : go\n    B --> [*]\n";
        auto built = state(source.c_str());
        check(built.ok, std::string("direction ") + direction + " builds");
        if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    }
}

void stateRejects() {
    expectError(state("flowchart TD\nA-->B"), "Expected stateDiagram header",
                "wrong header");
    expectError(state("stateDiagram-v2\n    state Outer {\n    }\n"),
                "Composite states are not supported", "a composite state");
    expectError(state("stateDiagram-v2\n    note above A : x\n"),
                "Unsupported note position", "an unknown note side");
    expectError(state("stateDiagram-v2\n    note right of A\n"),
                "Multi-line notes are not supported", "a note without text");
    expectError(state("stateDiagram-v2\n    state \"unterminated as A\n"),
                "Unterminated state description", "an unterminated name");
    expectError(state("stateDiagram-v2\n    --> Idle\n"),
                "Transition needs two states", "a transition without a source");
    expectError(state("stateDiagram-v2\n    nonsense\n"),
                "Unsupported state statement", "an unknown statement");
    expectError(state("stateDiagram-v2\n    direction LR\n"), "No states",
                "a header without states");
}

// --- entity relationship --------------------------------------------------

void erCardinalities() {
    // Each of these maps to a different end marker.
    const char* source = R"(erDiagram
    A ||--o{ B : one-to-many
    A |o--o| C : zero-one
    A }o--o{ D : many-to-many
    A }|--|{ E : many-to-many with bars
    A |--| F : one-to-one
    A o--o G : zero-to-zero
    A |o--|{ H : mixed
    A {
        string name PK
        int age FK
        string code PK, FK
        int legacy
    }
)";
    auto built = er(source);
    check(built.ok, "every ER cardinality builds");
    if (!built.ok) std::cerr << "  (" << built.error << ")\n";
    check(built.width > 0.0f && built.height > 0.0f,
          "the ER diagram has an extent");
}

void erRejects() {
    expectError(er("flowchart TD\nA-->B"), "Expected erDiagram header",
                "wrong header");
    expectError(er("erDiagram\n    nonsense line\n"),
                "Unsupported ER statement", "an unknown statement");
    expectError(er("erDiagram\n    ||--o{ B : x\n"),
                "Relationship needs two entities", "a relation without a source");
    expectError(er("erDiagram\n    {\n"),
                "Entity block without a name", "an unnamed entity block");
    expectError(er("erDiagram\n"), "No entities",
                "a header without entities");

    // A relationship label is optional, and an entity block may hold a
    // bare name attribute (no type).
    auto lenient = er("erDiagram\n    A ||--o{ B\n    A {\n        nickname\n"
                      "        string code PK \"the code\"\n    }\n");
    check(lenient.ok, "an unlabelled relation and a bare attribute build");
    if (!lenient.ok) std::cerr << "  (" << lenient.error << ")\n";
}

}  // namespace

int runUmlSuite() {
    failures = 0;
    classRelationMarkers();
    classMembersAndAnnotations();
    classSkippedRanks();
    classDirections();
    classNoteForms();
    classMultiplicities();
    classRejects();
    stateKinds();
    stateSkippedRanks();
    stateDirections();
    stateRejects();
    erCardinalities();
    erRejects();
    if (failures != 0) {
        std::cerr << failures << " class/state/ER test(s) failed\n";
    }
    return failures;
}
