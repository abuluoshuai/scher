#include "d2d_init.h"
#include "drafts.h"
#include "editor.h"
#include "input.h"
#include "render.h"
#include "settings.h"
#include "tabs.h"
#include "utils.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

// Editor conveniences (#251), the word count (#240) and draft recovery
// after a close without asking (#252), driven through the real handlers
namespace {
int failures = 0, checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

struct Modifiers { bool ctrl = false, shift = false, alt = false; };

// The handlers read modifiers with GetKeyState, so set the thread's state
void withKeys(Modifiers mods, const std::function<void()>& action) {
    BYTE original[256], pressed[256];
    GetKeyboardState(original);
    memcpy(pressed, original, sizeof pressed);
    pressed[VK_CONTROL] = mods.ctrl ? 0x80 : 0;
    pressed[VK_SHIFT] = mods.shift ? 0x80 : 0;
    pressed[VK_MENU] = mods.alt ? 0x80 : 0;
    SetKeyboardState(pressed);
    action();
    SetKeyboardState(original);
}
void key(App& app, unsigned vk, Modifiers mods = {}) {
    withKeys(mods, [&] { handleKeyDown(app, app.hwnd, vk); });
}
// Alt+arrows arrive as WM_SYSKEYDOWN, which the window procedure hands to
// editorAltArrowKey
bool altArrow(App& app, unsigned vk) {
    bool handled = false;
    withKeys({false, false, true}, [&] { handled = editorAltArrowKey(app, app.hwnd, vk); });
    return handled;
}
void type(App& app, const std::wstring& text) {
    for (wchar_t ch : text) handleCharInput(app, app.hwnd, ch);
}
void setText(App& app, const std::wstring& text, size_t caret) {
    editorClearExtraCarets(app);
    app.editorText = text;
    rebuildLineStarts(app);
    app.undoStack.clear();
    app.redoStack.clear();
    app.editorCursorPos = app.editorSelStart = app.editorSelEnd = caret;
    app.editorHasSelection = false;
    app.editorDesiredCol = -1;
}
void select(App& app, size_t anchor, size_t caret) {
    app.editorSelStart = anchor;
    app.editorSelEnd = app.editorCursorPos = caret;
    app.editorHasSelection = anchor != caret;
}
void addCaret(App& app, size_t anchor, size_t pos) {
    App::EditorCaret caret;
    caret.anchor = anchor;
    caret.pos = pos;
    caret.hasSelection = anchor != pos;
    app.editorExtraCarets.push_back(caret);
}
std::vector<size_t> carets(const App& app) {
    std::vector<size_t> positions{app.editorCursorPos};
    for (const auto& caret : app.editorExtraCarets) positions.push_back(caret.pos);
    std::sort(positions.begin(), positions.end());
    return positions;
}

// Alt+Shift+arrows arrive the same way, with Shift added to the thread state
bool altShiftArrow(App& app, unsigned vk) {
    bool handled = false;
    withKeys({false, true, true}, [&] { handled = editorAltArrowKey(app, app.hwnd, vk); });
    return handled;
}
void moveLines(App& app) {
    setText(app, L"one\ntwo\nthree", 5);
    check(altArrow(app, VK_UP) && app.editorText == L"two\none\nthree" && app.editorCursorPos == 1,
          "Alt+Up swaps the caret line with the line above and keeps the column");
    check(altArrow(app, VK_UP) && app.editorText == L"two\none\nthree" && app.undoStack.size() == 1,
          "Alt+Up on the first line changes nothing");
    key(app, 'Z', {true});
    check(app.editorText == L"one\ntwo\nthree", "one Ctrl+Z puts the moved line back");
    setText(app, L"one\ntwo\nthree", 9);
    altArrow(app, VK_DOWN);
    check(app.editorText == L"one\ntwo\nthree" && app.undoStack.empty(),
          "Alt+Down on the last line changes nothing");
    setText(app, L"a\nb", 3);
    altArrow(app, VK_UP);
    check(app.editorText == L"b\na" && app.editorCursorPos == 1,
          "the last line moves up although it has no newline");
    setText(app, L"one\ntwo\nthree", 0);
    select(app, 0, 5);
    altArrow(app, VK_DOWN);
    check(app.editorText == L"three\none\ntwo" && app.editorHasSelection &&
          app.editorSelStart == 6 && app.editorSelEnd == 11,
          "Alt+Down moves every selected line, the selection riding along");
    setText(app, L"one\ntwo\nthree", 0);
    select(app, 0, 4);
    altArrow(app, VK_DOWN);
    check(app.editorText == L"two\none\nthree",
          "a selection ending at a line start leaves that line in place");
    setText(app, L"1\n2\n3\n4\n5", 0);
    addCaret(app, 4, 4);
    altArrow(app, VK_DOWN);
    check(app.editorText == L"2\n1\n4\n3\n5" && carets(app) == std::vector<size_t>({2, 6}),
          "every caret's line moves down");
    key(app, 'Z', {true});
    check(app.editorText == L"1\n2\n3\n4\n5", "one Ctrl+Z restores every moved line");
    setText(app, L"1\n2\n3\n4", 2);
    addCaret(app, 4, 4);
    altArrow(app, VK_UP);
    check(app.editorText == L"2\n3\n1\n4", "neighbouring caret lines move as one block");
    app.editorReadingPreview = true;
    check(!altArrow(app, VK_UP), "Alt+Up belongs to the source editor only");
    app.editorReadingPreview = false;
}

void duplicateLines(App& app) {
    // Down copies the caret line below; VSCode keeps the caret on the copy
    setText(app, L"one\ntwo\nthree", 0);
    check(altShiftArrow(app, VK_DOWN) && app.editorText == L"one\none\ntwo\nthree",
          "Alt+Shift+Down copies the caret line below it");
    check(app.editorCursorPos == 0, "Copy Down leaves the caret on the original line");

    // Up copies the line above the caret's line (inserted before it)
    setText(app, L"one\ntwo\nthree", 5);
    check(altShiftArrow(app, VK_UP) && app.editorText == L"one\ntwo\ntwo\nthree",
          "Alt+Shift+Up copies the caret line above it");
    check(app.editorCursorPos == 9, "Copy Up moves the caret into the copy at the same column");

    // A multi-line selection duplicates as one block
    setText(app, L"one\ntwo\nthree", 0);
    select(app, 0, 7);
    check(altShiftArrow(app, VK_DOWN) && app.editorText == L"one\ntwo\none\ntwo\nthree",
          "a selected block duplicates below itself");
    check(app.editorHasSelection && app.editorSelStart == 0 && app.editorSelEnd == 7,
          "the selection stays on the original block");

    // Repeated presses keep cloning (VSCode clones the caret line again)
    setText(app, L"a\nb", 0);
    altShiftArrow(app, VK_DOWN);
    altShiftArrow(app, VK_DOWN);
    check(app.editorText == L"a\na\na\nb", "repeating Alt+Shift+Down keeps cloning");

    // One Ctrl+Z removes one duplication at a time
    key(app, 'Z', {true});
    check(app.editorText == L"a\na\nb", "one Ctrl+Z undoes one duplication");

    // The first line has nothing above it: Alt+Shift+Up on line 0 is a no-op
    setText(app, L"one\ntwo", 0);
    check(altShiftArrow(app, VK_UP) && app.editorText == L"one\none\ntwo",
          "Alt+Shift+Up on the first line duplicates it above itself");
    check(app.editorCursorPos == 4, "the caret lands on the copy at the same column");

    // A file without a trailing newline cannot grow downward cleanly
    setText(app, L"a\nb", 3);
    check(altShiftArrow(app, VK_DOWN) && app.editorText == L"a\nb\nb\n",
          "duplicating the last line appends the newline it needs");
    check(app.editorCursorPos == 3, "the caret stays at its column in the original");

    // With one it works and stays out of the previous line
    setText(app, L"a\nb\n", 2);
    check(altShiftArrow(app, VK_DOWN) && app.editorText == L"a\nb\nb\n",
          "duplicating down with a trailing newline appends the copy");

    // Multi-caret: each caret's line duplicates, blocks processed bottom-up
    setText(app, L"1\n2\n3\n4\n5", 0);
    addCaret(app, 4, 4);
    check(altShiftArrow(app, VK_DOWN) &&
              app.editorText == L"1\n1\n2\n3\n3\n4\n5",
          "every caret's line duplicates in place");
    check(carets(app) == std::vector<size_t>({0, 6}),
          "each caret stays on its original line");
    key(app, 'Z', {true});
    check(app.editorText == L"1\n2\n3\n4\n5",
          "the whole multi-caret duplication is one undo step");

    // Reading preview owns the keys, same as Alt+Up/Down
    app.editorReadingPreview = true;
    check(!altShiftArrow(app, VK_DOWN), "Alt+Shift+Down belongs to the source editor only");
    app.editorReadingPreview = false;
}

void wrapSelection(App& app) {
    const std::pair<wchar_t, wchar_t> pairs[] = {
        {L'"', L'"'}, {L'\'', L'\''}, {L'`', L'`'}, {L'*', L'*'}, {L'~', L'~'}, {L'^', L'^'},
        {L'=', L'='}, {L':', L':'}, {L'(', L')'}, {L'[', L']'}, {L'{', L'}'}};
    for (const auto& [open, close] : pairs) {
        setText(app, L"a word here", 0);
        select(app, 2, 6);
        type(app, std::wstring(1, open));
        const std::wstring expected = L"a " + std::wstring(1, open) + L"word" + close + L" here";
        check(app.editorText == expected && app.editorHasSelection &&
              app.editorSelStart == 3 && app.editorSelEnd == 7,
              "a pair character typed over a selection wraps it and keeps it selected");
        key(app, 'Z', {true});
        check(app.editorText == L"a word here", "one Ctrl+Z unwraps");
    }
    setText(app, L"a word here", 0);
    select(app, 2, 6);
    type(app, L"**");
    check(app.editorText == L"a **word** here" && app.editorSelStart == 4 && app.editorSelEnd == 8,
          "typing * twice makes the selection bold");
    setText(app, L"a word here", 0);
    select(app, 6, 2);
    type(app, L"(");
    check(app.editorText == L"a (word) here" && app.editorSelStart == 7 &&
          app.editorSelEnd == 3 && app.editorCursorPos == 3,
          "a backward selection stays backward");
    setText(app, L"line one\nline two", 0);
    select(app, 0, 17);
    type(app, L"`");
    check(app.editorText == L"`line one\nline two`", "a selection across lines wraps as a whole");
    setText(app, L"a word here", 0);
    select(app, 2, 6);
    type(app, L"x");
    check(app.editorText == L"a x here", "other characters still replace the selection");
    app.editorAssists = false;
    setText(app, L"a word here", 0);
    select(app, 2, 6);
    type(app, L"(");
    check(app.editorText == L"a ( here", "with the assists off ( replaces the selection");
    app.editorAssists = true;
}

void columnCarets(App& app) {
    // The report's case: line up a table with carets in one column
    setText(app, L"| a | b |\n| cc | d |\n|eee| f |\nend", 3);
    key(app, VK_DOWN, {true, false, true});
    key(app, VK_DOWN, {true, false, true});
    check(carets(app) == std::vector<size_t>({3, 13, 24}) && app.editorCursorPos == 24,
          "each Ctrl+Alt+Down adds a caret below in the same column, as the primary");
    type(app, L"_");
    check(app.editorText == L"| a_ | b |\n| c_c | d |\n|ee_e| f |\nend" &&
          carets(app) == std::vector<size_t>({4, 15, 27}),
          "typing inserts at every caret");
    handleCharInput(app, app.hwnd, 8);
    check(app.editorText == L"| a | b |\n| cc | d |\n|eee| f |\nend" &&
          carets(app) == std::vector<size_t>({3, 13, 24}),
          "Backspace deletes at every caret");
    key(app, VK_DELETE);
    check(app.editorText == L"| a| b |\n| c | d |\n|ee| f |\nend", "Delete removes at every caret");
    type(app, L"xy");
    key(app, 'Z', {true});
    check(app.editorText == L"| ax| b |\n| cx | d |\n|eex| f |\nend" &&
          app.editorExtraCarets.empty(),
          "Ctrl+Z takes back the last keystroke at every caret, leaving one caret");
    key(app, 'Z', {true});
    key(app, 'Z', {true});
    check(app.editorText == L"| a | b |\n| cc | d |\n|eee| f |\nend",
          "each Ctrl+Z is one keystroke at all carets");
    key(app, VK_UP, {true, false, true});
    check(app.editorExtraCarets.empty() == false, "Ctrl+Alt+Up adds a caret above");
    key(app, VK_ESCAPE);
    check(app.editorExtraCarets.empty() && app.editMode && !app.escPressedOnce,
          "Esc returns to one caret and stays in the editor");
    setText(app, L"| a | b |\n| cc | d |\n|eee| f |\nend", 3);
    key(app, VK_DOWN, {true, false, true});
    key(app, VK_DOWN, {true, false, true});
    key(app, VK_DOWN);
    check(carets(app) == std::vector<size_t>({13, 24, 34}),
          "added carets keep their column when they move down together");
}

void caretMotion(App& app) {
    setText(app, L"ab\ncd\nef", 1);
    key(app, VK_DOWN, {true, false, true});
    key(app, VK_DOWN, {true, false, true});
    key(app, VK_END);
    check(carets(app) == std::vector<size_t>({2, 5, 8}), "End moves every caret to its line end");
    key(app, VK_HOME);
    check(carets(app) == std::vector<size_t>({0, 3, 6}), "Home moves every caret to its line start");
    key(app, VK_UP);
    check(carets(app) == std::vector<size_t>({0, 3}), "carets that meet merge into one");
    key(app, VK_RIGHT, {false, true});
    check(app.editorHasSelection && app.editorExtraCarets.size() == 1 &&
          app.editorExtraCarets[0].hasSelection,
          "Shift+arrows extend every caret's selection");
    type(app, L"Z");
    check(app.editorText == L"Zb\nZd\nef", "typing replaces every selection");
    key(app, VK_RIGHT, {true});
    check(carets(app) == std::vector<size_t>({2, 5}), "Ctrl+Right moves every caret by word");
}

void clickCarets(App& app) {
    setText(app, L"alpha\nbeta\ngamma", 0);
    const float lineHeight = app.editorTextFormat->GetFontSize() * 1.5f;
    D2D1_POINT_2F point{};
    app.editorCursorPos = 8;
    editorCaretPoint(app, point);
    app.editorCursorPos = app.editorSelStart = app.editorSelEnd = 0;
    const int x = (int)(point.x + 1.0f), y = (int)(point.y + lineHeight * 0.5f);
    app.altClickGuard = false;
    withKeys({false, false, true}, [&] {
        handleEditorMouseDown(app, app.hwnd, x, y);
        handleEditorMouseUp(app, app.hwnd, x, y);
    });
    check(carets(app) == std::vector<size_t>({0, 8}) && app.editorCursorPos == 8 && app.altClickGuard,
          "Alt+click adds a caret, which becomes the primary and guards the Alt release");
    withKeys({false, false, true}, [&] {
        handleEditorMouseDown(app, app.hwnd, x, y);
        handleEditorMouseUp(app, app.hwnd, x, y);
    });
    check(carets(app) == std::vector<size_t>({0}), "Alt+click on a caret removes it");
    withKeys({false, false, true}, [&] { handleEditorMouseDown(app, app.hwnd, x, y); });
    app.lastClickTime = {};
    withKeys({}, [&] {
        handleEditorMouseDown(app, app.hwnd, x, y);
        handleEditorMouseUp(app, app.hwnd, x, y);
    });
    check(carets(app) == std::vector<size_t>({8}), "a plain click starts over from one caret");
}

void nextOccurrence(App& app) {
    setText(app, L"cat dog cat cat", 1);
    key(app, 'D', {true});
    check(app.editorHasSelection && app.editorSelStart == 0 && app.editorSelEnd == 3 &&
          app.editorExtraCarets.empty(), "the first Ctrl+D selects the word at the caret");
    key(app, 'D', {true});
    key(app, 'D', {true});
    check(app.editorExtraCarets.size() == 2 && app.editorCursorPos == 15,
          "each further Ctrl+D selects the next occurrence");
    key(app, 'D', {true});
    check(app.editorExtraCarets.size() == 2, "Ctrl+D stops once every occurrence is selected");
    type(app, L"cow");
    check(app.editorText == L"cow dog cow cow", "typing replaces every selected occurrence");
    key(app, 'Z', {true});
    key(app, 'Z', {true});
    key(app, 'Z', {true});
    check(app.editorText == L"cat dog cat cat", "three Ctrl+Z take back the three keystrokes");
}

void caretClipboardText(App& app) {
    setText(app, L"a1 b2 c3", 2);
    select(app, 1, 2);
    addCaret(app, 4, 5);
    addCaret(app, 7, 8);
    check(editorCaretSelectionsText(app) == L"1\n2\n3", "copy joins the selections with line breaks");
    editorPasteAtCarets(app, app.hwnd, L"x\ny\nz\n");
    check(app.editorText == L"ax by cz", "a line per caret is spread over the carets");
    editorPasteAtCarets(app, app.hwnd, L"!");
    check(app.editorText == L"ax! by! cz!", "other text is pasted whole at every caret");
    key(app, 'Z', {true});
    check(app.editorText == L"ax by cz", "a paste at every caret is one undo step");
}

void wordCount(App& app) {
    app.showWordCount = true;
    setText(app, L"# Title\n\nHello **bold** world", 0);
    editorReparse(app, true);
    check(app.editorDocCounts.words == 4 && app.editorDocCounts.characters == 21,
          "the editor counts the words and characters a reader sees");
    select(app, 9, 14);
    app.renderTarget->BeginDraw();
    renderEditorWordCount(app);
    check(SUCCEEDED(app.renderTarget->EndDraw()), "the count chip paints");
    const D2D1_RECT_F pill = editorReadingButtonRect(app);
    const D2D1_RECT_F chip = app.editorWordCountRect;
    check(chip.right > chip.left && chip.left > pill.right && chip.top == pill.top,
          "the chip sits beside the Read button");
    check(app.editorSelCounts.words == 1 && app.editorSelCounts.characters == 5,
          "a selection shows its own count");
    const std::string saved = app.currentFile;
    app.currentFile = "notes.txt";
    setText(app, L"**not markup** here", 0);
    editorReparse(app, true);
    check(app.editorDocCounts.words == 3 && app.editorDocCounts.characters == 19,
          "plain-text documents count as they stand");
    app.currentFile = saved;
    app.showWordCount = false;
    app.renderTarget->BeginDraw();
    renderEditorWordCount(app);
    app.renderTarget->EndDraw();
    check(app.editorWordCountRect.right <= app.editorWordCountRect.left,
          "the switch hides the chip");
    app.showWordCount = true;
}

// #252: what the window keeps on a close without asking, and the next
// launch handing it back
void draftRecovery() {
    namespace fs = std::filesystem;
    const std::string a = fs::absolute("draft-a.md").u8string();
    const std::string b = fs::absolute("draft-b.md").u8string();
    std::ofstream(a, std::ios::binary) << "saved A\n";
    std::ofstream(b, std::ios::binary) << "saved B\n";

    auto first = std::make_unique<App>();
    first->hwnd = CreateWindowExW(0, L"STATIC", L"Draft tests", WS_POPUP, 0, 0, 1050, 900,
                                  nullptr, nullptr, nullptr, nullptr);
    check(first->hwnd && initD2D(*first) && createRenderTarget(*first), "draft test editor starts");
    first->width = 1050;
    first->height = 900;
    updateTextFormats(*first);
    first->currentFile = a;
    tabsInit(*first);
    tabOpenPath(*first, first->hwnd, b, false);
    enterRecoveredDraft(*first, first->hwnd, "draft of A", a);
    check(first->tabs.size() == 2 && first->currentFile == a && first->editMode &&
          first->editorDirty && first->editorText == L"draft of A",
          "a draft takes over the clean tab of its own file");
    enterRecoveredDraft(*first, first->hwnd, "draft of B", b);
    check(first->tabs.size() == 2 && first->currentFile == b && first->editorText == L"draft of B",
          "an inactive clean tab of the file is reused too");
    enterRecoveredDraft(*first, first->hwnd, "second draft of B", b);
    check(first->tabs.size() == 3 && first->editorText == L"second draft of B",
          "a tab with unsaved edits keeps them; the draft opens beside it");

    // Closing without asking writes every unsaved buffer as a draft
    draftsSweep(*first);
    std::vector<std::wstring> drafts;
    const std::wstring prefix = L"draft-" + std::to_wstring(GetCurrentProcessId()) + L"-";
    for (const auto& entry : fs::directory_iterator(fs::path(tintaConfigDir()) / L"drafts")) {
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(prefix, 0) == 0) drafts.push_back(entry.path().wstring());
    }
    check(drafts.size() == 3, "every unsaved tab leaves a draft behind");

