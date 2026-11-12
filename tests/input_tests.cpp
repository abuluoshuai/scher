#include "input.h"
#include "sidepanels.h"
#include "tabs.h"
#include "startpage.h"
#include "file_utils.h"
#include "d2d_init.h"
#include "overlays.h"
#include "settings.h"

#include <fstream>
#include <iostream>
#include <iostream>
#include <memory>
#include <sstream>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

// Same contract as the main message loop: a consumed key must not have
// its layout-dependent WM_CHAR translated into the new editor/overlay.
void stroke(App& app, unsigned vk, unsigned character = 0) {
    bool consumed = handleKeyDown(app, app.hwnd, vk);
    if (!consumed && character) handleCharInput(app, app.hwnd, character);
}

void settingsDismissal() {
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Settings input tests", WS_POPUP,
        100, 100, 1050, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) {
        check(false, "settings native renderer initializes");
        if (app.hwnd) DestroyWindow(app.hwnd);
        return;
    }
    app.currentFile = TINTA_SHORTCUT_FIXTURE;
    app.tabs.resize(1);
    app.tabs[0].path = app.currentFile;
    app.editorText = L"# Unsaved text\n\n$x^2$\n";
    app.editorDirty = true;
    app.editorCursorPos = 4;
    app.editRailAnim = 1;
    auto open = [&](int section = 0, float animation = 1.0f) {
        app.showSettings = true;
        app.settingsSection = section;
        app.settingsAnimation = animation;
        app.renderTarget->BeginDraw();
        renderSettingsOverlay(app);
        check(SUCCEEDED(app.renderTarget->EndDraw()), "settings panel renders");
    };
    auto click = [&](float x, float y) {
        LPARAM point = MAKELPARAM((int)x, (int)y);
        handleMouseDown(app, app.hwnd, MK_LBUTTON, point);
        handleMouseUp(app, app.hwnd, 0, point);
        check(app.editorText == L"# Unsaved text\n\n$x^2$\n" && app.editorCursorPos == 4 && app.editorDirty,
              "settings clicks preserve editor text, cursor and unsaved changes");
        check(!app.selecting && !app.editorSelecting && !app.createRefPending,
              "settings dismissal cannot start document selection or activate a link");
    };
    for (int theme : {0, 5}) {
        applyTheme(app, theme);
        for (float scale : {1.0f, 1.5f, 2.0f}) {
            app.contentScale = scale;
            updateTextFormats(app);
            for (int width : {650, 1050}) {
                app.width = width;
                app.height = width == 650 ? 600 : 900;
                for (bool edit : {false, true}) {
                    app.editMode = edit;
                    for (float animation : {0.0f, 1.0f}) {
                        open(0, animation);
                        auto panel = settingsPanelRect(app);
                        click(panel.left+8, (panel.top+panel.bottom)/2);
                        check(app.showSettings, "blank panel space does not dismiss settings");
                        const auto close = settingsCloseButtonRect(app);
                        check(close.left > panel.left && close.right < panel.right &&
                              close.top > panel.top && close.bottom < panel.bottom,
                              "close button stays inside the animated/scaled panel");
                        bool closeHit = false;
                        for (const auto& hit : app.settingsHits)
                            if (hit.second == SET_CLOSE) closeHit = true;
                        check(closeHit, "renderer registers a close-button hit target");
                        app.settingsLangOpen = app.settingsKeysOpen = true;
                        click((close.left+close.right)/2, (close.top+close.bottom)/2);
                        check(!app.showSettings && app.settingsAnimation == 0 &&
                              !app.settingsLangOpen && !app.settingsKeysOpen && app.settingsHits.empty(),
                              "close button clears transient dialog state");
                        for (const auto point : {D2D1::Point2F(panel.left-5, (panel.top+panel.bottom)/2),
                                D2D1::Point2F(panel.right+5, (panel.top+panel.bottom)/2),
                                D2D1::Point2F(app.width/2.0f, panel.top-5),
                                D2D1::Point2F(app.width/2.0f, panel.bottom+5)}) {
                            open(0, animation);
                            app.hoveredLink = "fileref-missing:must-not-be-created.md";
                            click(point.x, point.y);
                            check(!app.showSettings, "all four backdrop edges dismiss settings");
                        }
                    }
                    open();
                    // The backdrop also covers the icon and + tab control.
                    app.tabHits = {{D2D1::RectF(dpi(app, 80), 0, dpi(app, 110), dpi(app, 40)), -2}};
                    click(dpi(app, 90), dpi(app, 20));
                    check(!app.showSettings && app.tabs.size() == 1,
                          "backdrop click on + does not create a tab");
                    open();
                    click(dpi(app, 20), dpi(app, 20));
                    check(!app.showSettings && !app.showContextMenu,
                          "backdrop click on the icon does not open another menu");
                }
            }
        }
    }
    app.width = 1050;
    app.height = 900;
    app.contentScale = 1;
    updateTextFormats(app);
    open();
    const auto panel = settingsPanelRect(app);
    // Switching sections in the left rail must not resize the panel: one
    // shared height for all four sections keeps it from jumping.
    {
        const D2D1_RECT_F base = panel;
        for (int section = 0; section < 4; ++section) {
            open(section);
            const D2D1_RECT_F now = settingsPanelRect(app);
            check(std::abs((now.bottom - now.top) -
                           (base.bottom - base.top)) < 0.5f &&
                  std::abs((now.right - now.left) -
                           (base.right - base.left)) < 0.5f &&
                  std::abs(now.top - base.top) < 0.5f &&
                  std::abs(now.left - base.left) < 0.5f,
                  "every section shares the front-matter panel size");
        }
        open(0);
    }
    // The "Set as default Markdown viewer" row: a real hit target whose
    // click registers the association and flips the row into its
    // registered state; the renderer re-checks the registry on open.
    {
        open(0);
        D2D1_RECT_F registerBtn;
        bool found = false;
        for (const auto& [rect, action] : app.settingsHits)
            if (action == SET_REGISTER_DEFAULT) { registerBtn = rect; found = true; }
        check(found, "the register-default row exposes a hit target");
        if (found) {
            check(isRegisteredAsViewer() == app.settingsRegisterDefaultChecked,
                  "opening settings syncs the row with the registry");
            click((registerBtn.left + registerBtn.right) * 0.5f,
                  (registerBtn.top + registerBtn.bottom) * 0.5f);
            check(app.settingsRegisterDefaultChecked,
                  "clicking register-default runs the association write");
            check(isRegisteredAsViewer(),
                  "the association really exists in the registry afterwards");
        }
        // The button centers its label via a throwaway text layout; the
        // shared format must stay left-aligned or every later card label
        // and chip inherits centering (this broke the Pandoc row once).
        open(2);
        check(app.folderBrowserFormat->GetTextAlignment() ==
                  DWRITE_TEXT_ALIGNMENT_LEADING,
              "the shared settings format keeps left alignment");
    }
    // A dropdown row outside the panel is still part of the dialog.
    app.settingsLangOpen = true;
    app.settingsHits.push_back({D2D1::RectF(panel.left, panel.bottom+4, panel.left+100, panel.bottom+30), SET_LANG_PICK_BASE});
    click(panel.left+30, panel.bottom+15);
    check(app.showSettings && !app.settingsLangOpen && app.languageSetting == -1,
          "an outlying language choice is selected before backdrop dismissal");
    app.settingsLangOpen = true;
    click(panel.left+8, panel.top+8);
    check(app.showSettings && !app.settingsLangOpen, "inside blank space collapses only the dropdown");
    open(1);
    const auto track = app.settingsSliderTrack[0];
    handleMouseDown(app, app.hwnd, MK_LBUTTON, MAKELPARAM((int)(track.left+10), (int)track.top));
    check(app.settingsDragSlider != 0, "reading-width slider starts dragging");
    handleMouseUp(app, app.hwnd, 0, MAKELPARAM(5, 500));
    check(app.showSettings && !app.settingsDragSlider && GetCapture() != app.hwnd,
          "releasing a slider outside keeps settings open and releases capture");
    const int readingWidth = app.readingWidthPct;
    app.settingsLangOpen = app.settingsKeysOpen = true;
    app.settingsDragSlider = SET_SLIDER_READING;
    SetCapture(app.hwnd);
    stroke(app, VK_ESCAPE);
    check(!app.showSettings && !app.settingsLangOpen && !app.settingsKeysOpen &&
          !app.settingsDragSlider && GetCapture() != app.hwnd && app.readingWidthPct == readingWidth,
          "Escape shares close cleanup without reverting settings");
    openContextMenu(app, 4, chromeTopHeight(app), true);
    const auto& entries = contextMenuEntries(app);
    for (int row = 0; row < (int)entries.size(); ++row) {
        if (entries[row].action != CTX_SETTINGS) continue;
        click(app.contextMenuX+20, app.contextMenuY+contextMenuItemTop(app, row)+contextMenuItemHeight(app)/2);
        check(app.showSettings && !app.showContextMenu && !app.swallowNextMouseUp,
              "the icon menu's opening click cannot immediately dismiss settings");
        break;
    }
    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
    state.reset();
    CoUninitialize();
}

