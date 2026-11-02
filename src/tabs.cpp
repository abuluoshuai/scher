// Tabbed interface (design doc t3): Win11 Notepad-style tabs in a custom
// title bar. The strip is the caption -- the active tab lifts to the
// document surface with an accent underline, unsaved buffers show a dot,
// a chevron opens the open-files switcher when the strip is crowded.

#include "tabs.h"
#include "d2d_init.h"
#include "search.h"
#include "search_panel.h"
#include "overlays.h"

#include "document.h"
#include "drafts.h"
#include "signals.h"
#include "tableedit.h"
#include "editor.h"
#include "startpage.h"
#include "file_utils.h"
#include "i18n.h"
#include "input.h"
#include "remote.h"
#include "settings.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <shellapi.h>

namespace {

constexpr int TIMER_FILE_WATCH_ID = 1;
constexpr int TIMER_EDITOR_REPARSE_ID = 2;  // matches editor.cpp

std::wstring titleForPath(App& app, const std::string& path) {
    if (path.empty()) return tr(app, "title.untitled");
    std::wstring wide = toWide(path);
    size_t lastSep = wide.find_last_of(L"\\/");
    return lastSep == std::wstring::npos ? wide : wide.substr(lastSep + 1);
}

// Keep the active tab's bookkeeping in sync with the app state (path can
// change when a quick note is saved; dirty drives the unsaved dot)
void syncActiveTab(App& app) {
    if (app.tabs.empty()) return;
    if (app.activeTab < 0 || app.activeTab >= (int)app.tabs.size()) {
        app.activeTab = 0;
    }
    App::DocTab& tab = app.tabs[app.activeTab];
    tab.path = app.currentFile;
    // A pathless viewer tab with a custom title (an embedded Learn
    // document from the start page) keeps it; quick notes re-derive
    if (!app.currentFile.empty() || tab.title.empty() || app.editMode) {
        tab.title = titleForPath(app, app.currentFile);
    }
    tab.editMode = app.editMode;
    tab.editorDirty = app.editMode && app.editorDirty;
}

// Park the active editor buffer into its tab so the switch away loses
// nothing: the teardown mirrors exitEditMode minus the dialog and reload
// (another document loads immediately after).
void parkActiveEditBuffer(App& app) {
    if (!app.editMode || app.tabs.empty()) return;
    // An open table cell belongs to this buffer, not the next tab (#148)
    tableEditCommit(app);
    App::DocTab& tab = app.tabs[app.activeTab];
    tab.editMode = true;
    tab.editorDirty = app.editorDirty;
    tab.editorText = std::move(app.editorText);
    tab.editorScrollY = app.editorScrollY;
    tab.editorCursor = app.editorCursorPos;
    tab.wordWrap = app.editorWordWrap;
    tab.lastWrite = app.lastFileWriteTime;  // external-change detection

    app.editMode = false;
    app.editorReadingPreview = false;
    signalFadeHints(app);  // the edit hints leave with the buffer (#245)
    app.editorDirty = false;
    app.clearEditorLineLayoutCache();
    app.editorText.clear();
    app.editorLineStarts.clear();
    app.editorExtraCarets.clear();
    app.undoStack.clear();
    app.redoStack.clear();
    app.editorSearchMatches.clear();
    app.editorSearchCurrentIndex = 0;
    app.editorHasSelection = false;
    app.confirmExitPending = false;
    if (app.showSearch) closeSearchInput(app);
    KillTimer(app.hwnd, TIMER_EDITOR_REPARSE_ID);
    updateBlinkTimer(app);
    SetTimer(app.hwnd, TIMER_FILE_WATCH_ID, 500, nullptr);
}

}  // namespace

void tabsInit(App& app) {
    if (!app.tabs.empty()) return;
    App::DocTab tab;
    tab.id = ++app.tabIdCounter;
    tab.path = app.currentFile;
    tab.title = titleForPath(app, app.currentFile);
    // A pathless viewer tab is the start page, not an untitled note
    if (app.currentFile.empty() && !app.editMode) tab.title = L"Scher";
    if (!app.currentFile.empty())
        tab.readProgress = findReadProgress(loadSettings(), app.currentFile);
    app.tabs.push_back(std::move(tab));
    app.activeTab = 0;
}

void tabsSeedSession(App& app, const std::vector<std::string>& paths) {
    // Rebuilds the tab row from a saved session, in order; the already
    // loaded startup document becomes the active tab (appended if it was
    // not part of the session, e.g. a file-argument launch)
    Settings stored = loadSettings();
    app.tabs.clear();
    app.activeTab = -1;
    for (const auto& path : paths) {
        App::DocTab tab;
        tab.id = ++app.tabIdCounter;
        tab.path = path;
        tab.title = titleForPath(app, path);
        if (!path.empty()) tab.readProgress = findReadProgress(stored, path);
        if (app.activeTab < 0 &&
            _stricmp(path.c_str(), app.currentFile.c_str()) == 0) {
            app.activeTab = (int)app.tabs.size();
        }
        app.tabs.push_back(std::move(tab));
    }
    if (app.activeTab < 0) {
        App::DocTab tab;
        tab.id = ++app.tabIdCounter;
        tab.path = app.currentFile;
        tab.title = titleForPath(app, app.currentFile);
        if (!app.currentFile.empty())
            tab.readProgress = findReadProgress(stored, app.currentFile);
        app.tabs.push_back(std::move(tab));
        app.activeTab = (int)app.tabs.size() - 1;
    }
}

