// Markdown parse-tree details the renderer depends on.
//
// md4c hands the parser text runs that are not plain text: HTML entities
// stay encoded, anchors and presentational tags arrive as inline HTML,
// hard breaks can end a paragraph, and the parser adds two syntaxes of its
// own on top (wiki links and a single-tilde subscript). This suite pins
// those conversions down so the export, print and editor trees keep seeing
// the elements they expect.

#include "document.h"

#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    failures++;
}

using qmd::Element;
using qmd::ElementPtr;
using qmd::ElementType;

qmd::ParseResult parseText(const std::string& source,
                           const std::string& name = "note.md") {
    qmd::MarkdownParser parser;
    return parseDocument(parser, source, name);
}

// First descendant of the given type, in document order
const Element* findFirst(const Element* root, ElementType type) {
    if (!root) return nullptr;
    if (root->type == type) return root;
    for (const auto& child : root->children) {
        if (const Element* hit = findFirst(child.get(), type)) return hit;
    }
    return nullptr;
}

// Concatenated text of every Text node, in document order
void collectText(const Element* elem, std::string& out) {
    if (!elem) return;
    if (elem->type == ElementType::Text) out += elem->text;
    for (const auto& child : elem->children) collectText(child.get(), out);
}

std::string visibleText(const Element* root) {
    std::string text;
    collectText(root, text);
    return text;
}

void entities() {
    auto result = parseText("a &lt; b &gt; c &amp; d &quot;e&quot; f&nbsp;g &copy;\n");
    check(result.success, "a paragraph of entities parses");
    const std::string text = visibleText(result.root.get());
    check(text.find("a < b > c & d \"e\" f g") != std::string::npos,
          "the five common entities are decoded (" + text + ")");
    check(text.find("&copy;") != std::string::npos,
          "an unknown entity stays as written");
}

void titles() {
    auto link = parseText("[label](https://example.com \"Link title\")\n");
    check(link.success, "a link with a title parses");
    const Element* anchor = findFirst(link.root.get(), ElementType::Link);
    check(anchor != nullptr, "the link element exists");
    if (anchor) {
        check(anchor->url == "https://example.com", "the link url is captured");
        check(anchor->title == "Link title", "the link title is captured");
    }

    auto image = parseText("![alt text](picture.png \"Image title\")\n");
    check(image.success, "an image with a title parses");
    const Element* picture = findFirst(image.root.get(), ElementType::Image);
    check(picture != nullptr, "the image element exists");
    if (picture) {
        check(picture->url == "picture.png", "the image url is captured");
        check(picture->title == "Image title", "the image title is captured");
    }
}

void wikiLinks() {
    auto result = parseText("See [[Target note]] and [[Other|shown text]].\n");
    check(result.success, "wiki links parse");
    const Element* first = findFirst(result.root.get(), ElementType::Link);
    check(first != nullptr, "a wiki link becomes a link element");
    if (first) {
        check(first->url == "wiki:Target note", "the wiki target is kept in the url");
        check(visibleText(first) == "Target note",
              "a pipe-less wiki link shows its target");
    }
    int links = 0;
    std::function<void(const Element*)> walk = [&](const Element* elem) {
        if (elem->type == ElementType::Link) links++;
        for (const auto& child : elem->children) walk(child.get());
    };
    walk(result.root.get());
    check(links == 2, "both wiki links become links");

    // Degenerate forms stay literal text
    auto broken = parseText("empty [[]] and blank [[|shown]] and tail [[open\n");
    check(broken.success, "degenerate wiki links parse");
    const std::string text = visibleText(broken.root.get());
    check(findFirst(broken.root.get(), ElementType::Link) == nullptr,
          "empty or unclosed wiki links stay text");
    check(text.find("[[]]") != std::string::npos &&
              text.find("[[|shown]]") != std::string::npos,
          "the literal brackets survive");
}

void subscriptsAndStrikethrough() {
    auto result = parseText("H~2~O and ~~gone~~ and a ~ b ~ c\n");
    check(result.success, "sub and strike markers parse");
    check(findFirst(result.root.get(), ElementType::Subscript) != nullptr,
          "a single-tilde pair becomes a subscript");
    check(findFirst(result.root.get(), ElementType::Strikethrough) != nullptr,
          "a double-tilde pair becomes strikethrough");
    const std::string text = visibleText(result.root.get());
    check(text.find("a ~ b ~ c") != std::string::npos,
          "unpaired tildes stay literal");
}