// Dragging the left rail's width grip must end on mouse release. The press
// sets swallowNextMouseUp, and the release used to be swallowed without ever
// ending the drag - the divider then followed the pointer forever.
void tabRailResizeRelease() {
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Rail resize input tests", WS_POPUP,
        100, 100, 1050, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd) {
        check(false, "a window for the rail resize test");
        return;
    }
    app.width = 1050;
    app.height = 900;
    app.contentScale = 1.0f;
    app.tabsOnLeft = true;
    app.zenMode = false;
    app.tabsWidth = 260.0f;
    // The rail only shows with more than one tab (or a forced strip)
    app.tabs.resize(2);
    app.tabs[0].id = 1;
    app.tabs[0].path = "a.md";
    app.tabs[1].id = 2;
    app.tabs[1].path = "b.md";

    const float edge = leftTabsRailWidth(app);
    const float y = chromeTopHeight(app) + 120.0f;
    check(tabRailResizeAt(app, edge, y), "the rail edge offers a resize grip");

    handleMouseDown(app, app.hwnd, MK_LBUTTON, MAKELPARAM((int)edge, (int)y));
    check(app.tabRailResize.active, "pressing the grip starts the drag");

    handleMouseMove(app, app.hwnd, MAKELPARAM((int)edge + 40, (int)y));
    check(app.tabsWidth > 260.0f, "the width follows the pointer during the drag");

    const float duringDrag = app.tabsWidth;
    handleMouseUp(app, app.hwnd, 0, MAKELPARAM((int)edge + 40, (int)y));
    check(!app.tabRailResize.active, "releasing ends the drag");

    handleMouseMove(app, app.hwnd, MAKELPARAM((int)edge + 120, (int)y));
    check(app.tabsWidth == duringDrag,
          "the divider stops following the pointer after release");

    DestroyWindow(app.hwnd);
}