    // The next launch: the session restored A clean, the drafts come back
    auto next = std::make_unique<App>();
    next->hwnd = CreateWindowExW(0, L"STATIC", L"Draft tests 2", WS_POPUP, 0, 0, 1050, 900,
                                 nullptr, nullptr, nullptr, nullptr);
    check(next->hwnd && initD2D(*next) && createRenderTarget(*next), "next launch starts");
    next->width = 1050;
    next->height = 900;
    updateTextFormats(*next);
    next->currentFile = a;
    tabsInit(*next);
    next->recoveredDrafts = drafts;
    draftsRecoverAll(*next, next->hwnd);
    size_t dirtyTabs = 0;
    bool sawA = false;
    for (size_t i = 0; i < next->tabs.size(); ++i) {
        const bool active = (int)i == next->activeTab;
        const bool dirty = active ? next->editorDirty : next->tabs[i].editorDirty;
        const std::wstring text = active ? next->editorText : next->tabs[i].editorText;
        dirtyTabs += dirty ? 1 : 0;
        sawA = sawA || text == L"draft of A";
    }
    check(next->tabs.size() == 3 && dirtyTabs == 3 && sawA,
          "the next launch restores every draft as an unsaved tab, A in its own tab");
    bool leftover = false;
    for (const auto& path : drafts) leftover = leftover || fs::exists(path);
    check(!leftover, "restored drafts are removed from disk");