void hardBreaks() {
    auto result = parseText("first line  \nsecond line\ntrailing spaces  \n\n");
    check(result.success, "a paragraph with trailing spaces parses");
    check(findFirst(result.root.get(), ElementType::HardBreak) != nullptr,
          "two trailing spaces make a hard break");
    check(findFirst(result.root.get(), ElementType::SoftBreak) != nullptr,
          "a plain newline stays a soft break");
}

void anchorTags() {
    auto open = parseText("before <a id=\"target\"></a> after\n");
    check(open.success, "an anchor parses");
    const Element* anchor = findFirst(open.root.get(), ElementType::Anchor);
    check(anchor != nullptr, "a named anchor becomes an Anchor element");
    if (anchor) check(anchor->title == "target", "the anchor keeps its id");

    auto named = parseText("x <a name=\"legacy\">y</a> z\n");
    check(findFirst(named.root.get(), ElementType::Anchor) != nullptr,
          "a name= anchor is recognised too");

    auto selfClosed = parseText("p <a id=\"dot\"/> q\n");
    check(selfClosed.success, "a self-closed anchor parses");
    check(findFirst(selfClosed.root.get(), ElementType::Anchor) != nullptr,
          "a self-closed anchor sets its target");

    // An ordinary link keeps being a link, and a stray close stays text
    auto href = parseText("<a href=\"https://example.com\">site</a>\n");
    check(href.success, "an html link parses");
    const std::string hrefText = visibleText(href.root.get());
    check(hrefText.find("site") != std::string::npos,
          "an html link keeps its text");

    auto strayClose = parseText("text </a> more\n");
    check(visibleText(strayClose.root.get()).find("</a>") != std::string::npos,
          "an unmatched </a> stays literal text");
}

void fontTags() {
    // Producers that wrap whole tables in presentational <font> tags: the
    // tag has no meaning for the native renderer and would otherwise show
    // up as text and inflate the column widths.
    auto table = parseText(
        "| Column |\n| --- |\n| <font color=\"red\">cell text</font> |\n");
    check(table.success, "a table with font wrappers parses");
    const std::string tableText = visibleText(table.root.get());
    check(tableText.find("cell text") != std::string::npos,
          "the wrapped cell content survives");
    check(tableText.find("font") == std::string::npos,
          "a transparent <font> wrapper in a cell is dropped");

    // A tag that merely starts with "font" is not a font tag
    auto lookalike = parseText("a <fontface>x</fontface> b\n");
    check(lookalike.success, "a lookalike tag parses");
    check(visibleText(lookalike.root.get()).find("fontface") != std::string::npos,
          "a tag that only starts with font stays text");
}