// In the left rail placement the pointers's row must be the row it is over:
// hovering the caption band or the document must not light up a tab.
void tabRailHover() {
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Rail hover tests", WS_POPUP,
        100, 100, 1050, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) {
        check(false, "the rail hover test window initializes");
        if (app.hwnd) DestroyWindow(app.hwnd);
        return;
    }
    app.width = 1050;
    app.height = 900;
    app.contentScale = 1.0f;
    app.tabsOnLeft = true;
    app.tabs.resize(3);
    for (int i = 0; i < 3; i++) {
        app.tabs[i].id = i + 1;
        app.tabs[i].path = "doc" + std::to_string(i) + ".md";
        app.tabs[i].title = L"doc" + std::to_wstring(i);
    }
    app.activeTab = 0;
    // A document is open, so the rail (not the start page) is what renders
    app.currentFile = "doc0.md";
    auto paint = [&]() {
        app.renderTarget->BeginDraw();
        renderTabStrip(app);
        app.renderTarget->EndDraw();
    };
    auto hoverAt = [&](float x, float y) {
        paint();
        handleMouseMove(app, app.hwnd, MAKELPARAM((int)x, (int)y));
        return app.hoveredTab;
    };

    const float top = chromeTopHeight(app);
    const float railW = leftTabsRailWidth(app);
    paint();   // the hit table is filled by the paint pass
    check(app.tabHits.size() >= 3, "the rail publishes one hit per tab");

    // Rows start right below the caption: the first row is tab 0
    check(hoverAt(railW * 0.5f, top + 8.0f) == 0,
          "hovering the first rail row lights that row");
    // The caption band above the rail is not a tab
    const int captionHover = hoverAt(railW * 0.5f, top - 6.0f);
    check(captionHover == -1, "the caption band above the rail is not a tab");
    // Far right of the rail, same height as a row: no tab there
    const int pageHover = hoverAt(railW + 200.0f, top + 8.0f);
    check(pageHover == -1, "the document area beside the rail is not a tab");
    // Below the last row (empty rail space) is not a tab either
    const int emptyHover = hoverAt(railW * 0.5f, top + 3.0f * 34.0f + 40.0f);
    check(emptyHover == -1, "empty rail space below the tabs is not a tab");

    DestroyWindow(app.hwnd);
}