void tabActivate(App& app, HWND hwnd, int index) {
    tabsInit(app);
    if (index < 0 || index >= (int)app.tabs.size()) return;
    if (index == app.activeTab) return;

    // Leaving an embedded Learn document: an empty tab activated later
    // shows the launcher again
    app.startPageEmbeddedOpen = false;

    syncActiveTab(app);
    parkActiveEditBuffer(app);
    // The outgoing document's reading progress rides along with the
    // position memory the switch is about to persist anyway
    if (!app.tabs[app.activeTab].path.empty())
        persistReadProgress(app.tabs[app.activeTab].path,
                            app.tabs[app.activeTab].readProgress);
    app.activeTab = index;
    App::DocTab& tab = app.tabs[index];

    if (!tab.path.empty()) {
        // openDocumentInViewer saves the old reading position and restores
        // the new one; parse + viewport-first layout makes this feel instant
        openDocumentInViewer(app, toWide(tab.path));
    } else {
        // Untitled note: swap in an empty document so the preview pane
        // does not keep showing the previous tab's content
        app.currentFile.clear();
        auto result = parseDocument(app.parser, std::string(), app.currentFile);
        if (result.success) {
            app.root = result.root;
            app.parseTimeUs = result.parseTimeUs;
        }
        app.scrollY = app.targetScrollY = 0;
        app.scrollX = app.targetScrollX = 0;
        app.layoutDirty = true;
        // An empty viewer tab in a launcher window shows the start page
        // again; its label follows suit
        if (!tab.editMode && startPageActive(app)) {
            tab.title = L"Tinta";
        }
    }
    if (tab.editMode) {
        app.editorWordWrap = tab.wordWrap;
        restoreEditBuffer(app, tab.editorText, tab.editorDirty,
                          tab.editorScrollY, tab.editorCursor);
        tab.editorText.clear();  // ownership moved back to the app
    }
    app.showTabSwitcher = false;
    // An open search follows to the new document: its highlights, count
    // and, in the results panel, the other files from here (#246). The
    // editor has no side panels.
    if (app.editMode) closeSearchPanel(app);
    if (app.showSearch && !app.editMode && !app.searchQuery.empty()) {
        performSearch(app);
    }
    updateWindowTitle(app);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void tabOpenPath(App& app, HWND hwnd, const std::string& utf8Path,
                 bool activate, int insertBefore) {
    tabsInit(app);
    // A path that is already open switches to its tab instead of duplicating
    for (size_t i = 0; i < app.tabs.size(); i++) {
        const std::string& existing =
            (int)i == app.activeTab ? app.currentFile : app.tabs[i].path;
        if (!utf8Path.empty() && _stricmp(existing.c_str(),
                                          utf8Path.c_str()) == 0) {
            if (activate) tabActivate(app, hwnd, (int)i);
            return;
        }
    }
    App::DocTab tab;
    tab.id = ++app.tabIdCounter;
    tab.path = utf8Path;
    tab.title = titleForPath(app, utf8Path);
    if (!utf8Path.empty())
        tab.readProgress = findReadProgress(loadSettings(), utf8Path);
    int index = insertBefore < 0 ? (int)app.tabs.size()
                               : std::min(insertBefore, (int)app.tabs.size());
    app.tabs.insert(app.tabs.begin() + index, std::move(tab));
    // Keep the old active document attached to its tab until activation
    // parks its editor buffer; insertion may have shifted that tab right.
    if (app.activeTab >= index) ++app.activeTab;
    if (activate) {
        tabActivate(app, hwnd, index);
    } else {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void tabCloseIndex(App& app, HWND hwnd, int index) {
    tabsInit(app);
    if (index < 0 || index >= (int)app.tabs.size()) return;
    if (index==app.activeTab) tableEditCommit(app);

    // A dirty buffer routes through the unsaved-changes dialog (#106):
    // activate the tab, open the dialog, and finish the close from
    // confirmExitAction once the user decides
    bool dirty = (index == app.activeTab)
                     ? (app.editMode && app.editorDirty)
                     : (app.tabs[index].editMode && app.tabs[index].editorDirty);
    if (dirty) {
        if (index != app.activeTab) tabActivate(app, hwnd, index);
        app.pendingTabClose = app.activeTab;
        app.confirmExitPending = true;
        app.confirmExitOpenedAt = std::chrono::steady_clock::now();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    // The closed tab's crash-recovery draft goes with it
    draftsDeleteForTab(app, app.tabs[index].id);

    if ((int)app.tabs.size() <= 1) {
        // The last tab closes into the start page instead of taking the
        // window with it; closing the start page itself closes the window
        if (startPageActive(app)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else {
            tabBecomeStartPage(app, hwnd);
        }
        return;
    }

    if (index == app.activeTab) {
        // Leave a clean edit session silently before the neighbor loads
        if (app.editMode) {
            parkActiveEditBuffer(app);
            app.tabs[index].editMode = false;
            app.tabs[index].editorText.clear();
        }
        int next = index + 1 < (int)app.tabs.size() ? index + 1 : index - 1;
        // Activate first (so state lands in the surviving tab), then drop
        tabActivate(app, hwnd, next);
        int erase = index;
        app.tabs.erase(app.tabs.begin() + erase);
        if (app.activeTab > erase) app.activeTab--;
    } else {
        app.tabs.erase(app.tabs.begin() + index);
        if (app.activeTab > index) app.activeTab--;
    }
    app.hoveredTab = -1;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void tabCycle(App& app, HWND hwnd, int direction) {
    tabsInit(app);
    if (app.tabs.size() < 2) return;
    int count = (int)app.tabs.size();
    int next = (app.activeTab + direction + count) % count;
    tabActivate(app, hwnd, next);
}

// The lone surviving tab turns into the start page in place: closing
// the last document lands on the launcher instead of the window
// vanishing. Idempotent — a tab that is already the launcher stays one.
void tabBecomeStartPage(App& app, HWND hwnd) {
    tabsInit(app);
    parkActiveEditBuffer(app);  // clean editor teardown if one was open
    App::DocTab& tab = app.tabs[app.activeTab];
    draftsDeleteForTab(app, tab.id);
    tab.path.clear();
    tab.title = L"Scher";
    tab.editMode = false;
    tab.editorDirty = false;
    tab.editorText.clear();
    app.currentFile.clear();
    auto result = parseDocument(app.parser, std::string(), app.currentFile);
    if (result.success) {
        app.root = result.root;
        app.parseTimeUs = result.parseTimeUs;
    }
    app.scrollY = app.targetScrollY = 0;
    app.scrollX = app.targetScrollX = 0;
    app.layoutDirty = true;
    app.startPageEmbeddedOpen = false;
    app.hoveredTab = -1;
    updateWindowTitle(app);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Ctrl+T: a fresh empty viewer tab, which is the start page (the
// browser-style new-tab page with recents and open actions)
void tabOpenStartPage(App& app, HWND hwnd) {
    tabsInit(app);
    syncActiveTab(app);
    parkActiveEditBuffer(app);
    App::DocTab tab;
    tab.id = ++app.tabIdCounter;
    tab.title = L"Tinta";
    app.tabs.push_back(std::move(tab));
    app.activeTab = (int)app.tabs.size() - 1;
    app.currentFile.clear();
    auto result = parseDocument(app.parser, std::string(), app.currentFile);
    if (result.success) {
        app.root = result.root;
        app.parseTimeUs = result.parseTimeUs;
    }
    app.scrollY = app.targetScrollY = 0;
    app.scrollX = app.targetScrollX = 0;
    app.layoutDirty = true;
    app.startPageEmbeddedOpen = false;
    app.showTabSwitcher = false;
    updateWindowTitle(app);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void tabOpenQuickNote(App& app, HWND hwnd) {
    // The + button behaves like Ctrl+N, in-window: a fresh untitled
    // quick-note tab straight into the editor; the first Ctrl+S runs the
    // classic Save As flow and the tab picks up the chosen name
    tabsInit(app);
    syncActiveTab(app);
    parkActiveEditBuffer(app);
    App::DocTab tab;
    tab.id = ++app.tabIdCounter;
    tab.title = tr(app, "title.untitled");
    app.tabs.push_back(std::move(tab));
    app.activeTab = (int)app.tabs.size() - 1;
    // The preview pane renders app.root: swap in an empty document so the
    // fresh note does not preview the previous tab's content
    app.currentFile.clear();
    auto result = parseDocument(app.parser, std::string(), app.currentFile);
    if (result.success) {
        app.root = result.root;
        app.parseTimeUs = result.parseTimeUs;
    }
    enterQuickNoteMode(app);
    app.showTabSwitcher = false;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// --- geometry ---

namespace {

float captionButtonWidth(const App& app) { return dpi(app, 46.0f); }
// Always-on-top pin, left of the caption buttons
float pinButtonWidth(const App& app) { return dpi(app, 34.0f); }
D2D1_RECT_F pinButtonRect(const App& app) {
    float right = (float)app.width - captionButtonWidth(app) * 3.0f;
    return D2D1::RectF(right - pinButtonWidth(app), 0.0f, right,
                       chromeTopHeight(app));
}

struct StripMetrics {
    float height = 0.0f;
    float tabsLeft = 0.0f;    // after the icon cell
    float tabsRight = 0.0f;   // before + / chevron / caption buttons
    float tabWidth = 0.0f;
    bool compressed = false;  // chevron shown
    float plusX = 0.0f;
    float chevronX = 0.0f;
    int firstVisible = 0;
    int visibleCount = 0;
};

// Vertical rail (tabsOnLeft): one full-width row per tab, text horizontal,
// new tabs join at the bottom. Overflow scrolls vertically with the wheel.
struct VertMetrics {
    float width = 0.0f;       // rail width incl. the resize gutter
    float listTop = 0.0f;     // below the top title bar
    float listBottom = 0.0f;  // above the + button
    float rowH = 0.0f;
    float padX = 0.0f;        // text inset from the rail's left edge
    float gutterW = 0.0f;     // drag-to-resize gutter on the right edge
    float plusTop = 0.0f;     // new-tab row, right under the last visible tab
    float plusH = 0.0f;
    float rowX0 = 0.0f;       // left edge of the rows (clears the edit rail)
    int firstVisible = 0;
    int visibleCount = 0;
};

VertMetrics vertMetrics(const App& app) {
    VertMetrics v;
    v.width = leftTabsRailWidth(app);
    v.gutterW = dpi(app, 4.0f);
    v.rowH = dpi(app, 34.0f);
    v.padX = dpi(app, 12.0f);
    // In the editor the tool rail occupies the left edge of this column;
    // the tab rows start beside it instead of underneath it
    v.rowX0 = editRailWidth(app) > 0.0f ? editRailWidth(app) + dpi(app, 1.0f)
                                        : 0.0f;
    v.listTop = chromeTopHeight(app);
    // The new-tab row is tab-shaped, so the list reserves its height
    v.plusH = v.rowH;
    v.listBottom = (float)app.height - dpi(app, 6.0f) - v.plusH;
    int count = (int)app.tabs.size();
    v.visibleCount =
        std::max(1, (int)((v.listBottom - v.listTop - dpi(app, 2.0f)) / v.rowH));
    if (v.visibleCount > count) v.visibleCount = std::max(count, 1);
    v.firstVisible = std::clamp(app.tabVertFirstVisible, 0,
                                std::max(0, count - v.visibleCount));
    const int active = std::clamp(app.activeTab, 0, std::max(0, count - 1));
    if (active < v.firstVisible) v.firstVisible = active;
    if (active >= v.firstVisible + v.visibleCount)
        v.firstVisible = active - v.visibleCount + 1;
    // Directly under the last row on screen, never past the rail's bottom
    int rows = std::min(v.visibleCount, count);
    v.plusTop = v.listTop + rows * v.rowH + dpi(app, 2.0f);
    v.plusTop = std::min(v.plusTop,
                         (float)app.height - dpi(app, 6.0f) - v.plusH);
    return v;
}

StripMetrics stripMetrics(const App& app) {
    StripMetrics m;
    m.height = chromeTopHeight(app);
    // Tabs clear the edit-mode tool rail instead of hiding under it
    m.tabsLeft = std::max(dpi(app, 40.0f),
                          editRailWidth(app) + dpi(app, 8.0f));
    float plusW = dpi(app, 30.0f);
    float chevronW = dpi(app, 32.0f);
    float gap = dpi(app, 2.0f);
    int count = std::max((int)app.tabs.size(), 1);

    // The tab row ends where the title bar's drag gap begins
    float rowRight = titleDragRect(app).left;
    float available = std::max(0.0f, rowRight - m.tabsLeft - plusW - dpi(app, 4.0f));
    float natural = dpi(app, 190.0f);
    float minimum = dpi(app, 60.0f);
    float per = (available - gap * (count - 1)) / (float)count;
    m.compressed = count > 1 && per < dpi(app, 150.0f);
    if (m.compressed) {
        available = std::max(0.0f, available - chevronW - gap);
    }
    // Overflow uses the existing switcher rather than covering the drag
    // region or caption buttons. Keep the active tab in the visible range.
    m.visibleCount = std::min(count, std::max(1, (int)((available + gap) / (minimum + gap))));
    m.tabWidth = std::min(natural, (available - gap * (m.visibleCount - 1)) / m.visibleCount);
    m.firstVisible = std::clamp(app.tabFirstVisible, 0, count - m.visibleCount);
    const int active = std::clamp(app.activeTab, 0, count - 1);
    if (active < m.firstVisible) m.firstVisible = active;
    if (active >= m.firstVisible + m.visibleCount) m.firstVisible = active - m.visibleCount + 1;
    m.tabsRight = m.tabsLeft + (m.tabWidth + gap) * m.visibleCount - gap;
    m.plusX = m.tabsRight + dpi(app, 4.0f);
    m.chevronX = m.plusX + plusW + gap;
    return m;
}

// App icon in the caption's left cell: the app-menu button. Shared by the
// top strip and the vertical rail (which keeps the same caption bar).
void drawCaptionIcon(App& app, float stripH) {
    float iconCell = dpi(app, 40.0f);
    if (!app.titleIconBitmap && app.wicFactory && app.renderTarget) {
        HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"IDI_ICON1",
                                       IMAGE_ICON, 32, 32, 0);
        if (icon) {
            IWICBitmap* wicBitmap = nullptr;
            if (SUCCEEDED(app.wicFactory->CreateBitmapFromHICON(
                    icon, &wicBitmap)) && wicBitmap) {
                IWICFormatConverter* converter = nullptr;
                if (SUCCEEDED(app.wicFactory->CreateFormatConverter(
                        &converter)) && converter) {
                    if (SUCCEEDED(converter->Initialize(
                            wicBitmap, GUID_WICPixelFormat32bppPBGRA,
                            WICBitmapDitherTypeNone, nullptr, 0.0,
                            WICBitmapPaletteTypeCustom))) {
                        app.renderTarget->CreateBitmapFromWicBitmap(
                            converter, nullptr, &app.titleIconBitmap);
                    }
                    converter->Release();
                }
                wicBitmap->Release();
            }
            DestroyIcon(icon);
        }
    }
    if (!app.editMode || app.editorReadingPreview)
        renderAppMenuButtonBackground(app);
    if (app.titleIconBitmap) {
        float iconSize = dpi(app, 16.0f);
        float ix = (iconCell - iconSize) * 0.5f;
        float iy = (stripH - iconSize) * 0.5f;
        app.renderTarget->DrawBitmap(
            app.titleIconBitmap,
            D2D1::RectF(ix, iy, ix + iconSize, iy + iconSize));
    }
}

}  // namespace

// Right edge of the "+" (and the overflow chevron when compressed)
float tabStripControlsRight(const App& app) {
    StripMetrics m = stripMetrics(app);
    return m.compressed ? m.chevronX + dpi(app, 32.0f) : m.plusX + dpi(app, 30.0f);
}

// Caption geometry, shared with the remote/recent overlays that anchor to
// the title bar
float captionButtonSlotWidth(const App& app) { return dpi(app, 46.0f); }
float pinButtonSlotWidth(const App& app) { return dpi(app, 34.0f); }

int tabDropInsertionIndex(const App& app, POINT clientPoint) {
    const int count = (int)app.tabs.size();
    if (leftTabsRailShown(app)) {
        // Vertical rail: drops land by row, like reordering
        if (clientPoint.x < 0 ||
            clientPoint.x >= (LONG)leftTabsRailWidth(app)) {
            return count;
        }
        VertMetrics v = vertMetrics(app);
        float rowY = (float)clientPoint.y - v.listTop;
        int row = (int)(rowY / v.rowH);
        return std::clamp(v.firstVisible + row, 0, count);
    }
    const auto m = stripMetrics(app);
    if (!tabStripVisible(app) || clientPoint.y < 0 || clientPoint.y >= m.height ||
        clientPoint.x < 0 || clientPoint.x >= captionIslandLeft(app)) {
        return count;
    }
    const float step = m.tabWidth + dpi(app, 2.0f);
    for (int i = 0; i < m.visibleCount; ++i) {
        if (clientPoint.x < m.tabsLeft + step * i + m.tabWidth / 2) return m.firstVisible + i;
    }
    return m.firstVisible + m.visibleCount;
}

bool tabReceiveCopyData(App& app, HWND hwnd, const COPYDATASTRUCT& data) {
    // Type 1 is the existing path-only launch message. Type 2 adds a
    // screen-space drop point, resolved using the destination's own DPI
    // and tab widths before opening the document changes its layout.
    if (!data.lpData || (data.dwData != 1 && data.dwData != 2)) return false;
    const size_t prefix = data.dwData == 2 ? sizeof(POINT) : 0;
    if (data.cbData <= prefix + 1) return false;
    const char* bytes = static_cast<const char*>(data.lpData);
    const char* path = bytes + prefix;
    const size_t length = data.cbData - prefix - 1;
    if (path[length] != '\0' || std::memchr(path, '\0', length)) return false;
    int index = -1;
    if (data.dwData == 2) {
        POINT point;
        std::memcpy(&point, bytes, sizeof(point));
        if (!ScreenToClient(hwnd, &point)) return false;
        index = tabDropInsertionIndex(app, point);
    }
    tabOpenPath(app, hwnd, std::string(path, length), true, index);
    return true;
}

bool tabSendDrop(HWND target, const std::string& utf8Path, POINT screenPoint) {
    std::string payload(sizeof(screenPoint), '\0');
    std::memcpy(payload.data(), &screenPoint, sizeof(screenPoint));
    payload += utf8Path;
    payload += '\0';
    COPYDATASTRUCT data{};
    data.dwData = 2;
    data.cbData = (DWORD)payload.size();
    data.lpData = payload.data();
    return SendMessageW(target, WM_COPYDATA, 0, (LPARAM)&data) != FALSE;
}

D2D1_RECT_F captionButtonRect(const App& app, int button) {
    float w = captionButtonWidth(app);
    float right = (float)app.width - w * (2 - button);
    return D2D1::RectF(right - w, 0.0f, right, chromeTopHeight(app));
}

// Left edge of the caption controls: the pin and the window buttons
float captionIslandLeft(const App& app) {
    return (float)app.width - captionButtonWidth(app) * 3.0f -
           pinButtonWidth(app) - dpi(app, 8.0f);
}

D2D1_RECT_F titleDragRect(const App& app) {
    float right = pinButtonRect(app).left;
    right = std::max(appMenuButtonRect(app).right, right);
    // A very narrow split pane still needs a title/tab context target and
    // room for its controls. Reduce the gap only after that space runs out.
    const float controlsLeft = std::max(dpi(app, 40.0f), editRailWidth(app) + dpi(app, 8.0f));
    const float gap = std::clamp(right - controlsLeft - dpi(app, 84.0f), 0.0f, dpi(app, 32.0f));
    return D2D1::RectF(right - gap, 0.0f, right, chromeTopHeight(app));
}

int captionHitTest(const App& app, float x, float y) {
    if (y < 0.0f || y >= chromeTopHeight(app)) return 0;
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = captionButtonRect(app, i);
        if (x >= r.left && x < r.right) return i + 1;
    }
    return 0;
}

// Recent-documents clock: opens the three-section menu. It lives in the
// strip's right slot (or the rail's top corner), never in the caption
// island, so the window's drag area keeps its full width.
void drawHistoryButton(App& app, const D2D1_COLOR_F& text) {
    D2D1_RECT_F r = historyButtonRect(app);
    if (r.right - r.left < 1.0f) return;  // no room in this layout
    bool hover = (float)app.mouseX >= r.left && (float)app.mouseX <= r.right &&
                 (float)app.mouseY >= r.top && (float)app.mouseY <= r.bottom;
    float radius = dpi(app, 6.0f);
    if (hover || app.showHistoryMenu) {
        D2D1_COLOR_F bg = text;
        bg.a = hover ? 0.10f : 0.06f;
        app.brush->SetColor(bg);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(r, radius, radius), app.brush);
    }
    float cx = (r.left + r.right) * 0.5f;
    float cy = (r.top + r.bottom) * 0.5f;
    D2D1_COLOR_F glyph = text;
    if (!hover && !app.showHistoryMenu) glyph.a = 0.55f;
    app.brush->SetColor(glyph);
    float rad = dpi(app, 4.6f);
    app.renderTarget->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), app.brush,
        dpi(app, 1.1f));
    app.renderTarget->DrawLine(D2D1::Point2F(cx, cy),
                               D2D1::Point2F(cx, cy - rad * 0.62f),
                               app.brush, dpi(app, 1.1f));
    app.renderTarget->DrawLine(
        D2D1::Point2F(cx, cy),
        D2D1::Point2F(cx + rad * 0.5f, cy + rad * 0.36f), app.brush,
        dpi(app, 1.1f));
    App::TabHit historyHit;
    historyHit.rect = r;
    historyHit.index = -5;
    app.tabHits.push_back(historyHit);
}

// --- rendering ---

D2D1_COLOR_F tabStripBackground(const App& app) {
    D2D1_COLOR_F c = app.theme.background;
    float f = app.theme.isDark ? 0.78f : 0.955f;
    c.r *= f;
    c.g *= f;
    c.b *= f;
    return c;
}

namespace {

D2D1_COLOR_F stripBackground(const App& app) {
    return tabStripBackground(app);
}

// Rounded-top tab body: rounded rect extended past the strip bottom, the
// caller clips to the strip so the bottom corners stay square. The vertical
// rail passes extend=false: its rows sit flush against each other, so the
// tail would spill onto the row below.
void fillTabBody(App& app, const D2D1_RECT_F& r, float radius,
                 const D2D1_COLOR_F& color, bool extend = true) {
    app.brush->SetColor(color);
    app.renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(r.left, r.top, r.right,
                        r.bottom + (extend ? radius * 2.0f : 0.0f)),
            radius, radius),
        app.brush);
}

void drawCloseGlyph(App& app, float cx, float cy, float arm,
                    const D2D1_COLOR_F& color) {
    app.brush->SetColor(color);
    app.renderTarget->DrawLine(D2D1::Point2F(cx - arm, cy - arm),
                               D2D1::Point2F(cx + arm, cy + arm),
                               app.brush, dpi(app, 1.1f));
    app.renderTarget->DrawLine(D2D1::Point2F(cx - arm, cy + arm),
                               D2D1::Point2F(cx + arm, cy - arm),
                               app.brush, dpi(app, 1.1f));
}

// The + new-tab button, shared by the full strip and the tabless title
// (where it is the visible hint that the window can hold tabs)
void drawPlusButton(App& app, float plusX, float stripH,
                    const D2D1_COLOR_F& faint, const D2D1_COLOR_F& muted) {
    float plusSize = dpi(app, 30.0f);
    float plusY = (stripH - plusSize) * 0.5f;
    D2D1_RECT_F plusRect = D2D1::RectF(plusX, plusY, plusX + plusSize,
                                       plusY + plusSize);
    bool plusHover = app.mouseX >= plusRect.left &&
                     app.mouseX <= plusRect.right &&
                     app.mouseY >= plusRect.top &&
                     app.mouseY <= plusRect.bottom;
    if (plusHover) {
        app.brush->SetColor(faint);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(plusRect, dpi(app, 6.0f), dpi(app, 6.0f)),
            app.brush);
    }
    app.brush->SetColor(muted);
    float pcx = (plusRect.left + plusRect.right) * 0.5f;
    float pcy = (plusRect.top + plusRect.bottom) * 0.5f;
    float parm = dpi(app, 5.0f);
    app.renderTarget->DrawLine(D2D1::Point2F(pcx - parm, pcy),
                               D2D1::Point2F(pcx + parm, pcy), app.brush,
                               dpi(app, 1.2f));
    app.renderTarget->DrawLine(D2D1::Point2F(pcx, pcy - parm),
                               D2D1::Point2F(pcx, pcy + parm), app.brush,
                               dpi(app, 1.2f));
    App::TabHit plusHit;
    plusHit.rect = plusRect;
    plusHit.index = -2;
    app.tabHits.push_back(plusHit);
}

// Reading-progress ring (tab decoration): the sweep starts at 12 o'clock
// and runs clockwise; the read portion is green over a gray track. A
// finished document (>= 99.9%) trades the ring for a green check badge.
void drawProgressRing(App& app, float cx, float cy, float radius,
                      float progress) {
    D2D1_COLOR_F green = D2D1::ColorF(0.29f, 0.73f, 0.36f);
    float stroke = dpi(app, 2.0f);
    if (progress >= 0.999f) {
        app.brush->SetColor(green);
        app.renderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), app.brush);
        // White check on the green disc: short down-left arm, long up-right
        app.brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        float sw = dpi(app, 1.6f);
        app.renderTarget->DrawLine(
            D2D1::Point2F(cx - radius * 0.45f, cy + radius * 0.02f),
            D2D1::Point2F(cx - radius * 0.10f, cy + radius * 0.38f),
            app.brush, sw);
        app.renderTarget->DrawLine(
            D2D1::Point2F(cx - radius * 0.10f, cy + radius * 0.38f),
            D2D1::Point2F(cx + radius * 0.50f, cy - radius * 0.32f),
            app.brush, sw);
        return;
    }
    // Gray track: the unread remainder
    D2D1_COLOR_F gray = app.theme.text;
    gray.a = 0.25f;
    app.brush->SetColor(gray);
    app.renderTarget->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), app.brush,
        stroke);
    if (progress <= 0.005f) return;
    // Green arc from the top, clockwise: one path-geometry arc segment
    float sweepRad = std::min(progress, 0.999f) * 360.0f * 0.017453293f;
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(app.d2dFactory->CreatePathGeometry(&geo)) || !geo) return;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(geo->Open(&sink)) && sink) {
        D2D1_ARC_SEGMENT arc{};
        // The ellipse radii: leaving them zero collapses the arc into a
        // chord (a straight line to the end point)
        arc.size = D2D1::SizeF(radius, radius);
        arc.point = D2D1::Point2F(cx + radius * sinf(sweepRad),
                                  cy - radius * cosf(sweepRad));
        arc.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
        arc.arcSize = sweepRad > 3.1415927f ? D2D1_ARC_SIZE_LARGE
                                            : D2D1_ARC_SIZE_SMALL;
        sink->BeginFigure(D2D1::Point2F(cx, cy - radius),
                          D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(arc);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        app.brush->SetColor(green);
        app.renderTarget->DrawGeometry(geo, app.brush, stroke);
    }
    geo->Release();
}

