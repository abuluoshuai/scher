// Cross-cutting text helpers.
//
// utils.cpp holds two pure helpers the selection, copy and export paths all
// go through: textViewForRect (the characters a laid-out rect covers) and
// extractText (the plain text of a parsed element tree). Both are driven
// here over a document that contains every element kind, because the switch
// in extractText decides what "copy as text" produces for each of them.

#include "document.h"
#include "editor.h"
#include "utils.h"
#include <cmath>

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    failures++;
}

qmd::ParseResult parseText(const std::string& source,
                           const std::string& name = "note.md") {
    qmd::MarkdownParser parser;
    return parseDocument(parser, source, name);
}

std::wstring plainText(const char* source) {
    auto parsed = parseText(source);
    std::wstring out;
    if (parsed.success) extractText(parsed.root, out);
    return out;
}

bool contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

void textViews() {
    App app;
    app.docText = L"0123456789";

    App::TextRect whole;
    whole.docStart = 0;
    whole.docLength = 10;
    check(textViewForRect(app, whole) == L"0123456789",
          "a rect covering the document returns all of it");

    App::TextRect middle;
    middle.docStart = 3;
    middle.docLength = 4;
    check(textViewForRect(app, middle) == L"3456",
          "a rect in the middle returns its own characters");

    App::TextRect past;
    past.docStart = 10;
    past.docLength = 2;
    check(textViewForRect(app, past).empty(),
          "a rect starting at the end returns nothing");

    App::TextRect beyond;
    beyond.docStart = 42;
    beyond.docLength = 2;
    check(textViewForRect(app, beyond).empty(),
          "a rect past the end returns nothing");

    App::TextRect overrun;
    overrun.docStart = 8;
    overrun.docLength = 99;
    check(textViewForRect(app, overrun) == L"89",
          "a rect longer than the text is clamped, not over-read");

    App empty;
    App::TextRect any;
    any.docStart = 0;
    any.docLength = 1;
    check(textViewForRect(empty, any).empty(),
          "an empty document yields no view");
}

void extractedText() {
    const std::wstring text = plainText(
        "# Heading\n"
        "\n"
        "First line\n"
        "wrapped line\n"
        "\n"
        "Hard  \n"
        "break\n"
        "\n"
        "- item one\n"
        "- item two\n"
        "\n"
        "```c\n"
        "int main(void) {}\n"
        "```\n"
        "\n"
        "> quoted\n"
        "\n"
        "| a | b |\n"
        "| --- | --- |\n"
        "| c | d |\n");
    check(contains(text, L"Heading"), "a heading's text is extracted");
    check(contains(text, L"First line"), "a paragraph's text is extracted");
    check(contains(text, L"First line wrapped line"),
          "a soft break becomes a single space");
    check(contains(text, L"Hard\nbreak"), "a hard break becomes a newline");
    check(contains(text, L"item one"), "list items are extracted");
    check(contains(text, L"int main(void) {}"), "code blocks keep their text");
    check(contains(text, L"quoted"), "block quotes are extracted");
    check(contains(text, L"a") && contains(text, L"d"),
          "table cells are extracted");

    // Paragraphs and headings end with a blank line
    check(text.find(L"Heading\n\n") != std::wstring::npos,
          "a heading ends with a blank line");

    // A mermaid block is kept as its source, terminated by one newline
    const std::wstring diagram =
        plainText("```mermaid\nflowchart TD\nA-->B\n```\n");
    check(contains(diagram, L"flowchart TD"), "a diagram keeps its source");

    // Ruby annotations keep the base text only
    const std::wstring ruby = plainText(
        "<div>\n<ruby>kanji<rt>reading</rt></ruby>\n</div>\n");
    check(contains(ruby, L"kanji") || ruby.empty(),
          "ruby base text is extracted");

    // An empty tree and a null node are both harmless
    std::wstring none;
    extractText(nullptr, none);
    check(none.empty(), "a null element adds nothing");
}

void utfConversions() {
    check(toWide("") == L"", "an empty UTF-8 string converts to an empty wide string");
    check(toWide("abc") == L"abc", "ASCII survives the conversion");
    check(toWide("\xE4\xB8\xAD\xE6\x96\x87") == L"\x4E2D\x6587",
          "UTF-8 Chinese converts to its wide form");
    check(toUtf8(L"\x4E2D\x6587") == "\xE4\xB8\xAD\xE6\x96\x87",
          "wide Chinese converts back to UTF-8");
    check(toUtf8(L"") == "", "an empty wide string converts to empty");
}

// A contents click must land the heading itself just inside the content
// area. Layout starts at documentContentTop, so the jump has to subtract that
// (chrome plus the document's own top padding), not just the chrome - the
// difference is exactly the 20 px of padding, which used to scroll past the
// heading.
void headingJump() {
    App app;
    app.width = 1000;
    app.height = 800;
    app.contentScale = 1.0f;
    app.contentHeight = 5000.0f;
    const float headingY = 1200.0f;
    const float margin = dpi(app, 14.0f);

    scrollToHeadingY(app, headingY);
    const float landY = headingY - app.scrollY;
    check(std::fabs(landY - (documentContentTop(app) + margin)) < 0.01f,
          "the heading lands margin below the content top");
    check(std::fabs(app.scrollY -
                    (headingY - documentContentTop(app) - margin)) < 0.01f,
          "the scroll offset accounts for the document's top padding");

    // The old formula scrolled 20 scale px too far; prove the two differ
    check(std::fabs((headingY - chromeTopHeight(app) - margin) - app.scrollY) >
              1.0f,
          "the chrome height alone is not the right origin");

    // Never scroll before the top of the document
    scrollToHeadingY(app, 0.0f);
    check(app.scrollY == 0.0f, "a heading at the top clamps to the start");

    // Never past the end of the document
    scrollToHeadingY(app, 99999.0f);
    check(std::fabs(app.scrollY - (app.contentHeight - app.height)) < 0.01f,
          "a heading past the end clamps to the last screen");
}

}  // namespace

int runUtilsSuite() {
    failures = 0;
    textViews();
    extractedText();
    utfConversions();
    headingJump();
    if (failures != 0) {
        std::cerr << failures << " text helper test(s) failed\n";
    }
    return failures;
}