void parseFileErrors() {
    qmd::MarkdownParser parser;
    auto missing = parser.parseFile("this-file-does-not-exist.md");
    check(!missing.success, "a missing file fails to parse");
    check(missing.error.find("Failed to open file") != std::string::npos,
          "the failure names the file problem");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "tinta-parse-file-test.md";
    {
        std::ofstream out(path, std::ios::binary);
        out << "# From disk\n\nBody text.\n";
    }
    auto loaded = parser.parseFile(path.string());
    check(loaded.success, "an existing file parses");
    check(findFirst(loaded.root.get(), ElementType::Heading) != nullptr,
          "the file content reached the tree");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void typeNames() {
    using qmd::elementTypeToString;
    struct Case { ElementType type; const char* name; };
    const Case kCases[] = {
        {ElementType::Document, "Document"},
        {ElementType::Paragraph, "Paragraph"},
        {ElementType::Heading, "Heading"},
        {ElementType::Text, "Text"},
        {ElementType::Code, "Code"},
        {ElementType::CodeBlock, "CodeBlock"},
        {ElementType::BlockQuote, "BlockQuote"},
        {ElementType::List, "List"},
        {ElementType::ListItem, "ListItem"},
        {ElementType::Table, "Table"},
        {ElementType::Link, "Link"},
        {ElementType::Image, "Image"},
        {ElementType::HorizontalRule, "HorizontalRule"},
        {ElementType::SoftBreak, "SoftBreak"},
        {ElementType::HardBreak, "HardBreak"},
        {ElementType::Emphasis, "Emphasis"},
        {ElementType::Strong, "Strong"},
        {ElementType::MermaidDiagram, "MermaidDiagram"},
        {ElementType::Highlight, "Highlight"},
        {ElementType::Strikethrough, "Strikethrough"},
        {ElementType::Superscript, "Superscript"},
        {ElementType::Subscript, "Subscript"},
        {ElementType::TableRow, "TableRow"},
        {ElementType::TableCell, "TableCell"},
        {ElementType::HtmlBlock, "HtmlBlock"},
        {ElementType::Anchor, "Anchor"},
        {ElementType::Ruby, "Ruby"},
        {ElementType::RubyText, "RubyText"},
        // The debug names stop at Anchor; newer element types share the
        // "Unknown" fallback.
        {ElementType::MathInline, "Unknown"},
        {ElementType::MathDisplay, "Unknown"},
        {ElementType::FootnoteReference, "Unknown"},
        {ElementType::FootnoteDefinition, "Unknown"},
    };
    for (const auto& testCase : kCases) {
        check(std::string(elementTypeToString(testCase.type)) == testCase.name,
              std::string("elementTypeToString(") + testCase.name + ")");
    }
}

void plainAndMermaidDocuments() {
    qmd::MarkdownParser parser;
    auto plain = parseDocument(parser, "plain text body\nsecond line\n",
                               "notes.txt");
    check(plain.success, "a plain text document parses");
    check(plain.root != nullptr, "a plain text document has a root");

    auto mermaid = parseDocument(parser, "sequenceDiagram\nA->>B: hi\n",
                                 "diagram.mmd");
    check(mermaid.success, "a mermaid document parses");
    check(findFirst(mermaid.root.get(), ElementType::MermaidDiagram) != nullptr,
          "the whole file becomes one diagram");
}

void alertMarkers() {
    // The GitHub alert marker only counts when it is the whole first line
    auto alert = parseText("> [!NOTE]\n> body\n");
    check(alert.success, "an alert block parses");
    const Element* quote = findFirst(alert.root.get(), ElementType::BlockQuote);
    check(quote && quote->alertKind == 1, "a lone marker sets the alert kind");

    auto inline_ = parseText("> [!NOTE] trailing words\n> body\n");
    check(inline_.success, "an alert with trailing text parses");
    const Element* plain = findFirst(inline_.root.get(), ElementType::BlockQuote);
    check(plain && plain->alertKind == 0,
          "trailing text after the marker leaves a plain quote");

    auto unknown = parseText("> [!BOGUS]\n> body\n");
    const Element* bogus = findFirst(unknown.root.get(), ElementType::BlockQuote);
    check(bogus && bogus->alertKind == 0, "an unknown marker stays a plain quote");
}

void htmlBlocks() {
    // Raw HTML blocks are parsed into the same element tree so the reader
    // can theme them instead of showing tags
    const char* source =
        "<div>\n"
        "<h2>Raw heading</h2>\n"
        "<p>Raw paragraph</p>\n"
        "<ul>\n<li>One</li>\n<li>Two</li>\n</ul>\n"
        "<ol start=\"3\">\n<li>Three</li>\n</ol>\n"
        "<pre><code>int x;</code></pre>\n"
        "<blockquote><p>quoted</p></blockquote>\n"
        "<a id=\"raw-anchor\"></a>\n"
        "<h1>Top</h1>\n<hr>\n<h6>Small</h6>\n"
        "<table><tr><th>H</th></tr><tr><td>D</td></tr></table>\n"
        "<p>An <a href=\"https://example.com\" title=\"T\">html link</a>, "
        "<strong>bold</strong>, <em>italic</em>, <code>code</code>, "
        "<del>gone</del>, <img src=\"x.png\" alt=\"pic\"/>, "
        "and a <br> break.</p>\n"
        "<unknown-tag>stays text</unknown-tag>\n"
        "</div>\n";
    auto result = parseText(source);
    check(result.success, "an HTML block parses");
    check(findFirst(result.root.get(), ElementType::Heading) != nullptr,
          "a raw heading becomes a heading element");
    check(findFirst(result.root.get(), ElementType::List) != nullptr,
          "raw lists become list elements");
    check(findFirst(result.root.get(), ElementType::CodeBlock) != nullptr,
          "a raw pre/code becomes a code block");
    // Raw <table> markup is not in the HTML reader's element set: its cell
    // text still has to reach the reader as text, never as visible tags
    check(visibleText(result.root.get()).find("stays text") != std::string::npos,
          "an unknown raw tag keeps its content");
    check(findFirst(result.root.get(), ElementType::Anchor) != nullptr,
          "a raw anchor keeps its target");
}

void debugPrinter() {
    // The development helper prints the tree; it must walk every element and
    // print the fields each node carries without crashing on null children
    auto result = parseText("# Head\n\nA [link](https://example.com) and `code`.\n");
    check(result.success, "the debug fixture parses");
    qmd::debugPrintElement(result.root, 0);
    qmd::debugPrintElement(nullptr, 0);
    check(true, "debugPrintElement walks a real tree and a null root");
}

void tildeRetries() {
    // A double tilde that never closes must not swallow the single-tilde
    // subscript that follows it
    auto result = parseText("a ~~unclosed and ~sub~ text\n");
    check(result.success, "a half-open strikethrough parses");
    check(findFirst(result.root.get(), ElementType::Subscript) != nullptr,
          "the later single-tilde pair still becomes a subscript");
    check(visibleText(result.root.get()).find("~~unclosed") != std::string::npos,
          "the unpaired double tilde stays literal");
}

// HTML attribute subset (scheme A): img/div/p accept align/src/alt/style,
// three CSS declarations are honored, everything else in style is ignored.
void htmlAttributes() {
    qmd::MarkdownParser parser;
    auto result = parseText(
        "<div align=\"center\" style=\"color: #cc3333\">centered red</div>\n");
    const Element* div = findFirst(result.root.get(), ElementType::HtmlDiv);
    check(div != nullptr, "a styled div becomes an HtmlDiv element");
    if (div) {
        check(div->htmlAlign == 2, "div align=center maps to align code 2");
        check(div->styleColorR == 0xcc && div->styleColorG == 0x33 &&
              div->styleColorB == 0x33,
              "style color:#cc3333 parses into the RGB fields");
        check(visibleText(result.root.get()).find("centered red") !=
                  std::string::npos,
              "the div's text survives");
    }

    auto img = parseText(
        "<img src=\"images/badge.png\" alt=\"badge\" />\n");
    const Element* image = findFirst(img.root.get(), ElementType::Image);
    check(image != nullptr, "an HTML <img> becomes an Image element");
    if (image) {
        check(image->url == "images/badge.png", "img src lands in url");
        check(image->title == "badge", "img alt lands in title");
    }

    auto centeredImg = parseText(
        "<div align=\"center\"><img src=\"x.png\" /></div>\n");
    const Element* imgInDiv = findFirst(centeredImg.root.get(),
                                        ElementType::Image);
    check(imgInDiv != nullptr, "an img inside a centered div is kept");
    if (imgInDiv) {
        check(imgInDiv->parent &&
              imgInDiv->parent->type == ElementType::HtmlDiv &&
              imgInDiv->parent->htmlAlign == 2,
              "the wrapping div carries align=center for the renderer");
    }

    auto styled = parseText(
        "<p style=\"font-size: 20pt; text-align: bogus; margin: 0\">big</p>\n");
    const Element* para = findFirst(styled.root.get(), ElementType::Paragraph);
    check(para != nullptr && para->styleSizePt == 20.0f,
          "font-size parses and unknown declarations are ignored");

    auto plain = parseText("<div>just content</div>\n");
    check(findFirst(plain.root.get(), ElementType::HtmlDiv) == nullptr,
          "a div without align/style stays transparent");
}

}  // namespace

int runMarkdownParseSuite() {
    failures = 0;
    entities();
    titles();
    wikiLinks();
    subscriptsAndStrikethrough();
    hardBreaks();
    anchorTags();
    fontTags();
    parseFileErrors();
    typeNames();
    alertMarkers();
    htmlBlocks();
    htmlAttributes();
    debugPrinter();
    tildeRetries();
    plainAndMermaidDocuments();
    if (failures != 0) {
        std::cerr << failures << " markdown parse test(s) failed\n";
    }
    return failures;
}