// Tiny "ssh" wordmark in front of a remote document's tab label: marks the
// tab as a read-only remote file at a glance. Plain coloured text, no chip
// behind it, tinted with the theme's main colour (theme.heading) - the same
// colour as the document headings, so a purple-heading theme gets a purple
// ssh. Returns the width it took so the caller can shift the label.
float drawSshBadge(App& app, float left, float centerY) {
    const float badgeH = dpi(app, 12.0f);
    const wchar_t* label = L"ssh";
    if (!app.folderBrowserFormat) return 0.0f;
    float textW = measureText(app, label, app.folderBrowserFormat);
    if (textW <= 0.0f) return 0.0f;

    D2D1_COLOR_F color = app.theme.heading;
    color.a = 0.95f;
    app.brush->SetColor(color);
    app.renderTarget->DrawText(
        label, (UINT32)wcslen(label), app.folderBrowserFormat,
        D2D1::RectF(left, centerY - badgeH * 0.5f, left + textW,
                    centerY + badgeH * 0.5f),
        app.brush);
    return textW;
}

// Feeds the active tab's progress from the scroll position, mirroring the
// app's own scroll clamp (viewport height = window height). Both directions:
// scrolling back up lowers the ring again. Re-evaluates when the scroll
// moved or the measured tab changed -- the latter catches a fresh document
// whose restored offset happens to equal the previous one's, and a short
// document that reads as complete the moment it is shown. A manual
// read/unread mark parks the anchor, so it sticks until the next scroll.
// Runs every frame from renderTabStrip; also tracks in zen mode, where the
// strip is hidden.
void updateTabReadProgress(App& app) {
    if (app.editMode || app.tabs.empty()) return;
    // The reader lays out viewport-first: the first pass of a frame
    // reports a partial contentHeight, only the completed pass reports
    // the document's real height. Measuring on the partial pass would
    // briefly show a wildly wrong ring (a tenth of the document read).
    if (!app.layoutComplete) return;
    float contentH = app.contentHeight;
    if (contentH <= 1.0f) return;
    if (app.activeTab < 0 || app.activeTab >= (int)app.tabs.size()) return;
    App::DocTab& tab = app.tabs[app.activeTab];
    if (tab.path.empty()) return;
    bool moved = fabsf(app.scrollY - app.readProgressAnchorY) >= 0.01f;
    bool retarget = app.readProgressSeenTab != tab.id;
    if (!moved && !retarget) return;
    app.readProgressSeenTab = tab.id;
    app.readProgressAnchorY = app.scrollY;
    // The ring mirrors where the reader is right now: scrolling back up
    // gives the progress back instead of holding a high-water mark
    float progress = (app.scrollY + (float)app.height) / contentH;
    tab.readProgress = std::clamp(progress, 0.0f, 1.0f);
}

}  // namespace

static void renderCaptionChrome(App& app, const D2D1_COLOR_F& text);

