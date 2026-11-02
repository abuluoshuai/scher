#include "d2d_init.h"
#include "export.h"
#include "input.h"
#include "overlays.h"
#include "print.h"
#include "render.h"
#include "settings.h"
#include "utils.h"

#include "glyph_fonts.h"

#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

using Microsoft::WRL::ComPtr;

// Print, PDF and Word export follow the screen (#256, #257, #258): printed
// pages keep the theme's faces and run between the page margins whatever the
// reading width; Word names the faces the screen draws CJK in.
namespace {
int failures = 0, checks = 0;
void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

std::string utf8(const std::wstring& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                   nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The faces a format draws text in through the document fallback
std::wstring facesOf(App& app, IDWriteTextFormat* format, const std::wstring& text) {
    if (!format) return L"(no format)";
    ComPtr<IDWriteFontCollection> fonts;
    app.dwriteFactory->GetSystemFontCollection(&fonts);
    ComPtr<IDWriteTextLayout> layout;
    app.dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, 2000.0f,
                                        200.0f, &layout);
    if (!layout) return L"(no layout)";
    ComPtr<IDWriteTextLayout2> layout2;
    if (app.fontFallback && SUCCEEDED(layout.As(&layout2))) layout2->SetFontFallback(app.fontFallback);
    glyphfonts::RunFonts runs(fonts.Get());
    layout->Draw(nullptr, &runs, 0, 0);
    return runs.families;
}

std::wstring formatFamily(IDWriteTextFormat* format) {
    wchar_t name[128] = {};
    if (format) format->GetFontFamilyName(name, 128);
    return name;
}

// The w:rFonts a Word export writes for a face
std::string rFonts(const std::wstring& face, const std::wstring& eastAsia) {
    return "<w:rFonts w:ascii=\"" + utf8(face) + "\" w:hAnsi=\"" + utf8(face) +
           "\" w:eastAsia=\"" + utf8(eastAsia) + "\" w:cs=\"" + utf8(face) + "\"/>";
}

bool installed(App& app, const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fonts;
    app.dwriteFactory->GetSystemFontCollection(&fonts);
    UINT32 index = 0;
    BOOL exists = FALSE;
    return fonts && SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

const std::wstring kHan = L"\u4E2D\u6587";

// #257, #258: the preview holds the document in print layout
void printLayout(App& app) {
    const float screenIndent = app.layoutIndent, screenWidth = app.layoutMaxWidth;
    const wchar_t* screenFont = app.theme.fontFamily;
    const std::wstring bodyHan = facesOf(app, app.textFormat, kHan);
    const std::wstring codeHan = facesOf(app, app.codeFormat, kHan);
    check(bodyHan == L"SimSun", "the screen draws the theme's CJK body face");

    app.printPreviewPaper = 0;  // A4, without asking the default printer
    app.printPreviewLandscape = false;
    openPrintPreview(app, app.hwnd);
    check(app.showPrintPreview && app.printLayout, "the preview holds the document in print layout");
    check(std::wstring(app.theme.fontFamily) == L"SimSun" &&
          std::wstring(app.theme.codeFontFamily) == L"Courier New" &&
          formatFamily(app.textFormat) == L"SimSun" &&
          formatFamily(app.codeFormat) == L"Courier New",
          "print keeps the theme's body and code faces");
    check(facesOf(app, app.textFormat, kHan) == bodyHan &&
          facesOf(app, app.codeFormat, kHan) == codeHan &&
          facesOf(app, app.codeFormat, L"code") == L"Courier New",
          "printed CJK and code land in the faces the screen uses");
    check(app.theme.background.r == 1.0f && app.theme.background.g == 1.0f &&
          app.theme.background.b == 1.0f, "print keeps its white paper palette");

    const float contentW = app.printPreviewPageW - 2 * 72.0f;
    check(app.layoutIndent == 0.0f && std::abs(app.layoutMaxWidth - contentW) < 0.5f,
          "printed text spans the page margins despite a 70% reading width");
    check(!app.headings.empty() && app.headings[0].y < 40.0f,
          "the first page starts at its top margin, not below a title strip");
    int shrunk = 0;
    check(app.codeBlocks.size() == 3, "the fixture prints its three code blocks");
    for (const auto& block : app.codeBlocks) {
        // The card is the background rect at the block's top-left; long
        // lines widen it past the column
        const auto& b = block.bounds;
        float right = b.right;
        for (const auto& r : app.layoutRects) {
            if (std::abs(r.rect.top - b.top) < 0.5f && std::abs(r.rect.left - b.left) < 0.5f)
                right = std::max(right, r.rect.right);
        }
        check(std::abs(b.left) < 0.5f, "every code block starts at the left margin");
        if (right <= contentW + 0.5f) {
            check(std::abs(right - contentW) < 0.5f, "a code block ends at the right margin");
            continue;
        }
        ++shrunk;
        bool aligned = false;
        for (const auto& band : app.printShrinkBands) {
            if (b.top >= band.top - 0.5f && b.bottom <= band.bottom + 0.5f)
                aligned = std::abs(right * band.scale - contentW) < 1.0f;
        }
        check(aligned, "a code block wider than the page shrinks to the same margins");
    }
    check(shrunk == 1, "the wide code block is the one that shrinks");

    closePrintPreview(app, app.hwnd);
    check(!app.printLayout && app.theme.fontFamily == screenFont &&
          app.layoutIndent == screenIndent && app.layoutMaxWidth == screenWidth,
          "closing the preview restores the screen's theme and column");

    float height = 300.0f;
    ID2D1Bitmap* peek = renderPeekBitmap(app, toWide(app.currentFile), 400.0f, height);
    check(peek != nullptr && !app.printLayout, "the link peek still renders as the screen does");
    if (peek) peek->Release();
}

// #257: the preview's margin presets re-lay the pages out and are saved
void printMargins(App& app) {
    app.printPreviewPaper = 0;
    app.printPreviewLandscape = false;
    openPrintPreview(app, app.hwnd);
    const float pageW = app.printPreviewPageW;
    for (int i : {0, 2, 1}) {
        app.renderTarget->BeginDraw();
        renderPrintPreview(app);
        check(SUCCEEDED(app.renderTarget->EndDraw()), "the print preview renders");
        const D2D1_RECT_F chip = app.printPreviewMarginBtn[i];
        check(chip.right > chip.left, "the preview shows the margin presets");
        const LPARAM point = MAKELPARAM((int)((chip.left + chip.right) / 2),
                                        (int)((chip.top + chip.bottom) / 2));
        handleMouseDown(app, app.hwnd, MK_LBUTTON, point);
        handleMouseUp(app, app.hwnd, 0, point);
        const float margin = PRINT_MARGINS_MM[i] * 96.0f / 25.4f;
        check(app.showPrintPreview && std::abs(app.printMarginMm - PRINT_MARGINS_MM[i]) < 0.01f &&
              std::abs(app.layoutMaxWidth - (pageW - 2 * margin)) < 0.5f,
              "a margin preset re-lays the preview out between its margins");
        check(std::abs(loadSettings().printMarginMm - PRINT_MARGINS_MM[i]) < 0.01f,
              "the chosen margin is saved");
    }
    closePrintPreview(app, app.hwnd);

    // settings.ini takes any margin from 5 to 50 mm and ignores the rest
    const std::filesystem::path ini = std::filesystem::current_path() / L"settings.ini";
    const std::string saved = read(ini);
    for (const auto& [value, expected] : {std::pair<const char*, float>{"15", 15.0f},
                                         {"3", 19.05f}, {"60", 19.05f}, {"abc", 19.05f}}) {
        std::ofstream(ini, std::ios::binary) << "[Settings]\nprintMarginMm=" << value << "\n";
        check(std::abs(loadSettings().printMarginMm - expected) < 0.01f,
              std::string("settings.ini printMarginMm=") + value + " loads as expected");
    }
    std::ofstream(ini, std::ios::binary) << saved;
}

// #256: Word names the faces the screen draws CJK in
void wordFonts(App& app, const std::filesystem::path& dir) {
    const std::wstring bodyHan = facesOf(app, app.textFormat, kHan);
    const std::wstring codeHan = facesOf(app, app.codeFormat, kHan);
    check(exportDocxFile(app, (dir / L"theme-fonts.docx").wstring()), "the Word export succeeds");
    std::string docx = read(dir / L"theme-fonts.docx");
    check(docx.find("<w:rPrDefault><w:rPr>" + rFonts(L"SimSun", bodyHan)) != std::string::npos,
          "Word sets body CJK in the theme's CJK face");
    const std::string code = rFonts(L"Courier New", codeHan);
    const size_t first = docx.find(code);
    check(first != std::string::npos && docx.find(code, first + 1) != std::string::npos,
          "Word sets code blocks and inline code CJK in the face the screen falls back to");

    const D2DTheme saved = app.theme;
    app.theme.fontFamily = L"Georgia";
    app.theme.headingFontFamily = L"Cambria";
    updateTextFormats(app);
    const std::wstring latinHan = facesOf(app, app.textFormat, kHan);
    const std::wstring headingHan = facesOf(app, app.headingFormats[0], kHan);
    check(latinHan != L"Georgia" && !latinHan.empty(), "a Latin body face falls back for CJK");
    check(exportDocxFile(app, (dir / L"latin-fonts.docx").wstring()), "the Latin-face export succeeds");
    docx = read(dir / L"latin-fonts.docx");
    check(docx.find("<w:rPrDefault><w:rPr>" + rFonts(L"Georgia", latinHan)) != std::string::npos,
          "a Latin body face names the CJK face the screen falls back to");
    check(docx.find("<w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr>" + rFonts(L"Cambria", headingHan)) !=
              std::string::npos,
          "headingfont= reaches Word's heading styles");
    app.theme = saved;
    updateTextFormats(app);
}

// #export_as: the standalone HTML keeps the theme, the diagrams as inline
// SVG, math through the TeX layout engine and local images as data URIs
void htmlExport(App& app, const std::filesystem::path& dir) {
    const std::filesystem::path doc = dir / L"html-export-rich.md";
    std::error_code copyError;
    std::filesystem::remove(doc, copyError);
    std::filesystem::copy_file(TINTA_HTML_EXPORT_FIXTURE, doc, copyError);
    // A 1x1 transparent PNG so the local image is embedded, not linked
    static const unsigned char kDotPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    const std::filesystem::path dot = dir / L"dot.png";
    const std::filesystem::path dot2 = dir / L"dot2.png";
    std::ofstream(dot, std::ios::binary)
        .write(reinterpret_cast<const char*>(kDotPng), sizeof(kDotPng));
    // Same image plus one trailing byte, so the base64 encoder has to pad a
    // two-byte tail as well as a one-byte one
    std::string padded(reinterpret_cast<const char*>(kDotPng), sizeof(kDotPng));
    padded += '\0';
    std::ofstream paddedOut(dot2, std::ios::binary);
    paddedOut.write(padded.data(), (std::streamsize)padded.size());
    paddedOut.close();

    check(openDocumentInViewer(app, doc.wstring()),
          "the HTML export fixture opens");
    ensureLayoutComplete(app);

    const std::filesystem::path out = dir / L"rich.html";
    check(exportHtmlFile(app, out.wstring()), "the HTML export succeeds");
    const std::string html = read(out);
    check(html.rfind("<!doctype html>", 0) == 0, "the export is a standalone document");
    check(html.find("<title>html-export-rich.md</title>") != std::string::npos,
          "the export title is the document name");
    check(html.find("<meta charset=\"utf-8\"/>") != std::string::npos,
          "the export declares its encoding");
    check(html.find(".alert-caution") != std::string::npos,
          "the theme CSS carries the alert palette");
    check(html.find("mark{background:") != std::string::npos,
          "the theme CSS carries the highlight colour");
    check(html.find(".footnote{position:relative") != std::string::npos,
          "footnote styling is added only when the document has footnotes");
    check(html.find("<h1>") != std::string::npos &&
              html.find("<h3>") != std::string::npos,
          "headings keep their levels");
    check(html.find("<strong>") != std::string::npos &&
              html.find("<em>") != std::string::npos &&
              html.find("<mark>") != std::string::npos &&
              html.find("<del>") != std::string::npos &&
              html.find("<sup>") != std::string::npos &&
              html.find("<sub>") != std::string::npos,
          "inline emphasis is carried over");
    check(html.find("title=\"Example title\"") != std::string::npos,
          "link titles survive the export");
    check(html.find("class=\"wikilink\"") != std::string::npos,
          "wiki links become styled text, not dead links");
    check(html.find("<br/>") != std::string::npos, "hard breaks survive");
    check(html.find("<blockquote class=\"alert-note\">") != std::string::npos &&
              html.find("class=\"alert-title\"") != std::string::npos &&
              html.find("<blockquote>") != std::string::npos,
          "alerts and plain quotes both export");
    check(html.find("<ol start=\"3\">") != std::string::npos,
          "an ordered list keeps its start number");
    check(html.find("class=\"task\"") != std::string::npos &&
              html.find("disabled checked") != std::string::npos,
          "task list items export as disabled checkboxes");
    check(html.find("class=\"language-c\"") != std::string::npos,
          "fenced code keeps its language");
    check(html.find("<div class=\"diagram\">") != std::string::npos &&
              html.find("<svg") != std::string::npos,
          "diagrams export as inline SVG");
    check(html.find("<pre><code class=\"language-mermaid\">") != std::string::npos,
          "an unsupported diagram falls back to its source");
    check(html.find("class=\"math-inline\"") != std::string::npos &&
              html.find("class=\"math-display\"") != std::string::npos,
          "math exports through the TeX layout engine");
    check(html.find("<code>$\\notarealcommand{$</code>") != std::string::npos,
          "unsupported TeX falls back to the source");
    check(html.find("<table>") != std::string::npos &&
              html.find("align=\"center\"") != std::string::npos &&
              html.find("align=\"right\"") != std::string::npos,
          "table alignment is carried over");
    check(html.find("<hr/>") != std::string::npos, "rules survive");
    check(html.find("raw HTML block") != std::string::npos,
          "raw HTML blocks keep their text");
    check(html.find("<a id=\"anchor-target\"></a>") != std::string::npos,
          "explicit anchors survive");
    check(html.find("<ruby>") != std::string::npos &&
              html.find("<rt>kan</rt>") != std::string::npos,
          "ruby annotations survive");
    check(html.find("<img src=\"data:image/png;base64,") != std::string::npos,
          "a local image is embedded as a data URI");
    check(html.find("missing.png") != std::string::npos,
          "an unreadable image keeps its path");
    check(html.find("https://example.com/remote.png") != std::string::npos,
          "a remote image stays a URL");
    check(html.find("C:/nonexistent/thing.png") != std::string::npos,
          "an absolute image path is not prefixed with the document folder");
    check(html.find("<section class=\"footnotes\"") != std::string::npos,
          "footnotes export as an endnote section");

    // A document without footnotes skips the endnote stylesheet
    const std::filesystem::path plain = dir / L"plain.md";
    std::ofstream(plain, std::ios::binary) << "# Plain\n\nJust a paragraph.\n";
    check(openDocumentInViewer(app, plain.wstring()), "the plain fixture opens");
    const std::filesystem::path plainOut = dir / L"plain.html";
    check(exportHtmlFile(app, plainOut.wstring()), "the plain export succeeds");
    const std::string plainHtml = read(plainOut);
    check(plainHtml.find(".footnote{position:relative") == std::string::npos,
          "a document without footnotes has no endnote styling");

    // Nothing open: the writer refuses instead of emitting an empty page
    auto saved = app.root;
    app.root = nullptr;
    check(!exportHtmlFile(app, (dir / L"empty.html").wstring()),
          "exporting without a document fails");
    app.root = saved;
    check(openDocumentInViewer(app, doc.wstring()),
          "the rich fixture reopens for the remaining checks");
    ensureLayoutComplete(app);
}
}  // namespace

int runPrintExportTests() {
    namespace fs = std::filesystem;
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Print and export tests", WS_POPUP, 0, 0, 1050,
                               900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) return 2;
    app.width = 1050;
    app.height = 900;
    updateTextFormats(app);
    if (!installed(app, L"SimSun") || !installed(app, L"Courier New") ||
        !installed(app, L"Georgia") || !installed(app, L"Cambria")) {
        std::cout << "Print and export: skipped, a fixture font is not installed\n";
        return 0;
    }