    for (App* app : {first.get(), next.get()}) {
        draftsDeleteAll(*app);
        DestroyWindow(app->hwnd);
        app->hwnd = nullptr;
    }
}
}  // namespace

// Alt+Shift+drag draws a rectangle and gives every covered row its own
// caret, exactly like VSCode's column selection. The drag here replays the
// mouse-down / move / up sequence the window proc would deliver.
void boxSelection(App& app) {
    setText(app, L"abcd\nefgh\nijkl\n", 0);

    // Press at column 1 of the first line with Alt+Shift held
    auto boxDown = [&](int x, int y) {
        withKeys({false, true, true}, [&] { handleEditorMouseDown(app, app.hwnd, x, y); });
    };
    auto boxMove = [&](int x, int y) {
        withKeys({false, true, true}, [&] { handleEditorMouseMove(app, app.hwnd, x, y); });
    };
    auto boxUp = [&]() { handleEditorMouseUp(app, app.hwnd, 0, 0); };

    // Line height is font size * 1.5; land the drag on lines 0 and 2
    const float lineH = app.editorTextFormat->GetFontSize() * 1.5f;
    const int yLine0 = (int)(chromeTopHeight(app) + dpi(app, 8.0f) + lineH * 0.5f);
    const int yLine2 = yLine0 + (int)(lineH * 2.0f);
    boxDown(95, yLine0);
    check(app.editorBoxSelecting, "Alt+Shift+press starts a box selection");
    boxMove(120, yLine2);
    {
        std::cerr << "[box] carets=" << app.editorExtraCarets.size() << " spans=";
        for (const auto& c : app.editorExtraCarets)
            std::cerr << c.anchor << "-" << c.pos << " ";
        std::cerr << "\n";
    }
    check(app.editorExtraCarets.size() == 3,
          "three covered rows get three carets");
    for (const auto& caret : app.editorExtraCarets) {
        check(caret.hasSelection, "each box row has a real span");
    }
    // The spans are the same columns on every line
    std::vector<std::pair<size_t, size_t>> spans;
    for (const auto& caret : app.editorExtraCarets)
        spans.push_back({caret.anchor, caret.pos});
    check(spans.size() == 3 && spans[0].second - spans[0].first ==
              spans[1].second - spans[1].first,
          "the box keeps a uniform column range");
    boxUp();
    check(!app.editorBoxSelecting, "release ends the box drag");
    check(app.editorExtraCarets.size() == 3,
          "the carets survive the release for immediate typing");

    // Editing all rows at once: type replaces each row's own span (cd, gh, kl)
    handleCharInput(app, app.hwnd, L'X');
    check(app.editorText == L"abX\nefX\nijX\n",
          "typing replaces every row's span simultaneously");

    // One Ctrl+Z restores the pre-edit text
    key(app, 'Z', {true});
    check(app.editorText == L"abcd\nefgh\nijkl\n", "the box edit is undoable");

    // Upward drag works too (anchor below the cursor)
    setText(app, L"abcd\nefgh\nijkl\n", 0);
    boxDown(120, yLine2);
    boxMove(95, yLine0);
    check(app.editorExtraCarets.size() == 3, "dragging upward covers the same rows");
    boxUp();

    // Zero-width box: dragging straight down inserts a caret per row
    setText(app, L"abcd\nefgh\nijkl\n", 0);
    boxDown(95, yLine0);
    boxMove(95, yLine2);
    boxUp();
    bool allZero = true;
    for (const auto& caret : app.editorExtraCarets)
        if (caret.hasSelection) allZero = false;
    check(allZero, "a vertical drag gives zero-width carets");

    // Plain click still collapses the box back to one caret
    boxDown(95, 100);
    boxUp();
    withKeys({false, false, false}, [&] { handleEditorMouseDown(app, app.hwnd, 95, 100); });
    handleEditorMouseUp(app, app.hwnd, 0, 0);
    check(app.editorExtraCarets.empty(), "a plain click clears the box carets");

    app.editorReadingPreview = true;
    boxDown(95, 100);
    check(!app.editorBoxSelecting, "the reading preview does not start a box");
    app.editorReadingPreview = false;
}

int runEditorFeatureTests() {
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Editor feature tests", WS_POPUP, 0, 0, 1050, 900,
                               nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) return 2;
    app.width = 1050;
    app.height = 900;
    updateTextFormats(app);
    Settings settings;
    settings.keyProfile = "windows";
    applyKeymap(app, settings);
    app.currentFile = TINTA_MULTICURSOR_FIXTURE;
    enterEditMode(app);
    check(app.editMode && app.editorText.find(L"| Grace | Admiral |") != std::wstring::npos,
          "the mixed #251 fixture opens in the editor");
    for (bool wrap : {false, true}) {
        app.editorWordWrap = wrap;
        moveLines(app);
        duplicateLines(app);
        boxSelection(app);
        wrapSelection(app);
        columnCarets(app);
        caretMotion(app);
        clickCarets(app);
        nextOccurrence(app);
        caretClipboardText(app);
    }
    wordCount(app);
    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
    state.reset();
    draftRecovery();
    CoUninitialize();
    std::cout << "Editor features: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