void renderTabStrip(App& app) {
    // Reading progress follows the scroll even when the strip is hidden
    // (zen mode returns below before any drawing)
    updateTabReadProgress(app);
    float stripH = chromeTopHeight(app);
    if (stripH <= 0.0f) return;
    tabsInit(app);
    syncActiveTab(app);

    D2D1_COLOR_F text = app.theme.text;
    D2D1_COLOR_F muted = text;
    muted.a = 0.7f;
    D2D1_COLOR_F faint = text;
    faint.a = 0.06f;

    // ---- vertical rail (tabsOnLeft) --------------------------------------
    // The rail replaces the top tab row; the top bar keeps the caption
    // (drag, pin, min/max/close) and is drawn by the shared code below.
    if (leftTabsRailShown(app)) {
        VertMetrics v = vertMetrics(app);
        app.tabVertFirstVisible = v.firstVisible;

        // Rail surface, under the top bar's strip background
        app.brush->SetColor(stripBackground(app));
        app.renderTarget->FillRectangle(
            D2D1::RectF(0.0f, 0.0f, v.width, (float)app.height), app.brush);

        // Hairline separating rail from the page
        D2D1_COLOR_F edge = text;
        edge.a = 0.10f;
        app.brush->SetColor(edge);
        app.renderTarget->FillRectangle(
            D2D1::RectF(v.width - 1.0f, 0.0f, v.width, (float)app.height),
            app.brush);

        // The rail covers the caption corner; the app-menu icon still owns
        // the top-left cell in this placement
        drawCaptionIcon(app, chromeTopHeight(app));

        float rowH = v.rowH;
        float radius = dpi(app, 6.0f);

        for (int i = v.firstVisible;
             i < v.firstVisible + v.visibleCount && i < (int)app.tabs.size();
             i++) {
            if (app.tabDragDetached && i == app.tabDragIndex) continue;
            bool active = i == app.activeTab;
            bool hovered = i == app.hoveredTab;
            float y = v.listTop + rowH * (i - v.firstVisible);
            if (app.tabDragging && i == app.tabDragIndex) {
                // The dragged row follows the pointer within the rail
                float follow = (float)app.mouseY - app.tabDragOffsetY;
                y = std::max(v.listTop,
                             std::min(follow, v.listBottom - rowH));
            }
            float inset = v.rowX0 + dpi(app, 3.0f);
            D2D1_RECT_F r = D2D1::RectF(inset, y, v.width - v.gutterW - 1.0f,
                                         y + rowH - dpi(app, 2.0f));

            if (active) {
                // Accent bar on the left edge + lifted body, like the top
                // strip's active tab but turned upright
                float shadowAlpha = app.theme.isDark ? 0.30f : 0.14f;
                for (int ring = 3; ring >= 1; ring--) {
                    float spread = dpi(app, (float)ring * 2.0f);
                    D2D1_COLOR_F shadow =
                        D2D1::ColorF(0.0f, 0.0f, 0.0f,
                                     shadowAlpha / (float)(ring * ring));
                    fillTabBody(app,
                                D2D1::RectF(r.left - spread,
                                            r.top - spread * 0.6f,
                                            r.right + spread, r.bottom),
                                radius + spread * 0.5f, shadow, false);
                }
                fillTabBody(app, r, radius, app.theme.background, false);
                app.brush->SetColor(app.theme.accent);
                app.renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        D2D1::RectF(r.left, r.top + dpi(app, 6.0f),
                                    r.left + dpi(app, 3.0f),
                                    r.bottom - dpi(app, 6.0f)),
                        dpi(app, 1.5f), dpi(app, 1.5f)),
                    app.brush);
            } else if (hovered) {
                fillTabBody(app, r, radius, faint, false);
            }

            bool dirty = active ? (app.editMode && app.editorDirty)
                                : (app.tabs[i].editMode &&
                                   app.tabs[i].editorDirty);
            bool missing = app.tabs[i].fileMissing &&
                           !app.tabs[i].path.empty();
            bool showClose = active || hovered;
            D2D1_COLOR_F dotColor =
                dirty ? D2D1::ColorF(0.94f, 0.56f, 0.12f)
                      : D2D1::ColorF(0.74f, 0.36f, 0.34f);

            // Reading-progress ring at the text head, then the file name:
            // horizontal text, rail width minus the ring and the
            // close/dot reserve, ellipsis-trimmed by DirectWrite.
            // Remote tabs wear a small "ssh" tag before the label.
            bool tabRemote = remotePathIs(app.tabs[i].path);
            float reserve = showClose ? dpi(app, 34.0f) : dpi(app, 16.0f);
            float labelLeft = r.left + v.padX;
            if (tabRemote) {
                labelLeft += drawSshBadge(app, labelLeft,
                                          (r.top + r.bottom) * 0.5f) +
                             dpi(app, 5.0f);
            }
            if (!app.tabs[i].path.empty()) {
                drawProgressRing(app, labelLeft + dpi(app, 8.0f),
                                 r.top + (r.bottom - r.top) * 0.5f,
                                 dpi(app, 7.0f),
                                 app.tabs[i].readProgress);
                labelLeft += dpi(app, 22.0f);
            }
            D2D1_RECT_F label = D2D1::RectF(
                labelLeft, r.top,
                r.right - reserve, r.bottom);
            if (app.folderBrowserFormat &&
                label.right > label.left + dpi(app, 8.0f)) {
                app.brush->SetColor(active ? text : muted);
                app.renderTarget->DrawText(
                    app.tabs[i].title.c_str(),
                    (UINT32)app.tabs[i].title.size(),
                    app.folderBrowserFormat,
                    D2D1::RectF(label.left,
                                r.top + (r.bottom - r.top - dpi(app, 17.0f)) * 0.5f,
                                label.right, r.bottom),
                    app.brush);
            }

            App::TabHit hit;
            hit.rect = r;
            hit.index = i;
            if (label.right > label.left + dpi(app, 8.0f)) {
                hit.labelRect = D2D1::RectF(label.left, r.top, label.right,
                                            r.bottom);
            }
            hit.contextRect = r;
            if (showClose) {
                float cbSize = dpi(app, 22.0f);
                float cbX = r.right - cbSize - dpi(app, 6.0f);
                float cbY = r.top + (rowH - dpi(app, 2.0f) - cbSize) * 0.5f;
                D2D1_RECT_F cb = D2D1::RectF(cbX, cbY, cbX + cbSize,
                                             cbY + cbSize);
                bool closeHover =
                    app.mouseX >= cb.left && app.mouseX <= cb.right &&
                    app.mouseY >= cb.top && app.mouseY <= cb.bottom;
                if (closeHover || !active) {
                    D2D1_COLOR_F cbBg = text;
                    cbBg.a = closeHover ? 0.12f : 0.07f;
                    app.brush->SetColor(cbBg);
                    app.renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(cb, dpi(app, 4.0f), dpi(app, 4.0f)),
                        app.brush);
                }
                if ((dirty || missing) && !closeHover && !hovered) {
                    app.brush->SetColor(dotColor);
                    float dotR = dpi(app, 3.5f);
                    app.renderTarget->FillEllipse(
                        D2D1::Ellipse(
                            D2D1::Point2F((cb.left + cb.right) * 0.5f,
                                          (cb.top + cb.bottom) * 0.5f),
                            dotR, dotR),
                        app.brush);
                } else {
                    drawCloseGlyph(app, (cb.left + cb.right) * 0.5f,
                                   (cb.top + cb.bottom) * 0.5f,
                                   dpi(app, 3.6f), muted);
                }
                hit.closeRect = cb;
                hit.hasClose = true;
            } else if (dirty || missing) {
                app.brush->SetColor(dotColor);
                float dotR = dpi(app, 3.5f);
                float cx = r.right - dpi(app, 14.0f);
                app.renderTarget->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(cx, r.top + (r.bottom - r.top) * 0.5f),
                        dotR, dotR),
                    app.brush);
            }
            app.tabHits.push_back(hit);
        }

        // Overflow: the rail scrolls with the wheel; a small counter row
        // under the list shows where you are (e.g. "3/12")
        {
            int count = (int)app.tabs.size();
            if (count > v.visibleCount) {
                wchar_t counter[32];
                swprintf_s(counter, _countof(counter), L"%d/%d",
                           std::min(v.firstVisible + v.visibleCount, count),
                           count);
                D2D1_COLOR_F c = text;
                c.a = 0.45f;
                app.brush->SetColor(c);
                if (app.folderBrowserFormat) {
                    app.renderTarget->DrawText(
                        counter, (UINT32)wcslen(counter),
                        app.folderBrowserFormat,
                        D2D1::RectF(v.padX, v.listBottom - dpi(app, 16.0f),
                                    v.width - v.gutterW - 4.0f,
                                    v.listBottom + dpi(app, 2.0f)),
                        app.brush);
                }
            }
        }

        // New-tab row: tab-shaped and directly under the last tab, so it
        // reads as part of the list instead of a floating square
        {
            float inset = v.rowX0 + dpi(app, 3.0f);
            D2D1_RECT_F plusRect =
                D2D1::RectF(inset, v.plusTop, v.width - v.gutterW - 1.0f,
                            v.plusTop + v.plusH - dpi(app, 2.0f));
            bool plusHover = app.mouseX >= plusRect.left &&
                             app.mouseX <= plusRect.right &&
                             app.mouseY >= plusRect.top &&
                             app.mouseY <= plusRect.bottom;
            if (plusHover) {
                app.brush->SetColor(faint);
                app.renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(plusRect, radius, radius), app.brush);
            }
            // Glyph in the icon slot, label where tab names begin
            float pcx = plusRect.left + v.padX + dpi(app, 8.0f);
            float pcy = (plusRect.top + plusRect.bottom) * 0.5f;
            float parm = dpi(app, 4.5f);
            app.brush->SetColor(plusHover ? text : muted);
            app.renderTarget->DrawLine(
                D2D1::Point2F(pcx - parm, pcy),
                D2D1::Point2F(pcx + parm, pcy), app.brush, dpi(app, 1.2f));
            app.renderTarget->DrawLine(
                D2D1::Point2F(pcx, pcy - parm),
                D2D1::Point2F(pcx, pcy + parm), app.brush, dpi(app, 1.2f));
            if (app.folderBrowserFormat) {
                app.brush->SetColor(plusHover ? text : muted);
                app.renderTarget->DrawText(
                    tr(app, "tabs.new"),
                    (UINT32)wcslen(tr(app, "tabs.new")),
                    app.folderBrowserFormat,
                    D2D1::RectF(plusRect.left + v.padX + dpi(app, 22.0f),
                                plusRect.top +
                                    (plusRect.bottom - plusRect.top -
                                     dpi(app, 17.0f)) * 0.5f,
                                plusRect.right - dpi(app, 8.0f),
                                plusRect.bottom),
                    app.brush);
            }
            App::TabHit plusHit;
            plusHit.rect = plusRect;
            plusHit.index = -2;
            app.tabHits.push_back(plusHit);
        }

        // Resize gutter on the rail's right edge: a hand-drawn grip
        // (the live highlight comes from renderTabRailResizeGrip)
        drawHistoryButton(app, text);
        renderCaptionChrome(app, text);
        return;
    }

    // Strip background: the full width in every mode, the docked edit
    // preview included (#245)
    app.brush->SetColor(stripBackground(app));
    app.renderTarget->FillRectangle(
        D2D1::RectF(0, 0, (float)app.width, stripH), app.brush);

    drawCaptionIcon(app, stripH);

    app.tabHits.clear();

    bool tabless = !tabStripVisible(app);
    if (tabless) {
        // Single document: the caption shows the plain window title, with
        // the + button right after it so the tab row is discoverable
        std::wstring title = L"Scher";
        if (!app.tabs.empty() && !app.tabs[0].title.empty() &&
            !startPageActive(app)) {
            title = app.tabs[0].title;
            if (app.editMode && app.editorDirty) title = L"* " + title;
        }
        float textLeft = std::max(dpi(app, 44.0f),
                                  editRailWidth(app) + dpi(app, 8.0f));
        // The right-click target starts at the icon edge so the ring does
        // not eat into it
        float hitLeft = textLeft;
        // A lone remote document wears the "ssh" tag ahead of its ring
        if (!app.tabs.empty() && !app.tabs[0].path.empty() &&
            remotePathIs(app.tabs[0].path)) {
            textLeft += drawSshBadge(app, textLeft, stripH * 0.5f) +
                        dpi(app, 5.0f);
        }
        // The lone document wears its progress ring ahead of the title
        if (!app.currentFile.empty() && !startPageActive(app) &&
            !app.tabs.empty()) {
            drawProgressRing(app, textLeft + dpi(app, 8.0f), stripH * 0.5f,
                             dpi(app, 7.0f), app.tabs[0].readProgress);
            textLeft += dpi(app, 22.0f);
        }
        float textRight = textLeft;
        float maxRight = titleDragRect(app).left;
        float labelRight = std::max(textLeft, maxRight - dpi(app, 74.0f));
        if (app.folderBrowserFormat) {
            app.brush->SetColor(muted);
            app.renderTarget->DrawText(
                title.c_str(), (UINT32)title.size(), app.folderBrowserFormat,
                D2D1::RectF(textLeft, (stripH - dpi(app, 17.0f)) * 0.5f,
                            labelRight, stripH),
                app.brush);
            IDWriteTextLayout* layout = nullptr;
            app.dwriteFactory->CreateTextLayout(
                title.c_str(), (UINT32)title.size(), app.folderBrowserFormat,
                std::max(1.0f, labelRight - textLeft), stripH, &layout);
            if (layout) {
                DWRITE_TEXT_METRICS tm{};
                layout->GetMetrics(&tm);
                textRight += tm.width;
                layout->Release();
            }
        }
        // A document's lone tab still closes: a small x after the title
        // lands on the start page. The launcher itself shows none - the
        // window's own close button is the way out there.
        float nextX = textRight + dpi(app, 10.0f);
        if (!startPageActive(app)) {
            float cbSize = dpi(app, 20.0f);
            float cbX = std::min(nextX, maxRight - dpi(app, 38.0f) -
                                            cbSize - dpi(app, 6.0f));
            float cbY = (stripH - cbSize) * 0.5f;
            D2D1_RECT_F cb =
                D2D1::RectF(cbX, cbY, cbX + cbSize, cbY + cbSize);
            bool closeHover =
                app.mouseX >= cb.left && app.mouseX <= cb.right &&
                app.mouseY >= cb.top && app.mouseY <= cb.bottom;
            if (closeHover) {
                D2D1_COLOR_F cbBg = text;
                cbBg.a = 0.12f;
                app.brush->SetColor(cbBg);
                app.renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(cb, dpi(app, 4.0f), dpi(app, 4.0f)),
                    app.brush);
            }
            drawCloseGlyph(app, (cb.left + cb.right) * 0.5f,
                           (cb.top + cb.bottom) * 0.5f, dpi(app, 3.6f),
                           muted);
            App::TabHit hit;
            hit.rect = cb;
            hit.index = 0;
            hit.closeRect = cb;
            hit.hasClose = true;
            hit.contextRect = D2D1::RectF(hitLeft, 0, cb.right, stripH);
            app.tabHits.push_back(hit);
            nextX = cb.right + dpi(app, 6.0f);
        }
        float plusX = std::min(nextX, maxRight - dpi(app, 38.0f));
        drawPlusButton(app, plusX, stripH, faint, muted);
    } else {
        StripMetrics m = stripMetrics(app);
        app.tabFirstVisible = m.firstVisible;
        float radius = dpi(app, 8.0f);
        float gap = dpi(app, 2.0f);

        app.renderTarget->PushAxisAlignedClip(
            D2D1::RectF(m.tabsLeft, 0, m.tabsRight, stripH),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        for (int i = m.firstVisible; i < m.firstVisible + m.visibleCount; i++) {
            // Pulled clear of the strip: the slot stays as a gap while
            // the tab rides along as the ghost card
            if (app.tabDragDetached && (int)i == app.tabDragIndex) continue;
            bool active = (int)i == app.activeTab;
            bool hovered = (int)i == app.hoveredTab;
            float x = m.tabsLeft + (m.tabWidth + gap) * (i - m.firstVisible);
            if (app.tabDragging && (int)i == app.tabDragIndex) {
                // The dragged tab follows the pointer within the row
                float follow = (float)app.mouseX - app.tabDragOffsetX;
                x = std::max(m.tabsLeft,
                             std::min(follow, m.tabsRight - m.tabWidth));
            }
            float top = active ? dpi(app, 6.0f) : dpi(app, 10.0f);
            D2D1_RECT_F r = D2D1::RectF(x, top, x + m.tabWidth, stripH);

            if (active) {
                // The active tab drops a soft shadow onto its neighbors
                // instead of an underline: layered halos, clipped to the
                // strip so they spread sideways over the inactive tabs
                float shadowAlpha = app.theme.isDark ? 0.34f : 0.16f;
                for (int ring = 3; ring >= 1; ring--) {
                    float spread = dpi(app, (float)ring * 2.2f);
                    D2D1_COLOR_F shadow =
                        D2D1::ColorF(0.0f, 0.0f, 0.0f,
                                     shadowAlpha / (float)(ring * ring));
                    fillTabBody(app,
                                D2D1::RectF(r.left - spread,
                                            r.top - spread * 0.6f,
                                            r.right + spread, r.bottom),
                                radius + spread * 0.5f, shadow);
                }
                fillTabBody(app, r, radius, app.theme.background);
            } else if (hovered) {
                fillTabBody(app, r, radius, faint);
            }

            bool dirty = active ? (app.editMode && app.editorDirty)
                                : (app.tabs[i].editMode &&
                                   app.tabs[i].editorDirty);
            bool missing = app.tabs[i].fileMissing && !app.tabs[i].path.empty();
            bool showClose = (active || hovered) && m.tabWidth >= dpi(app, 48.0f);
            // Status dot: orange = unsaved changes, red-grey = the file
            // vanished from disk
            bool showDot = dirty || missing;
            D2D1_COLOR_F dotColor =
                dirty ? D2D1::ColorF(0.94f, 0.56f, 0.12f)
                      : D2D1::ColorF(0.74f, 0.36f, 0.34f);

            // Progress ring ahead of the label when the tab is wide
            // enough; the label keeps its ellipsis trim either way.
            // Remote tabs wear a small "ssh" tag before the label.
            bool isRemote = remotePathIs(app.tabs[i].path);
            bool showRing =
                !app.tabs[i].path.empty() && m.tabWidth >= dpi(app, 76.0f);
            float labelLeft = r.left + dpi(app, 12.0f);
            if (isRemote && m.tabWidth >= dpi(app, 90.0f)) {
                labelLeft += drawSshBadge(app, labelLeft,
                                          (top + stripH) * 0.5f) +
                             dpi(app, 5.0f);
            }
            if (showRing) {
                drawProgressRing(app, r.left + dpi(app, 20.0f),
                                 (top + stripH) * 0.5f, dpi(app, 7.0f),
                                 app.tabs[i].readProgress);
                labelLeft += dpi(app, 22.0f);
            }
            float labelRight = r.right - dpi(app, showClose ? 30.0f : 20.0f);
            if (app.folderBrowserFormat &&
                labelRight > labelLeft + dpi(app, 4.0f)) {
                app.brush->SetColor(active ? text : muted);
                app.renderTarget->DrawText(
                    app.tabs[i].title.c_str(), (UINT32)app.tabs[i].title.size(),
                    app.folderBrowserFormat,
                    D2D1::RectF(labelLeft,
                                top + (stripH - top - dpi(app, 17.0f)) * 0.5f,
                                labelRight, stripH),
                    app.brush);
            }

            App::TabHit hit;
            hit.rect = r;
            hit.index = (int)i;
            if (labelRight > labelLeft + dpi(app, 4.0f)) {
                hit.labelRect = D2D1::RectF(labelLeft, r.top,
                                            labelRight, r.bottom);
            }
            if (showClose) {
                float cbSize = dpi(app, 20.0f);
                float cbX = r.right - cbSize - dpi(app, 6.0f);
                float cbY = top + (stripH - top - cbSize) * 0.5f;
                D2D1_RECT_F cb = D2D1::RectF(cbX, cbY, cbX + cbSize,
                                             cbY + cbSize);
                bool closeHover =
                    app.mouseX >= cb.left && app.mouseX <= cb.right &&
                    app.mouseY >= cb.top && app.mouseY <= cb.bottom;
                if (closeHover || !active) {
                    D2D1_COLOR_F cbBg = text;
                    cbBg.a = closeHover ? 0.12f : 0.07f;
                    app.brush->SetColor(cbBg);
                    app.renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(cb, dpi(app, 4.0f), dpi(app, 4.0f)),
                        app.brush);
                }
                if (showDot && !closeHover && !hovered) {
                    // status dot instead of x until the pointer commits
                    app.brush->SetColor(dotColor);
                    float dotR = dpi(app, 3.5f);
                    app.renderTarget->FillEllipse(
                        D2D1::Ellipse(
                            D2D1::Point2F((cb.left + cb.right) * 0.5f,
                                          (cb.top + cb.bottom) * 0.5f),
                            dotR, dotR),
                        app.brush);
                } else {
                    drawCloseGlyph(app, (cb.left + cb.right) * 0.5f,
                                   (cb.top + cb.bottom) * 0.5f,
                                   dpi(app, 3.6f), muted);
                }
                hit.closeRect = cb;
                hit.hasClose = true;
            } else if (showDot) {
                app.brush->SetColor(dotColor);
                float dotR = dpi(app, 3.5f);
                float cx = r.right - dpi(app, 13.0f);
                app.renderTarget->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(cx, top + (stripH - top) * 0.5f),
                        dotR, dotR),
                    app.brush);
            }
            app.tabHits.push_back(hit);
        }
        app.renderTarget->PopAxisAlignedClip();

        // + new tab
        drawPlusButton(app, m.plusX, stripH, faint, muted);
        float plusSize = dpi(app, 30.0f);
        float plusY = (stripH - plusSize) * 0.5f;

        // Overflow chevron when compressed
        if (m.compressed) {
            float chW = dpi(app, 30.0f);
            D2D1_RECT_F chRect = D2D1::RectF(m.chevronX, plusY,
                                             m.chevronX + chW,
                                             plusY + plusSize);
            D2D1_COLOR_F chBg = text;
            chBg.a = 0.07f;
            app.brush->SetColor(chBg);
            app.renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(chRect, dpi(app, 6.0f), dpi(app, 6.0f)),
                app.brush);
            app.brush->SetColor(muted);
            float ccx = (chRect.left + chRect.right) * 0.5f;
            float ccy = (chRect.top + chRect.bottom) * 0.5f - dpi(app, 1.0f);
            float carm = dpi(app, 3.5f);
            app.renderTarget->DrawLine(
                D2D1::Point2F(ccx - carm, ccy),
                D2D1::Point2F(ccx, ccy + carm), app.brush, dpi(app, 1.2f));
            app.renderTarget->DrawLine(
                D2D1::Point2F(ccx, ccy + carm),
                D2D1::Point2F(ccx + carm, ccy), app.brush, dpi(app, 1.2f));
            App::TabHit chHit;
            chHit.rect = chRect;
            chHit.index = -3;
            app.tabHits.push_back(chHit);
        }
    }

    // ---- shared caption chrome (both placements) -------------------------
    // Caption buttons and the pin; in left mode only this island draws.
    drawHistoryButton(app, text);
    renderCaptionChrome(app, text);
}