    // The reporter's setup: a custom theme with a CJK body face and a
    // Latin-only code face, read from themes.ini like any user theme
    const fs::path dir = fs::current_path();
    // libstdc++ on MinGW throws "File exists" from copy_file even with
    // overwrite_existing when the run directory is reused, so remove first
    // and never let a filesystem error escape as an exception.
    std::error_code copyError;
    fs::remove(dir / L"themes.ini", copyError);
    fs::copy_file(TINTA_PRINT_EXPORT_THEMES, dir / L"themes.ini", copyError);
    loadCustomThemes();
    int theme = -1;
    for (int i = 0; i < themeCount(); ++i) {
        if (std::wstring(themeAt(i).name) == L"Arctic SimSun") theme = i;
    }
    check(theme >= 0, "the fixture theme loads from themes.ini");
    if (theme < 0) return 1;
    applyTheme(app, theme);
    app.readingWidthPct = 70;
    check(openDocumentInViewer(app, fs::path(TINTA_PRINT_EXPORT_FIXTURE).wstring()),
          "the mixed fixture opens");
    ensureLayoutComplete(app);
    check(app.codeBlocks.size() == 3 && !app.headings.empty(), "the fixture lays out on screen");

    printLayout(app);
    printMargins(app);
    wordFonts(app, dir);
    htmlExport(app, dir);

    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
    state.reset();
    CoUninitialize();
    std::cout << "Print and export: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