// Switching the tab bar from the top strip to the left rail must not leave
// the old strip's rectangles in the hit table: they used to survive the
// rail's early return, so pointing where the tabs used to be still hit a tab
// (highlighted a rail row, switched on click, and could be dragged out).
void tabRailStaleHits() {
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Rail stale hit tests", WS_POPUP,
        100, 100, 1050, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) {
        check(false, "the rail stale-hit window initializes");
        if (app.hwnd) DestroyWindow(app.hwnd);
        return;
    }
    app.width = 1050;
    app.height = 900;
    app.contentScale = 1.0f;
    app.tabs.resize(3);
    for (int i = 0; i < 3; i++) {
        app.tabs[i].id = i + 1;
        app.tabs[i].path = "doc" + std::to_string(i) + ".md";
        app.tabs[i].title = L"doc" + std::to_wstring(i);
    }
    app.activeTab = 0;
    app.currentFile = "doc0.md";
    auto paint = [&]() {
        app.renderTarget->BeginDraw();
        renderTabStrip(app);
        app.renderTarget->EndDraw();
    };

    // Top strip first, then the left rail: the strip rects must be gone
    app.tabsOnLeft = false;
    paint();
    const size_t topHits = app.tabHits.size();
    check(topHits >= 3, "the top strip publishes its own hits");

    app.tabsOnLeft = true;
    paint();
    const float railW = leftTabsRailWidth(app);
    size_t staleRect = 0;
    for (const auto& hit : app.tabHits) {
        // Only tab rows matter here; caption buttons (pin, history, +) are
        // wider by design. A tab row never reaches past the rail's width.
        if (hit.index >= 0 && hit.rect.right > railW + 1.0f) staleRect++;
    }
    // A left-rail Hover at the caption band is still no tab
    handleMouseMove(app, app.hwnd,
                    MAKELPARAM((int)(railW * 0.5f), 6));
    check(app.hoveredTab == -1,
          "the caption band above the rail holds no tab after the switch");
    check(staleRect == 0,
          "no hit rectangle from the old top strip survives the switch");
    check(app.tabHits.size() <= 8,
          "the rail republishes its own hits instead of stacking on the old");

    DestroyWindow(app.hwnd);
}



// The local browser opens several files at once: each file row carries a tick
// box, and ticking must not open the document behind it.
void folderBrowserPickTicks() {
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Picker tests", WS_POPUP,
        0, 0, 1050, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) {
        check(false, "the picker test window initializes");
        if (app.hwnd) DestroyWindow(app.hwnd);
        return;
    }
    app.width = 1050;
    app.height = 900;
    app.contentScale = 1.0f;

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"scher-pick-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const wchar_t* name : {L"a.md", L"b.md"}) {
        std::wstring path = dir + L"\\" + name;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const char body[] = "# doc\n";
            DWORD written = 0;
            WriteFile(f, body, sizeof(body) - 1, &written, nullptr);
            CloseHandle(f);
        }
    }

    app.folderBrowserPath = dir;
    app.showFolderBrowser = true;
    app.folderBrowserAnimation = 1.0f;
    populateFolderItems(app);
    check(app.folderPicked.empty(), "a fresh listing starts with nothing ticked");

    auto paint = [&]() {
        app.renderTarget->BeginDraw();
        renderFolderBrowser(app);
        app.renderTarget->EndDraw();
    };
    paint();

    FolderBrowserMetrics g = folderBrowserMetrics(app);
    const float rowRight = g.cardRight - dpi(app, 6.0f);
    const float tickX = rowRight - dpi(app, 16.0f);
    // Walk the rows through the real hit test to find the tick box's row
    float tickY = -1.0f;
    for (float y = g.listStartY + g.namingOffset; y < g.listBottom; y += 2.0f) {
        int idx = folderItemIndexAt(app, tickX, y);
        if (idx >= 0 && app.folderItems[idx].name == L"a.md") { tickY = y; break; }
    }
    check(tickY > 0.0f, "the first file row is reachable for a tick");
    if (tickY <= 0.0f) { DestroyWindow(app.hwnd); return; }

    const std::string before = app.currentFile;
    LPARAM tick = MAKELPARAM((int)tickX, (int)tickY);
    handleMouseDown(app, app.hwnd, MK_LBUTTON, tick);
    handleMouseUp(app, app.hwnd, 0, tick);
    check(app.folderPicked.size() == 1 && app.folderPicked[0] == L"a.md",
          "a click on the tick box selects the file");
    check(app.currentFile == before,
          "ticking a file does not open it");

    handleMouseDown(app, app.hwnd, MK_LBUTTON, tick);
    handleMouseUp(app, app.hwnd, 0, tick);
    check(app.folderPicked.empty(), "clicking the tick box again clears it");

    // A tick from one folder must not survive navigation
    handleMouseDown(app, app.hwnd, MK_LBUTTON, tick);
    handleMouseUp(app, app.hwnd, 0, tick);
    check(app.folderPicked.size() == 1, "the file is ticked before navigating");
    app.folderBrowserPath = std::wstring(tmp);
    populateFolderItems(app);
    check(app.folderPicked.empty(),
          "changing directory drops the previous selection");

    app.showFolderBrowser = false;
    DestroyWindow(app.hwnd);
}


}  // namespace