// Caption buttons (min/max/close) and the always-on-top pin. Shared by
// the top strip and the vertical rail, which keeps the top bar as its
// caption.
static void renderCaptionChrome(App& app, const D2D1_COLOR_F& text) {
    for (int b = 0; b < 3; b++) {
        D2D1_RECT_F r = captionButtonRect(app, b);
        bool hover = app.captionButtonHover == b + 1;
        bool pressed = app.captionButtonPressed == b + 1;
        if (hover || pressed) {
            D2D1_COLOR_F bg;
            if (b == 2) {
                bg = D2D1::ColorF(0.77f, 0.17f, 0.11f, pressed ? 0.8f : 1.0f);
            } else {
                bg = text;
                bg.a = pressed ? 0.12f : 0.07f;
            }
            app.brush->SetColor(bg);
            app.renderTarget->FillRectangle(r, app.brush);
        }
        D2D1_COLOR_F glyph = (b == 2 && (hover || pressed))
                                 ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
                                 : text;
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f;
        app.brush->SetColor(glyph);
        if (b == 0) {
            app.renderTarget->DrawLine(
                D2D1::Point2F(cx - dpi(app, 5.0f), cy),
                D2D1::Point2F(cx + dpi(app, 5.0f), cy), app.brush,
                dpi(app, 1.0f));
        } else if (b == 1) {
            float half = dpi(app, 4.5f);
            if (IsZoomed(app.hwnd)) {
                // restore: two offset squares
                float off = dpi(app, 2.0f);
                app.renderTarget->DrawRectangle(
                    D2D1::RectF(cx - half + off, cy - half - off + dpi(app, 1.0f),
                                cx + half + off - dpi(app, 1.0f), cy + half - off),
                    app.brush, dpi(app, 1.0f));
                app.brush->SetColor(stripBackground(app));
                app.renderTarget->FillRectangle(
                    D2D1::RectF(cx - half, cy - half, cx + half, cy + half),
                    app.brush);
                app.brush->SetColor(glyph);
            }
            app.renderTarget->DrawRectangle(
                D2D1::RectF(cx - half, cy - half, cx + half, cy + half),
                app.brush, dpi(app, 1.0f));
        } else {
            drawCloseGlyph(app, cx, cy, dpi(app, 4.5f), glyph);
        }
    }

    // Always-on-top pin: an upright pushpin, accent-filled while pinned
    {
        D2D1_RECT_F r = pinButtonRect(app);
        bool hover = (float)app.mouseX >= r.left && (float)app.mouseX < r.right &&
                     (float)app.mouseY >= r.top && (float)app.mouseY < r.bottom;
        if (hover) {
            D2D1_COLOR_F bg = text;
            bg.a = 0.07f;
            app.brush->SetColor(bg);
            app.renderTarget->FillRectangle(r, app.brush);
        }
        float cx = (r.left + r.right) * 0.5f;
        float cy = (r.top + r.bottom) * 0.5f - dpi(app, 1.0f);
        D2D1_COLOR_F pin = app.alwaysOnTop ? app.theme.accent : text;
        if (!app.alwaysOnTop && !hover) pin.a = 0.45f;
        app.brush->SetColor(pin);
        float bw = dpi(app, 3.0f);   // body half-width
        float bh = dpi(app, 4.0f);   // body half-height
        D2D1_RECT_F body = D2D1::RectF(cx - bw, cy - bh, cx + bw, cy + bh - 1);
        if (app.alwaysOnTop) {
            app.renderTarget->FillRectangle(body, app.brush);
        } else {
            app.renderTarget->DrawRectangle(body, app.brush, dpi(app, 1.0f));
        }
        // Head bar, flange, and needle
        app.renderTarget->DrawLine(
            D2D1::Point2F(cx - bw - dpi(app, 1.0f), cy - bh),
            D2D1::Point2F(cx + bw + dpi(app, 1.0f), cy - bh), app.brush,
            dpi(app, 1.2f));
        app.renderTarget->DrawLine(
            D2D1::Point2F(cx - bw - dpi(app, 2.0f), cy + bh - 1),
            D2D1::Point2F(cx + bw + dpi(app, 2.0f), cy + bh - 1), app.brush,
            dpi(app, 1.2f));
        app.renderTarget->DrawLine(
            D2D1::Point2F(cx, cy + bh - 1),
            D2D1::Point2F(cx, cy + bh + dpi(app, 4.0f)), app.brush,
            dpi(app, 1.2f));
        App::TabHit pinHit;
        pinHit.rect = r;
        pinHit.index = -4;
        app.tabHits.push_back(pinHit);
    }
}

void renderTabSwitcher(App& app) {
    if (!app.showTabSwitcher || app.tabs.size() < 2) return;
    float stripH = chromeTopHeight(app);
    float width = dpi(app, 280.0f);
    float rowH = dpi(app, 32.0f);
    float headH = dpi(app, 26.0f);
    float padY = dpi(app, 4.0f);
    float height = headH + padY * 2.0f + rowH * (float)app.tabs.size();
    float x = (float)app.width - captionButtonWidth(app) * 3.0f - width -
              dpi(app, 8.0f);
    if (x < dpi(app, 8.0f)) x = dpi(app, 8.0f);
    float y = stripH + dpi(app, 4.0f);

    D2D1_COLOR_F text = app.theme.text;
    D2D1_COLOR_F panel = stripBackground(app);
    panel.a = 0.97f;
    D2D1_COLOR_F border = text;
    border.a = 0.12f;

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + width, y + height), dpi(app, 8.0f),
        dpi(app, 8.0f));
    app.brush->SetColor(panel);
    app.renderTarget->FillRoundedRectangle(rr, app.brush);
    app.brush->SetColor(border);
    app.renderTarget->DrawRoundedRectangle(rr, app.brush, 1.0f);

    app.tabSwitcherHits.clear();

    // Caps header: OPEN FILES - N
    if (app.themeHeaderFormat) {
        wchar_t header[96];
        swprintf_s(header, _countof(header), L"%ls \u2014 %d",
                   tr(app, "tabs.switcher.header"), (int)app.tabs.size());
        D2D1_COLOR_F headColor = text;
        headColor.a = 0.5f;
        app.brush->SetColor(headColor);
        app.renderTarget->DrawText(
            header, (UINT32)wcslen(header), app.themeHeaderFormat,
            D2D1::RectF(x + dpi(app, 14.0f), y + dpi(app, 9.0f), x + width,
                        y + headH),
            app.brush);
    }

    for (size_t i = 0; i < app.tabs.size(); i++) {
        bool active = (int)i == app.activeTab;
        bool hovered = (int)i == app.tabSwitcherHover;
        float rowY = y + headH + padY + rowH * (float)i;
        D2D1_RECT_F row = D2D1::RectF(x + dpi(app, 4.0f), rowY,
                                      x + width - dpi(app, 4.0f),
                                      rowY + rowH - dpi(app, 2.0f));
        if (active || hovered) {
            D2D1_COLOR_F rowBg = text;
            rowBg.a = active ? 0.09f : 0.05f;
            app.brush->SetColor(rowBg);
            app.renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(row, dpi(app, 5.0f), dpi(app, 5.0f)),
                app.brush);
        }
        if (active) {
            app.brush->SetColor(app.theme.accent);
            app.renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(row.left, rowY + dpi(app, 7.0f),
                                row.left + dpi(app, 3.0f),
                                rowY + rowH - dpi(app, 9.0f)),
                    dpi(app, 1.5f), dpi(app, 1.5f)),
                app.brush);
        }
        // Document glyph
        D2D1_COLOR_F glyph = text;
        glyph.a = 0.55f;
        app.brush->SetColor(glyph);
        float gx = row.left + dpi(app, 12.0f);
        float gy = rowY + (rowH - dpi(app, 14.0f)) * 0.5f - dpi(app, 1.0f);
        app.renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(gx, gy, gx + dpi(app, 11.0f), gy + dpi(app, 14.0f)),
                dpi(app, 2.0f), dpi(app, 2.0f)),
            app.brush, 1.0f);

        bool dirty = active ? (app.editMode && app.editorDirty)
                            : (app.tabs[i].editMode && app.tabs[i].editorDirty);
        bool missing = app.tabs[i].fileMissing && !app.tabs[i].path.empty();
        bool showDot = dirty || missing;
        float nameRight = row.right - dpi(app, showDot ? 66.0f : 56.0f);
        if (app.folderBrowserFormat) {
            D2D1_COLOR_F nameColor = text;
            if (!active) nameColor.a = 0.8f;
            app.brush->SetColor(nameColor);
            app.renderTarget->DrawText(
                app.tabs[i].title.c_str(), (UINT32)app.tabs[i].title.size(),
                app.folderBrowserFormat,
                D2D1::RectF(gx + dpi(app, 19.0f),
                            rowY + (rowH - dpi(app, 17.0f)) * 0.5f - 1.0f,
                            nameRight, rowY + rowH),
                app.brush);
        }
        if (showDot) {
            app.brush->SetColor(dirty ? D2D1::ColorF(0.94f, 0.56f, 0.12f)
                                      : D2D1::ColorF(0.74f, 0.36f, 0.34f));
            float dotR = dpi(app, 3.5f);
            app.renderTarget->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(row.right - dpi(app, 60.0f),
                                            rowY + rowH * 0.5f - 1.0f),
                              dotR, dotR),
                app.brush);
        }
        // Ctrl+1..9 hint
        if (i < 9 && app.folderBrowserFormat) {
            wchar_t hint[16];
            swprintf_s(hint, _countof(hint), L"Ctrl+%d", (int)i + 1);
            D2D1_COLOR_F hintColor = text;
            hintColor.a = 0.4f;
            app.brush->SetColor(hintColor);
            app.renderTarget->DrawText(
                hint, (UINT32)wcslen(hint), app.folderBrowserFormat,
                D2D1::RectF(row.right - dpi(app, 54.0f),
                            rowY + (rowH - dpi(app, 17.0f)) * 0.5f - 1.0f,
                            row.right - dpi(app, 4.0f), rowY + rowH),
                app.brush);
        }
        app.tabSwitcherHits.push_back({row, (int)i});
    }
}

// --- Tab hover card (#246) ---
//
// A pointer resting on a tab brings up a small card below the strip: the
// tab's whole name, the folder it lives in and, when the tab carries a
// dot, what the dot means. A name the strip cut short reads in full, and
// two README.md tabs from different projects stop looking alike. The
// first card waits for a dwell; once one has been up, gliding along the
// strip swaps cards almost at once. Suggested by Alex (@Lex987).

namespace {

constexpr UINT TAB_HOVER_CARD_DWELL_MS = 600;
constexpr UINT TAB_HOVER_CARD_WARM_MS = 60;
constexpr ULONGLONG TAB_HOVER_CARD_WARM_WINDOW_MS = 800;

const App::TabHit* tabHitFor(const App& app, int tab) {
    for (const App::TabHit& hit : app.tabHits) {
        if (hit.index == tab) return &hit;
    }
    return nullptr;
}

// Menus, dialogs and drags own the pointer: no card over them
bool tabHoverCardBlocked(const App& app) {
    return app.showTabSwitcher || app.showTabMenu || app.showContextMenu ||
           app.confirmExitPending || app.createRefPending || app.showHelp ||
           app.showSettings || app.showThemeChooser || app.showThemeEditor ||
           app.showShortcutEditor || app.showLightbox ||
           app.showPrintPreview || app.tabDragIndex >= 0;
}

// A zero-width space after each separator lets a long folder wrap
// between its parts rather than in the middle of a name
std::wstring breakablePath(const std::wstring& path) {
    std::wstring out;
    out.reserve(path.size() + path.size() / 4);
    for (wchar_t ch : path) {
        out += ch;
        if (ch == L'\\' || ch == L'/') out += wchar_t(0x200B);
    }
    return out;
}

// Wrapped to the card's width and cut to `maxLines`; a longer text ends
// in the format's ellipsis
IDWriteTextLayout* hoverCardLayout(App& app, const std::wstring& text,
                                   IDWriteTextFormat* format, float width,
                                   UINT32 maxLines, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextLayout* layout = nullptr;
    if (text.empty() || !format ||
        FAILED(app.dwriteFactory->CreateTextLayout(
            text.c_str(), (UINT32)text.size(), format, width, 10000.0f,
            &layout)) ||
        !layout) {
        return nullptr;
    }
    useUiFontFallback(app, layout);
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
    layout->SetFontWeight(weight, {0, (UINT32)text.size()});
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    if (tm.lineCount > maxLines) {
        std::vector<DWRITE_LINE_METRICS> lines(tm.lineCount);
        UINT32 count = 0;
        if (SUCCEEDED(layout->GetLineMetrics(lines.data(), tm.lineCount,
                                             &count))) {
            float height = 0.0f;
            for (UINT32 i = 0; i < maxLines && i < count; i++) {
                height += lines[i].height;
            }
            layout->SetMaxHeight(height);
        }
    }
    return layout;
}

void hoverCardExtent(IDWriteTextLayout* layout, float& width, float& height) {
    width = height = 0.0f;
    if (!layout) return;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    width = tm.width;
    height = std::min(tm.height, layout->GetMaxHeight());
}

}  // namespace

TabHoverCardText tabHoverCardText(const App& app, int tab) {
    TabHoverCardText card;
    if (tab < 0 || tab >= (int)app.tabs.size()) return card;
    const App::DocTab& doc = app.tabs[tab];
    card.name = doc.title;
    if (!doc.path.empty()) {
        std::wstring wide = toWide(doc.path);
        size_t sep = wide.find_last_of(L"\\/");
        if (sep != std::wstring::npos) {
            card.folder = wide.substr(0, sep);
            // A drive root keeps its separator: "C:\" rather than "C:"
            if (card.folder.size() == 2 && card.folder[1] == L':') {
                card.folder += L'\\';
            }
        }
    }
    bool active = tab == app.activeTab;
    card.dirty = active ? (app.editMode && app.editorDirty)
                        : (doc.editMode && doc.editorDirty);
    if (card.dirty) {
        card.status = tr(app, "tabs.card.unsaved");
    } else if (doc.fileMissing && !doc.path.empty()) {
        card.status = tr(app, "tabs.card.missing");
    }
    return card;
}

void tabHoverCardTrack(App& app, HWND hwnd, int tab) {
    // A card put away over a tab stays away until the pointer moves on
    if (tab != app.tabHoverCardQuiet) app.tabHoverCardQuiet = -1;
    if (tab >= 0 &&
        (tab == app.tabHoverCardTab || tab == app.tabHoverCardPending)) {
        return;
    }
    bool wasUp = app.tabHoverCardTab >= 0;
    if (wasUp) app.tabHoverCardHiddenAt = GetTickCount64();
    app.tabHoverCardTab = app.tabHoverCardPending = -1;
    KillTimer(hwnd, TIMER_TAB_HOVER_CARD);
    if (tab >= 0 && tab != app.tabHoverCardQuiet && tabStripVisible(app)) {
        bool warm = app.tabHoverCardHiddenAt != 0 &&
                    GetTickCount64() - app.tabHoverCardHiddenAt <
                        TAB_HOVER_CARD_WARM_WINDOW_MS;
        app.tabHoverCardPending = tab;
        SetTimer(hwnd, TIMER_TAB_HOVER_CARD,
                 warm ? TAB_HOVER_CARD_WARM_MS : TAB_HOVER_CARD_DWELL_MS,
                 nullptr);
        // The gaps between tabs belong to the caption: hear the pointer
        // slip out of the client area there
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
    }
    if (wasUp) InvalidateRect(hwnd, nullptr, FALSE);
}

void tabHoverCardDismiss(App& app, HWND hwnd) {
    int tab = app.tabHoverCardTab >= 0 ? app.tabHoverCardTab
                                       : app.tabHoverCardPending;
    if (tab < 0) return;
    KillTimer(hwnd, TIMER_TAB_HOVER_CARD);
    bool wasUp = app.tabHoverCardTab >= 0;
    app.tabHoverCardTab = app.tabHoverCardPending = -1;
    // Put away on purpose: no quick follow-up from the neighbours
    app.tabHoverCardQuiet = tab;
    app.tabHoverCardHiddenAt = 0;
    if (wasUp) InvalidateRect(hwnd, nullptr, FALSE);
}