int runTabDropTests();
int runTabHoverCardTests();
int runSearchPanelTests();
int runTableInputTests();
int runDockedPreviewTests();
int runTableLayoutTests();
int runWindowDragTests();
int runSearchInputTests();
int runWrappedCursorTests();
int runFileFragmentTests();
int runEditorContextTests();
int runSuperscriptTests();
int runSidePanelTests();
int runImageTests(const char* remoteBase);
int runTabDropLaunchProbe();
int runEditorFeatureTests();
int runUiFontTests();
int runPrintExportTests();
int runSettingsTests();
int runUtilsSuite();

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--editor-feature-tests") return runEditorFeatureTests();
    if (argc == 2 && std::string(argv[1]) == "--ui-font-tests") return runUiFontTests();
    if (argc == 2 && std::string(argv[1]) == "--print-export-tests") return runPrintExportTests();
    if (argc == 2 && std::string(argv[1]) == "--settings-tests") return runSettingsTests();
    if (argc == 2 && std::string(argv[1]) == "--utils-tests") return runUtilsSuite();
    if (argc == 2 && std::string(argv[1]) == "--window-drag-tests") return runWindowDragTests();
    if (argc == 2 && std::string(argv[1]) == "--table-layout-tests") return runTableLayoutTests();
    if (argc == 2 && std::string(argv[1]) == "--table-input-tests") return runTableInputTests();
    if (argc == 2 && std::string(argv[1]) == "--docked-preview-tests") return runDockedPreviewTests();
    if (argc == 2 && std::string(argv[1]) == "--editor-context-tests") return runEditorContextTests();
    if (argc == 2 && std::string(argv[1]) == "--file-fragment-tests") return runFileFragmentTests();
    if (argc == 2 && std::string(argv[1]) == "--wrapped-cursor-tests") return runWrappedCursorTests();
    if (argc == 2 && std::string(argv[1]) == "--search-input-tests") return runSearchInputTests();
    if (argc >= 2 && std::string(argv[1]) == "--image-tests")
        return runImageTests(argc > 2 ? argv[2] : nullptr);
    if (argc == 2 && std::string(argv[1]) == "--sidepanel-tests") return runSidePanelTests();
    if (argc == 2 && std::string(argv[1]) == "--superscript-tests") return runSuperscriptTests();
    if (argc == 2 && std::string(argv[1]) == "--tab-drop-tests") return runTabDropTests();
    if (argc == 2 && std::string(argv[1]) == "--tab-hover-card-tests") return runTabHoverCardTests();
    if (argc == 2 && std::string(argv[1]) == "--search-panel-tests") return runSearchPanelTests();
    if (argc == 7 && std::string(argv[1]) == "--cascade") return runTabDropLaunchProbe();
    settingsDismissal();
    tabRailHover();
    folderBrowserPickTicks();
    tabRailStaleHits();
    tabRailResizeRelease();
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Tinta input tests", 0,
                              0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    check(app.hwnd != nullptr, "message-only test window created");
    if (!app.hwnd) return 1;
    app.currentFile = TINTA_SHORTCUT_FIXTURE;
    app.width = 1050;
    app.height = 900;
    Settings settings;
    settings.keyProfile = "windows";
    applyKeymap(app, settings);

    stroke(app, VK_F1);
    check(app.showHelp, "F1 opens Help with no character message");
    stroke(app, VK_F1);
    check(!app.showHelp, "F1 closes Help");
    stroke(app, 'P', 'p');
    check(!app.showHelp, "P cannot open Help");

    std::wstring original;
    for (unsigned ch : {0x65, 0x0443, 0x03B5}) {
        app.editMode = false;
        stroke(app, 'E', ch);
        check(app.editMode && !app.editorDirty, "E enters a clean editor for Latin/Cyrillic/Greek input");
        check(app.editorText.rfind(L"# Shortcut regression:", 0) == 0,
              "the mode-switch key cannot prefix the Markdown heading");
        original = app.editorText;
        stroke(app, 'X', 'x');
        check(app.editorText == L"x" + original, "immediate next character is preserved without a timer");
    }
    app.editMode = false;
    stroke(app, 'E', 0x0443);
    original = app.editorText;
    stroke(app, VK_F1);
    check(app.showHelp && app.editMode, "F1 opens Help while editing");
    stroke(app, 'E', 'e');
    check(app.editorText == original, "Help captures printable input over the editor");
    stroke(app, VK_F1);
    check(!app.showHelp && app.editMode && !app.editorDirty, "Help closes over a clean editor");
    stroke(app, 'P', 'p');
    check(app.editorText == L"p" + original, "normal P typing survives in editor");

    app.editMode = false;
    app.showSearch = true;
    app.searchActive = true;
    stroke(app, 'E', 0x0443);
    check(!app.editMode && app.searchQuery == L"\x0443", "search receives non-Latin E text");
    stroke(app, VK_ESCAPE);
    app.showToc = true;
    app.tocPinned = false;
    stroke(app, 'E', 0x0443);
    check(!app.editMode && app.tocFilter == L"\x0443", "unpinned Contents receives non-Latin filter text");
    app.tocPinned = true;
    stroke(app, 'E', 0x0443);
    check(app.editMode, "pinned Contents lets E reach the document");

    app.editMode = false;
    app.showToc = false;
    app.showSettings = true;
    stroke(app, 'E', 'e');
    stroke(app, VK_F1);
    check(!app.editMode && !app.showHelp, "Settings retains modal keyboard capture");
    app.showSettings = false;
    settings.keyProfile = "vim";
    applyKeymap(app, settings);
    stroke(app, VK_OEM_2, '?');
    check(app.showHelp, "vim punctuation opens Help");
    stroke(app, VK_OEM_2, '?');
    check(!app.showHelp, "vim punctuation closes Help");
    stroke(app, VK_OEM_2, '/');
    stroke(app, 'E', 'e');
    check(app.showSearch && app.searchQuery == L"e", "vim search keeps its first query character");
    stroke(app, VK_ESCAPE);
    stroke(app, VK_OEM_1, 0xFF1A);
    check(app.editMode && !app.editorDirty, "full-width colon still enters a clean editor");

    app.editMode = false;
    settings.keyProfile = "custom";
    settings.keyOverrides = {{"edit", "F2"}, {"help", "H"}};
    applyKeymap(app, settings);
    stroke(app, 'H', 'h');
    check(app.showHelp, "custom letter Help opens only once");
    stroke(app, 'H', 'h');
    check(!app.showHelp, "custom letter Help closes without reopening");
    stroke(app, VK_F2);
    check(app.editMode && !app.editorDirty, "custom function key enters Edit");
    app.confirmExitPending = true;
    settings.keyOverrides = {{"help", "F3"}};
    applyKeymap(app, settings);
    stroke(app, VK_F3);
    check(app.confirmExitPending && !app.showHelp, "Help cannot bypass unsaved-changes confirmation");

    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
    std::cout << "Input shortcuts: " << failures << " failures\n";
    return failures ? 1 : 0;
}