void tabHoverCardLeave(App& app, HWND hwnd) {
    tabHoverCardTrack(app, hwnd, -1);
    app.tabHoverCardQuiet = -1;
    // The strip forgets its hover too, so no tab stays lit
    if (app.hoveredTab >= 0) {
        app.hoveredTab = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void handleTabHoverCardTimer(App& app, HWND hwnd) {
    KillTimer(hwnd, TIMER_TAB_HOVER_CARD);
    int tab = app.tabHoverCardPending;
    app.tabHoverCardPending = -1;
    if (tab < 0 || tab >= (int)app.tabs.size() || tab != app.hoveredTab) {
        return;
    }
    app.tabHoverCardTab = tab;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void renderTabHoverCard(App& app) {
    app.tabHoverCardRect = D2D1::RectF(0, 0, 0, 0);
    int tab = app.tabHoverCardTab;
    if (tab < 0 || tab >= (int)app.tabs.size() || tab != app.hoveredTab ||
        !tabStripVisible(app) || tabHoverCardBlocked(app) ||
        !app.folderBrowserFormat || !app.tocFormat) {
        return;
    }
    // Only while the pointer is still on that tab
    const App::TabHit* hit = tabHitFor(app, tab);
    if (!hit || (float)app.mouseX < hit->rect.left ||
        (float)app.mouseX > hit->rect.right ||
        (float)app.mouseY < hit->rect.top ||
        (float)app.mouseY > hit->rect.bottom) {
        return;
    }
    TabHoverCardText card = tabHoverCardText(app, tab);
    // A tab without a file adds nothing unless the strip cut its title
    // short or it holds unsaved changes
    if (card.folder.empty() && card.status.empty()) {
        float room = hit->labelRect.right - hit->labelRect.left;
        if (room > 0.0f &&
            measureText(app, card.name, app.folderBrowserFormat) <= room) {
            return;
        }
    }

    float padX = dpi(app, 12.0f);
    float padY = dpi(app, 9.0f);
    float gap = dpi(app, 3.0f);
    float margin = dpi(app, 8.0f);
    float dotIndent = dpi(app, 13.0f);
    float textW = std::min(dpi(app, 420.0f),
                           (float)app.width - margin * 2.0f - padX * 2.0f);
    if (textW < dpi(app, 60.0f)) return;

    IDWriteTextLayout* name =
        hoverCardLayout(app, card.name, app.folderBrowserFormat, textW, 4,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextLayout* folder =
        hoverCardLayout(app, breakablePath(card.folder), app.tocFormat, textW,
                        4, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextLayout* status =
        hoverCardLayout(app, card.status, app.tocFormat, textW - dotIndent, 2,
                        DWRITE_FONT_WEIGHT_NORMAL);
    float nameW, nameH, folderW, folderH, statusW, statusH;
    hoverCardExtent(name, nameW, nameH);
    hoverCardExtent(folder, folderW, folderH);
    hoverCardExtent(status, statusW, statusH);
    float contentW = std::max(nameW, folderW);
    if (status) contentW = std::max(contentW, statusW + dotIndent);
    float w = std::ceil(contentW) + padX * 2.0f;
    float h = padY * 2.0f + nameH + (folder ? gap + folderH : 0.0f) +
              (status ? gap + statusH : 0.0f);
    float x = std::max(margin,
                       std::min(hit->rect.left, (float)app.width - w - margin));
    float y = chromeTopHeight(app) + dpi(app, 6.0f);
    if (leftTabsRailShown(app)) {
        // Vertical rail: the card floats beside the row, not under a strip
        x = std::min(leftTabsRailWidth(app) + margin,
                     (float)app.width - w - margin);
        y = std::clamp(hit->rect.top, chromeTopHeight(app) + dpi(app, 4.0f),
                       std::max(chromeTopHeight(app) + dpi(app, 4.0f),
                                (float)app.height - h - margin));
    }
    D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
    float radius = dpi(app, 8.0f);

    // A soft lift off the page underneath, then the strip's own surface
    float shadowAlpha = app.theme.isDark ? 0.30f : 0.10f;
    for (int ring = 2; ring >= 1; ring--) {
        float spread = dpi(app, (float)ring * 2.0f);
        app.brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f,
                                         shadowAlpha / (float)(ring * ring)));
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(rect.left - spread, rect.top - spread * 0.5f,
                            rect.right + spread, rect.bottom + spread * 1.5f),
                radius + spread, radius + spread),
            app.brush);
    }
    D2D1_COLOR_F text = app.theme.text;
    D2D1_COLOR_F panel = stripBackground(app);
    panel.a = 1.0f;  // opaque: no page text ghosting through
    D2D1_COLOR_F border = text;
    border.a = 0.12f;
    D2D1_COLOR_F muted = text;
    muted.a = 0.62f;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, radius, radius);
    app.brush->SetColor(panel);
    app.renderTarget->FillRoundedRectangle(rr, app.brush);
    app.brush->SetColor(border);
    app.renderTarget->DrawRoundedRectangle(rr, app.brush, 1.0f);

    float ty = y + padY;
    if (name) {
        app.brush->SetColor(text);
        app.renderTarget->DrawTextLayout(D2D1::Point2F(x + padX, ty), name,
                                         app.brush,
                                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        ty += nameH;
    }
    if (folder) {
        ty += gap;
        app.brush->SetColor(muted);
        app.renderTarget->DrawTextLayout(D2D1::Point2F(x + padX, ty), folder,
                                         app.brush,
                                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        ty += folderH;
    }
    if (status) {
        ty += gap;
        // The strip's dot, explained: orange unsaved, red-grey gone
        DWRITE_TEXT_METRICS tm{};
        status->GetMetrics(&tm);
        std::vector<DWRITE_LINE_METRICS> lines(std::max(1u, tm.lineCount));
        UINT32 count = 0;
        status->GetLineMetrics(lines.data(), (UINT32)lines.size(), &count);
        DWRITE_LINE_METRICS first = lines[0];
        float dotR = dpi(app, 3.5f);
        app.brush->SetColor(card.dirty ? D2D1::ColorF(0.94f, 0.56f, 0.12f)
                                       : D2D1::ColorF(0.74f, 0.36f, 0.34f));
        app.renderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(x + padX + dotR,
                                        ty + first.height * 0.5f),
                          dotR, dotR),
            app.brush);
        app.brush->SetColor(muted);
        app.renderTarget->DrawTextLayout(
            D2D1::Point2F(x + padX + dotIndent, ty), status, app.brush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    for (IDWriteTextLayout* layout : {name, folder, status}) {
        if (layout) layout->Release();
    }
    app.tabHoverCardRect = rect;
}

// --- input ---

bool tabStripMouseDown(App& app, HWND hwnd, int x, int y, bool middle) {
    // The vertical rail hit-tests from the layout as it is now. Using the
    // rects cached by the last paint would act on the previous tab count:
    // adding a tab moves the new-tab row under the pointer, and a click on
    // the new tab's close button would land on that stale row and add yet
    // another tab.
    if (leftTabsRailShown(app) && (float)x < leftTabsRailWidth(app)) {
        VertMetrics v = vertMetrics(app);
        int count = (int)app.tabs.size();
        float left = v.rowX0 + dpi(app, 3.0f);
        float right = v.width - v.gutterW - 1.0f;
        if (x >= left && x <= right) {
            for (int i = v.firstVisible;
                 i < v.firstVisible + v.visibleCount && i < count; i++) {
                float top = v.listTop + v.rowH * (float)(i - v.firstVisible);
                float bottom = top + v.rowH - dpi(app, 2.0f);
                if ((float)y < top || (float)y > bottom) continue;
                bool showClose = i == app.activeTab || i == app.hoveredTab;
                if (showClose) {
                    float cbSize = dpi(app, 22.0f);
                    float cbX = right - cbSize - dpi(app, 6.0f);
                    float cbY = top + (v.rowH - dpi(app, 2.0f) - cbSize) * 0.5f;
                    if ((float)x >= cbX && (float)x <= cbX + cbSize &&
                        (float)y >= cbY && (float)y <= cbY + cbSize) {
                        if (!middle) tabCloseIndex(app, hwnd, i);
                        return true;
                    }
                }
                if (middle) {
                    tabCloseIndex(app, hwnd, i);
                    return true;
                }
                tabActivate(app, hwnd, i);
                app.tabDragIndex = i;
                app.tabDragging = false;
                app.tabDragOffsetX = (float)x - left;
                app.tabDragOffsetY = (float)y - top;
                app.tabDragStartX = x;
                app.tabDragStartY = y;
                SetCapture(hwnd);
                return true;
            }
            if ((float)y >= v.plusTop &&
                (float)y <= v.plusTop + v.plusH - dpi(app, 2.0f)) {
                if (!middle) tabOpenQuickNote(app, hwnd);
                return true;
            }
        }
        return true;  // rail presses never reach the document
    }
    bool overRail = false;
    if ((float)y >= chromeTopHeight(app) && !overRail) return false;
    for (const App::TabHit& hit : app.tabHits) {
        if (x < hit.rect.left || x > hit.rect.right || y < hit.rect.top ||
            y > hit.rect.bottom) {
            continue;
        }
        if (hit.index == -2) {
            // + = new untitled quick-note tab (Ctrl+T still opens the
            // browser into a new tab for picking an existing file)
            if (!middle) tabOpenQuickNote(app, hwnd);
            return true;
        }
        if (hit.index == -3) {
            if (!middle) {
                app.showTabSwitcher = !app.showTabSwitcher;
                app.tabSwitcherHover = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return true;
        }
        if (hit.index == -4) {
            // Pin: toggle always-on-top for this window
            if (!middle) {
                app.alwaysOnTop = !app.alwaysOnTop;
                SetWindowPos(hwnd,
                             app.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                             0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return true;
        }
        if (middle) {
            tabCloseIndex(app, hwnd, hit.index);
            return true;
        }
        if (hit.hasClose && x >= hit.closeRect.left &&
            x <= hit.closeRect.right && y >= hit.closeRect.top &&
            y <= hit.closeRect.bottom) {
            tabCloseIndex(app, hwnd, hit.index);
            return true;
        }
        tabActivate(app, hwnd, hit.index);
        // Arm a potential drag: moving past the threshold starts
        // reordering, pulling away from the strip detaches the tab
        app.tabDragIndex = hit.index;
        app.tabDragging = false;
        app.tabDragOffsetX = (float)x - hit.rect.left;
        app.tabDragOffsetY = (float)y - hit.rect.top;
        app.tabDragStartX = x;
        app.tabDragStartY = y;
        SetCapture(hwnd);
        return true;
    }
    return true;  // strip clicks never fall through to the document
}

bool tabStripVisible(const App& app) {
    return app.tabs.size() > 1 || (app.forceTabStrip && !app.tabs.empty());
}

// --- vertical rail helpers ---

bool tabRailHitZone(const App& app, float x, float y) {
    return leftTabsRailShown(app) && x >= 0.0f &&
           x < leftTabsRailWidth(app) && y >= 0.0f &&
           y <= (float)app.height;
}

void tabRailWheel(App& app, float delta) {
    if (!leftTabsRailShown(app)) return;
    int count = (int)app.tabs.size();
    if (count < 1) return;
    VertMetrics v = vertMetrics(app);
    if (count <= v.visibleCount) return;  // everything already fits
    int dir = delta > 0 ? -1 : 1;        // wheel up = scroll toward the top
    int next = std::clamp(app.tabVertFirstVisible + dir, 0,
                          count - v.visibleCount);
    if (next != app.tabVertFirstVisible) {
        app.tabVertFirstVisible = next;
        InvalidateRect(app.hwnd, nullptr, FALSE);
    }
}

int tabContextMenuIndexAt(const App& app, float x, float y) {
    if (leftTabsRailShown(app)) {
        if (x < 0 || x >= leftTabsRailWidth(app) ||
            y < 0 || y > (float)app.height) return -1;
    } else if (y < 0 || y >= chromeTopHeight(app)) {
        return -1;
    }
    for (const App::TabHit& hit : app.tabHits) {
        if (hit.index < 0 || hit.index >= (int)app.tabs.size()) continue;
        const auto& rect = hit.contextRect.right > hit.contextRect.left
                               ? hit.contextRect : hit.rect;
        if (x >= rect.left && x <= rect.right &&
            y >= rect.top && y <= rect.bottom) return hit.index;
    }
    return -1;
}

// --- tab context menu: right-click a tab for NPP-style close operations
// (close, close others, close left/right) plus path utilities ---

namespace {

enum TabMenuItem {
    TM_MARK_READ = 0,
    TM_MARK_UNREAD,
    TM_CLOSE,
    TM_CLOSE_OTHERS,
    TM_CLOSE_LEFT,
    TM_CLOSE_RIGHT,
    TM_COPY_PATH,
    TM_REVEAL,
    TM_ITEM_COUNT,
};

struct TabMenuEntry {
    const char* key;         // translation key
    const wchar_t* shortcut;
    bool separatorAfter;
};

const TabMenuEntry TAB_MENU[TM_ITEM_COUNT] = {
    { "tab.menu.mark_read",    L"",       false },
    { "tab.menu.mark_unread",  L"",       true  },
    { "tab.menu.close",        L"Ctrl+W", false },
    { "tab.menu.close_others", L"",       false },
    { "tab.menu.close_left",   L"",       false },
    { "tab.menu.close_right",  L"",       true  },
    { "tab.menu.copy_path",    L"",       false },
    { "ctx.reveal",            L"",       false },
};

float tabMenuItemHeight(const App& app) { return dpi(app, 30.0f); }
float tabMenuSeparator(const App& app) { return dpi(app, 9.0f); }
float tabMenuWidth(const App& app) { return dpi(app, 235.0f); }
float tabMenuPadding(const App& app) { return dpi(app, 6.0f); }

float tabMenuItemTop(const App& app, int index) {
    float y = tabMenuPadding(app);
    for (int i = 0; i < index; i++) {
        y += tabMenuItemHeight(app);
        if (TAB_MENU[i].separatorAfter) y += tabMenuSeparator(app);
    }
    return y;
}

float tabMenuTotalHeight(const App& app) {
    return tabMenuItemTop(app, TM_ITEM_COUNT) + tabMenuPadding(app);
}

bool tabMenuItemEnabled(const App& app, int item) {
    int index = app.tabMenuIndex;
    if (index < 0 || index >= (int)app.tabs.size()) return false;
    switch (item) {
        case TM_MARK_READ:
        case TM_MARK_UNREAD:
        case TM_COPY_PATH:
        case TM_REVEAL:
            return !(index == app.activeTab ? app.currentFile : app.tabs[index].path).empty();
        case TM_CLOSE:        return true;
        case TM_CLOSE_OTHERS: return app.tabs.size() > 1;
        case TM_CLOSE_LEFT:   return index > 0;
        case TM_CLOSE_RIGHT:  return index + 1 < (int)app.tabs.size();
    }
    return false;
}

int tabIndexById(const App& app, int id) {
    for (size_t i = 0; i < app.tabs.size(); i++) {
        if (app.tabs[i].id == id) return (int)i;
    }
    return -1;
}

void tabBulkCloseBegin(App& app, HWND hwnd, int mode, int keepIndex) {
    if (keepIndex < 0 || keepIndex >= (int)app.tabs.size()) return;
    app.tabBulkCloseMode = mode;
    app.tabBulkKeepId = app.tabs[keepIndex].id;
    tabBulkCloseStep(app, hwnd);
}

}  // namespace

void tabBulkCloseStep(App& app, HWND hwnd) {
    // Sweeps clean victims immediately; a dirty one opens the #106 dialog
    // and the sweep resumes from confirmExitAction (Keep editing aborts)
    while (app.tabBulkCloseMode) {
        int keep = tabIndexById(app, app.tabBulkKeepId);
        if (keep < 0) {
            app.tabBulkCloseMode = 0;
            return;
        }
        int victim = -1;
        if (app.tabBulkCloseMode != 3 && keep > 0) {
            victim = 0;  // everything left of the kept tab, one by one
        } else if (app.tabBulkCloseMode != 2 &&
                   keep + 1 < (int)app.tabs.size()) {
            victim = keep + 1;  // everything to its right
        }
        if (victim < 0) {
            app.tabBulkCloseMode = 0;
            if (keep != app.activeTab) tabActivate(app, hwnd, keep);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        tabCloseIndex(app, hwnd, victim);
        if (app.confirmExitPending) return;  // waiting on the dialog
    }
}

void openTabMenu(App& app, int tabIndex, float x, float y) {
    float w = tabMenuWidth(app);
    float h = tabMenuTotalHeight(app);
    app.tabMenuIndex = tabIndex;
    app.tabMenuX = std::max(0.0f, std::min(x, (float)app.width - w));
    app.tabMenuY = std::max(0.0f, std::min(y, (float)app.height - h));
    app.showTabMenu = true;
    app.tabMenuHover = -1;
    app.tabMenuAnimation = 0.0f;
    // The strip's own hover state stays quiet underneath
    app.hoveredTab = -1;
}

void closeTabMenu(App& app) {
    app.showTabMenu = false;
    app.tabMenuHover = -1;
    app.tabMenuAnimation = 0.0f;
}

int tabMenuItemAt(const App& app, float x, float y) {
    if (x < app.tabMenuX || x > app.tabMenuX + tabMenuWidth(app)) return -1;
    float rel = y - app.tabMenuY;
    for (int i = 0; i < TM_ITEM_COUNT; i++) {
        float top = tabMenuItemTop(app, i);
        if (rel >= top && rel < top + tabMenuItemHeight(app)) return i;
    }
    return -1;
}

void renderTabMenu(App& app) {
    if (!app.showTabMenu) return;
    if (app.tabMenuIndex < 0 || app.tabMenuIndex >= (int)app.tabs.size()) {
        closeTabMenu(app);
        return;
    }
    if (app.tabMenuAnimation < 1.0f) {
        float prev = app.tabMenuAnimation;
        app.tabMenuAnimation = std::min(1.0f, app.tabMenuAnimation + 0.25f);
        if (app.tabMenuAnimation != prev)
            InvalidateRect(app.hwnd, nullptr, FALSE);
    }
    float anim = app.tabMenuAnimation;

    IDWriteTextFormat* format = app.folderBrowserFormat;
    if (!format) return;

    float x = app.tabMenuX;
    float y = app.tabMenuY;
    float w = tabMenuWidth(app);
    float h = tabMenuTotalHeight(app);

    D2D1_COLOR_F panelBg = app.theme.isDark ? hexColor(0x1E1E1E, 0.97f)
                                            : hexColor(0xF8F8F8, 0.97f);
    D2D1_COLOR_F borderColor = app.theme.isDark ? hexColor(0x3A3A40, 0.9f)
                                                : hexColor(0xC8C8C8, 0.9f);
    app.brush->SetColor(panelBg);
    app.renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 6, 6), app.brush);
    app.brush->SetColor(borderColor);
    app.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 6, 6), app.brush,
        1.0f);

    app.tabMenuHover = tabMenuItemAt(app, (float)app.mouseX, (float)app.mouseY);

    float pad = tabMenuPadding(app);
    float itemH = tabMenuItemHeight(app);
    float inset = dpi(app, 12.0f);
    for (int i = 0; i < TM_ITEM_COUNT; i++) {
        float top = y + tabMenuItemTop(app, i);
        bool enabled = tabMenuItemEnabled(app, i);

        if (i == app.tabMenuHover && enabled) {
            D2D1_COLOR_F hoverColor = app.theme.accent;
            hoverColor.a = 0.18f * anim;
            app.brush->SetColor(hoverColor);
            app.renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(x + pad, top, x + w - pad, top + itemH), 4, 4),
                app.brush);
        }

        D2D1_COLOR_F textColor = app.theme.text;
        textColor.a = (enabled ? 1.0f : 0.35f) * anim;
        app.brush->SetColor(textColor);
        float textY = top + (itemH - dpi(app, 18.0f)) / 2;
        const wchar_t* label = tr(app, TAB_MENU[i].key);
        app.renderTarget->DrawText(
            label, (UINT32)wcslen(label), format,
            D2D1::RectF(x + inset, textY, x + w - inset, top + itemH),
            app.brush);

        if (TAB_MENU[i].shortcut[0]) {
            IDWriteTextLayout* hint = nullptr;
            app.dwriteFactory->CreateTextLayout(
                TAB_MENU[i].shortcut, (UINT32)wcslen(TAB_MENU[i].shortcut),
                format, 1000.0f, itemH, &hint);
            if (hint) {
                DWRITE_TEXT_METRICS metrics{};
                hint->GetMetrics(&metrics);
                D2D1_COLOR_F hintColor = app.theme.text;
                hintColor.a = 0.4f * anim;
                app.brush->SetColor(hintColor);
                app.renderTarget->DrawTextLayout(
                    D2D1::Point2F(x + w - inset - metrics.width, textY), hint,
                    app.brush);
                hint->Release();
            }
        }

        if (TAB_MENU[i].separatorAfter) {
            float sepY = top + itemH + tabMenuSeparator(app) * 0.5f;
            app.brush->SetColor(borderColor);
            app.renderTarget->DrawLine(D2D1::Point2F(x + pad, sepY),
                                       D2D1::Point2F(x + w - pad, sepY),
                                       app.brush, 1.0f);
        }
    }
}

bool tabMenuMouseDown(App& app, HWND hwnd, int x, int y) {
    if (!app.showTabMenu) return false;
    int item = tabMenuItemAt(app, (float)x, (float)y);
    int index = app.tabMenuIndex;
    closeTabMenu(app);
    app.swallowNextMouseUp = true;
    InvalidateRect(hwnd, nullptr, FALSE);
    if (item < 0 || index < 0 || index >= (int)app.tabs.size()) return true;
    // Re-validate against the surviving tab row (indices can shift while
    // the menu is up only via external means; the guard is cheap)
    App& a = app;
    const std::string& path = index == a.activeTab ? a.currentFile : a.tabs[index].path;
    switch (item) {
        case TM_MARK_READ:
        case TM_MARK_UNREAD: {
            if (path.empty()) break;  // untitled/start-page tabs carry none
            float p = (item == TM_MARK_READ) ? 1.0f : 0.0f;
            App::DocTab& t = a.tabs[index];
            t.readProgress = p;
            // Park the auto-tracking anchor so a fresh "unread" is not
            // immediately re-measured from the current scroll position
            if (index == a.activeTab) a.readProgressAnchorY = a.scrollY;
            persistReadProgress(path, p);
            break;
        }
        case TM_CLOSE:
            if (tabMenuItemEnabled(a, TM_CLOSE)) tabCloseIndex(a, hwnd, index);
            break;
        case TM_CLOSE_OTHERS:
            if (a.tabs.size() > 1) tabBulkCloseBegin(a, hwnd, 1, index);
            break;
        case TM_CLOSE_LEFT:
            if (index > 0) tabBulkCloseBegin(a, hwnd, 2, index);
            break;
        case TM_CLOSE_RIGHT:
            if (index + 1 < (int)a.tabs.size())
                tabBulkCloseBegin(a, hwnd, 3, index);
            break;
        case TM_COPY_PATH:
            copyFilePath(a, hwnd, path);
            break;
        case TM_REVEAL: {
            std::wstring fullPath = absoluteFilePath(path);
            if (!fullPath.empty()) {
                std::wstring params = L"/select,\"" + fullPath + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe",
                              params.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        }
    }
    return true;
}

// --- drag ghost: a layered popup card that follows the cursor while a
// tab is pulled clear of the strip, so the detach reads as a motion
// instead of a jump ---

namespace {

const wchar_t* kGhostClass = L"TintaTabGhost";

COLORREF toColorref(const D2D1_COLOR_F& c) {
    return RGB((BYTE)(c.r * 255.0f + 0.5f), (BYTE)(c.g * 255.0f + 0.5f),
               (BYTE)(c.b * 255.0f + 0.5f));
}

void ghostDestroy(App& app) {
    if (app.tabGhostWnd) {
        DestroyWindow(app.tabGhostWnd);
        app.tabGhostWnd = nullptr;
    }
}

// Paints the tab card into a premultiplied ARGB surface and shows it via
// UpdateLayeredWindow (GDI text writes alpha 0, so alpha is rebuilt from
// rounded-rect membership afterwards)
void ghostCreate(App& app, const std::wstring& title, int screenX,
                 int screenY) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kGhostClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    int width = (int)dpi(app, 190.0f);
    int height = (int)dpi(app, 34.0f);
    float radius = dpi(app, 8.0f);

    app.tabGhostWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kGhostClass, L"", WS_POPUP, screenX, screenY, width, height, nullptr,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!app.tabGhostWnd) return;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    if (dib && bits) {
        HGDIOBJ oldBitmap = SelectObject(memory, dib);
        memset(bits, 0, (size_t)width * height * 4);

        // Card: strip-colored fill, subtle border
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                         (int)(radius * 2), (int)(radius * 2));
        HBRUSH fill = CreateSolidBrush(toColorref(tabStripBackground(app)));
        FillRgn(memory, region, fill);
        D2D1_COLOR_F borderColor = app.theme.text;
        HBRUSH border = CreateSolidBrush(toColorref(borderColor));
        FrameRgn(memory, region, border, 1, 1);
        DeleteObject(fill);
        DeleteObject(border);
        DeleteObject(region);

        // Title
        HFONT font = CreateFontW(-(int)dpi(app, 13.0f), 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(memory, font);
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, toColorref(app.theme.text));
        RECT textRect = {(int)dpi(app, 12.0f), 0,
                         width - (int)dpi(app, 10.0f), height};
        DrawTextW(memory, title.c_str(), (int)title.size(), &textRect,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        SelectObject(memory, oldFont);
        DeleteObject(font);

        // Rebuild alpha: opaque inside the rounded card, clear outside
        uint32_t* pixels = static_cast<uint32_t*>(bits);
        float rr = radius;
        for (int py = 0; py < height; py++) {
            for (int px = 0; px < width; px++) {
                float dx = 0.0f, dy = 0.0f;
                if (px < rr && py < rr) { dx = rr - px; dy = rr - py; }
                else if (px >= width - rr && py < rr) { dx = px - (width - 1 - rr); dy = rr - py; }
                else if (px < rr && py >= height - rr) { dx = rr - px; dy = py - (height - 1 - rr); }
                else if (px >= width - rr && py >= height - rr) { dx = px - (width - 1 - rr); dy = py - (height - 1 - rr); }
                bool inside = dx * dx + dy * dy <= rr * rr;
                uint32_t& p = pixels[(size_t)py * width + px];
                p = inside ? (p | 0xFF000000u) : 0u;
            }
        }

        POINT source = {0, 0};
        POINT position = {screenX, screenY};
        SIZE size = {width, height};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 235, AC_SRC_ALPHA};
        UpdateLayeredWindow(app.tabGhostWnd, screen, &position, &size,
                            memory, &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, oldBitmap);
        DeleteObject(dib);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    ShowWindow(app.tabGhostWnd, SW_SHOWNOACTIVATE);
}

void ghostMoveTo(App& app, int screenX, int screenY) {
    if (!app.tabGhostWnd) return;
    SetWindowPos(app.tabGhostWnd, HWND_TOPMOST, screenX, screenY, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}

// Close the dragged tab locally after it moved elsewhere
void dragCloseLocal(App& app, HWND hwnd, int index) {
    if (index < 0 || index >= (int)app.tabs.size()) return;
    if ((int)app.tabs.size() <= 1) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (index == app.activeTab) {
        int next = index + 1 < (int)app.tabs.size() ? index + 1 : index - 1;
        tabActivate(app, hwnd, next);
    }
    app.tabs.erase(app.tabs.begin() + index);
    if (app.activeTab > index) app.activeTab--;
    app.hoveredTab = -1;
}

}  // namespace

void tabDragMove(App& app, HWND hwnd, int x, int y) {
    if (app.tabDragIndex < 0) return;
    if (app.tabDragIndex >= (int)app.tabs.size()) {
        tabDragEnd(app, hwnd, x, y);
        return;
    }

    if (!app.tabDragging) {
        if (std::abs(x - app.tabDragStartX) < (int)dpi(app, 6.0f) &&
            std::abs(y - app.tabDragStartY) < (int)dpi(app, 6.0f)) {
            return;
        }
        app.tabDragging = true;
    }

    int index = app.tabDragIndex;
    bool active = index == app.activeTab;
    bool dirty = active ? (app.editMode && app.editorDirty)
                        : (app.tabs[index].editMode &&
                           app.tabs[index].editorDirty);
    const std::string& path = active ? app.currentFile : app.tabs[index].path;
    bool detachable = !dirty && !path.empty();

    // Clear of the strip: the tab floats as a ghost card until release
    // (the vertical rail detaches when pulled past its right edge)
    float stripH = chromeTopHeight(app);
    bool outside;
    if (leftTabsRailShown(app)) {
        outside = (float)x > leftTabsRailWidth(app) + dpi(app, 48.0f);
    } else {
        outside = (float)y > stripH + dpi(app, 48.0f) ||
                  (float)y < -dpi(app, 48.0f);
    }
    if (outside && detachable) {
        POINT screen = {x, y};
        ClientToScreen(hwnd, &screen);
        // Keep the cursor on the card even when the tab was wider than it
        int grab = std::min((int)app.tabDragOffsetX,
                            (int)dpi(app, 166.0f));
        int ghostX = screen.x - std::max(grab, (int)dpi(app, 12.0f));
        int ghostY = screen.y - (int)dpi(app, 17.0f);
        if (!app.tabDragDetached) {
            app.tabDragDetached = true;
            ghostCreate(app, app.tabs[index].title, ghostX, ghostY);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            ghostMoveTo(app, ghostX, ghostY);
        }
        return;
    }
    if (app.tabDragDetached) {
        // Back over the strip: the tab snaps home and reordering resumes
        app.tabDragDetached = false;
        ghostDestroy(app);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // Reorder: the dragged tab claims the slot under the pointer
    if (leftTabsRailShown(app)) {
        VertMetrics v = vertMetrics(app);
        int slot = v.firstVisible +
                   (int)(((float)y - v.listTop) / v.rowH);
        slot = std::clamp(slot, 0, (int)app.tabs.size() - 1);
        if (slot != app.tabDragIndex) {
            App::DocTab moved = std::move(app.tabs[app.tabDragIndex]);
            app.tabs.erase(app.tabs.begin() + app.tabDragIndex);
            app.tabs.insert(app.tabs.begin() + slot, std::move(moved));
            app.activeTab = slot;
            app.tabDragIndex = slot;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    StripMetrics m = stripMetrics(app);
    float gap = dpi(app, 2.0f);
    int slot = m.firstVisible + (int)(((float)x - m.tabsLeft) / (m.tabWidth + gap));
    slot = std::clamp(slot, m.firstVisible, m.firstVisible + m.visibleCount - 1);
    if (slot != app.tabDragIndex) {
        App::DocTab moved = std::move(app.tabs[app.tabDragIndex]);
        app.tabs.erase(app.tabs.begin() + app.tabDragIndex);
        app.tabs.insert(app.tabs.begin() + slot, std::move(moved));
        // The dragged tab is the active one (the press activated it)
        app.activeTab = slot;
        app.tabDragIndex = slot;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void tabDragEnd(App& app, HWND hwnd, int x, int y) {
    if (app.tabDragIndex < 0) return;
    int index = app.tabDragIndex;
    bool detached = app.tabDragDetached;
    app.tabDragIndex = -1;
    app.tabDragging = false;
    app.tabDragDetached = false;
    ghostDestroy(app);
    if (GetCapture() == hwnd) ReleaseCapture();

    if (detached && index < (int)app.tabs.size()) {
        bool active = index == app.activeTab;
        const std::string& path =
            active ? app.currentFile : app.tabs[index].path;
        POINT screen = {x, y};
        ClientToScreen(hwnd, &screen);

        // Dropped onto another Tinta window: the tab moves there
        HWND under = WindowFromPoint(screen);
        HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
        wchar_t className[16] = {};
        if (root && root != hwnd &&
            GetClassNameW(root, className, _countof(className)) &&
            wcscmp(className, L"Tinta") == 0) {
            if (tabSendDrop(root, path, screen)) {
                SetForegroundWindow(root);
                dragCloseLocal(app, hwnd, index);
            }
        } else if (app.tabs.size() > 1) {
            // Once clear of the strip, releasing over our own document
            // also tears the tab off. Returning to the strip cancels the
            // detached state in tabDragMove before we reach this branch.
            wchar_t exePath[MAX_PATH];
            if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
                std::wstring args = L"--cascade --tabbed --pos " +
                    std::to_wstring(screen.x - (int)dpi(app, 120.0f)) + L" " +
                    std::to_wstring(screen.y - (int)dpi(app, 20.0f)) + L" \"" +
                    toWide(path) + L"\"";
                if ((INT_PTR)ShellExecuteW(nullptr, L"open", exePath, args.c_str(),
                                          nullptr, SW_SHOWNORMAL) > 32) {
                    dragCloseLocal(app, hwnd, index);
                }
            }
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void tabWindowDropMerge(App& app, HWND hwnd) {
    // A tabless window dropped so the cursor lands on another Tinta
    // window's tab strip donates its document there (Notepad-style);
    // multi-tab windows hand tabs over through per-tab drags instead
    if (app.tabs.size() != 1) return;
    if (app.editMode && app.editorDirty) return;
    const std::string& path = app.currentFile;
    if (path.empty()) return;

    POINT cursor;
    if (!GetCursorPos(&cursor)) return;
    HWND target = nullptr;
    // Top-level Tinta windows in z-order; the first one under the cursor
    // (other than us) is the drop candidate
    for (HWND w = FindWindowExW(nullptr, nullptr, L"Tinta", nullptr); w;
         w = FindWindowExW(nullptr, w, L"Tinta", nullptr)) {
        if (w == hwnd || !IsWindowVisible(w) || IsIconic(w)) continue;
        RECT r;
        if (!GetWindowRect(w, &r) || !PtInRect(&r, cursor)) continue;
        // Only the strip band counts as a drop zone; anywhere lower is
        // an ordinary overlapping window placement
        UINT targetDpi = GetDpiForWindow(w);
        if (cursor.y > r.top + MulDiv(46, (int)targetDpi, 96)) break;
        target = w;
        break;
    }
    if (!target) return;

    if (tabSendDrop(target, path, cursor)) {
        SetForegroundWindow(target);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

void tabDragCancel(App& app, HWND hwnd) {
    if (app.tabDragIndex < 0) return;
    app.tabDragIndex = -1;
    app.tabDragging = false;
    app.tabDragDetached = false;
    ghostDestroy(app);
    if (GetCapture() == hwnd) ReleaseCapture();
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool tabSwitcherMouseDown(App& app, HWND hwnd, int x, int y) {
    if (!app.showTabSwitcher) return false;
    for (const auto& [rect, index] : app.tabSwitcherHits) {
        if (x >= rect.left && x <= rect.right && y >= rect.top &&
            y <= rect.bottom) {
            app.showTabSwitcher = false;
            tabActivate(app, hwnd, index);
            return true;
        }
    }
    app.showTabSwitcher = false;
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}
