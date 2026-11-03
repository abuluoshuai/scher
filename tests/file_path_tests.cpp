#include "d2d_init.h"
#include "file_utils.h"
#include "editor.h"
#include "input.h"
#include "overlays.h"
#include "tabs.h"
#include "utils.h"

#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

std::wstring clipboardText(HWND window) {
    std::wstring result;
    // Clipboard-history tools may briefly read a newly copied value.
    bool opened = false;
    for (int i = 0; i < 100 && !(opened = OpenClipboard(window)); ++i) Sleep(5);
    if (!opened) { check(false, "test clipboard opens for inspection"); return {}; }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (auto text = static_cast<const wchar_t*>(GlobalLock(data))) {
            result = text;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
}
void documentCopy(App& app) {
    openContextMenu(app, 20, 80);
    app.hoveredContextMenuItem = CTX_COPY_PATH;
    handleKeyDown(app, app.hwnd, VK_RETURN);
    check(!app.showContextMenu, "document Copy file path closes the menu");
}
void clickTabCopy(App& app) {
    // Copy file path follows the four close actions and their separator.
    int x = static_cast<int>(app.tabMenuX + 20);
    int y = static_cast<int>(app.tabMenuY + dpi(app, 6+4*30+9+15));
    check(tabMenuItemAt(app, static_cast<float>(x), static_cast<float>(y)) == 4,
          "tab Copy file path remains next to Reveal");
    check(tabMenuMouseDown(app, app.hwnd, x, y), "tab path action consumes its click");
    check(!app.showTabMenu, "tab Copy file path closes the menu");
}
void tabCopy(App& app, int index) {
    openTabMenu(app, index, 20, 40);
    clickTabCopy(app);
}
void renderStrip(App& app) {
    app.renderTarget->BeginDraw();
    renderTabStrip(app);
    check(SUCCEEDED(app.renderTarget->EndDraw()), "native title strip renders");
}
void rightClick(App& app, int x, int y) {
    closeTabMenu(app);
    POINT point{x, y};
    ClientToScreen(app.hwnd, &point);
    handleContextMenu(app, app.hwnd, MAKELPARAM(point.x, point.y));
}
// Explorer starts Tinta inside the document's folder. Once the process has
// parked its working directory, that folder can be renamed and deleted
// while Tinta runs, and the launch folder stays known for relative paths
// and the file browser (#253). Runs first: it owns the launch capture.
void launchFolderRelease() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring folder =
        std::wstring(temp) + L"tinta-launch-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring renamed = folder + L"-renamed";
    CreateDirectoryW(folder.c_str(), nullptr);
    wchar_t original[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, original);
    check(SetCurrentDirectoryW(folder.c_str()) != FALSE, "the run starts inside a document folder");
    check(!MoveFileW(folder.c_str(), renamed.c_str()),
          "a working directory holds its folder (the probe is sound)");
    parkWorkingDirectory();
    check(launchDirectory() == folder, "the launch folder is remembered after leaving it");
    check(MoveFileW(folder.c_str(), renamed.c_str()) != FALSE,
          "the parked process no longer holds the launch folder");
    MoveFileW(renamed.c_str(), folder.c_str());

    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Tinta launch folder test", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    app.width = 1050;
    app.height = 900;
    openContextMenu(app, 20, 80);
    app.hoveredContextMenuItem = CTX_BROWSE;
    handleKeyDown(app, app.hwnd, VK_RETURN);
    check(app.showFolderBrowser && app.folderBrowserPath == folder,
          "without a document the file browser opens on the launch folder");
    check(RemoveDirectoryW(folder.c_str()) != FALSE,
          "the launch folder can be deleted while the browser shows it");
    DestroyWindow(app.hwnd);
    SetCurrentDirectoryW(original);
}

void titleMenuCases(bool testClipboard) {
    // Render the actual caption into a hidden test window so these checks
    // catch missing hit regions, rather than opening a tab menu directly.
    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Tinta title test", WS_POPUP,
        100, 100, 1050, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd || !initD2D(app) || !createRenderTarget(app)) {
        check(false, "native caption test initializes");
        if (app.hwnd) DestroyWindow(app.hwnd);
        return;
    }
    app.height = 900;
    app.editRailAnim = 1;
    for (int theme : {0, 5}) {
        applyTheme(app, theme);
        for (float scale : {1.0f, 1.5f, 2.0f}) {
            app.contentScale = scale;
            updateTextFormats(app);
            for (int width : {650, 1050}) {
                app.width = width;
                for (bool edit : {false, true}) {
                    app.editMode = edit;
                    app.editorShowPreview = edit;
                    for (const std::string& path : {std::string(u8"C:\\Tinta tests\\\u017divot \u6f22\u5b57.md"),
                            "C:\\Tinta tests\\" + std::string(120, 'a') + ".md"}) {
                        app.currentFile = path;
                        app.tabs.resize(2);
                        app.tabs[1].path = "C:\\companion.md";
                        app.tabs[1].title = L"companion.md";
                        app.forceTabStrip = false;
                        renderStrip(app);
                        for (const auto& hit : app.tabHits) {
                            if (hit.index < 0) continue;
                            rightClick(app, (int)(hit.rect.left+5), (int)dpi(app, 20));
                            check(app.showTabMenu && app.tabMenuIndex == hit.index,
                                  "both tab headers open their own context menu");
                        }
                        app.tabs.resize(1);  // last companion closed
                        renderStrip(app);
                        int x = (int)dpi(app, edit ? 60.0f : 48.0f);
                        int y = (int)dpi(app, 20);
                        check(!tabStripVisible(app) && tabContextMenuIndexAt(app, (float)x, (float)y) == 0,
                              "the lone title has a tab context target after closing a companion");
                        for (const auto& hit : app.tabHits)
                            check(!(x >= hit.rect.left && x <= hit.rect.right &&
                                    y >= hit.rect.top && y <= hit.rect.bottom),
                                  "the lone title retains non-client window dragging");
                        rightClick(app, x, y);
                        check(app.showTabMenu && app.tabMenuIndex == 0,
                              "right-click routing opens the lone document's tab menu");
                        if (testClipboard && app.showTabMenu) {
                            clickTabCopy(app);
                            check(clipboardText(app.hwnd) == toWide(path),
                                  "single-title menu copies the full path");
                        }
                        check(tabContextMenuIndexAt(app, dpi(app, 20), (float)y) == -1,
                              "the app icon does not open the tab menu");
                        for (const auto& hit : app.tabHits)
                            if (hit.index < 0)
                                check(tabContextMenuIndexAt(app, (hit.rect.left+hit.rect.right)/2,
                                      (hit.rect.top+hit.rect.bottom)/2) == -1,
                                      "plus, pin and caption controls remain separate");
                        check(tabContextMenuIndexAt(app, (float)width/2, dpi(app, 50)) == -1,
                              "document content has no tab context target");
                        app.zenMode = true;
                        check(tabContextMenuIndexAt(app, (float)x, (float)y) == -1,
                              "zen mode cannot use stale caption hit regions");
                        app.zenMode = false;
                        app.forceTabStrip = true;
                        renderStrip(app);
                        rightClick(app, (int)dpi(app, 60), y);
                        check(app.showTabMenu && app.tabMenuIndex == 0,
                              "a detached single-tab window retains its tab menu");
                    }
                }
            }
        }
    }
    app.currentFile.clear();
    app.editMode = false;
    app.forceTabStrip = false;
    renderStrip(app);
    check(tabContextMenuIndexAt(app, dpi(app, 48), dpi(app, 20)) == -1,
          "the start page has no document title menu");
    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
    state.reset();
    CoUninitialize();
}
}

// Path helpers that the save dialog, the browser and the folder launcher
// share: they never touch the filesystem, so they are pure string work.
void pathHelpers() {
    check(getParentPath(L"") == L"", "an empty path has no parent");
    check(getParentPath(L"C:\\notes\\a.md") == L"C:\\notes",
          "a file's parent drops the file name");
    check(getParentPath(L"C:\\notes\\deep\\\\") == L"C:\\notes",
          "trailing separators are ignored");
    check(getParentPath(L"a.md") == L"a.md",
          "a bare file name is its own parent");
    check(getParentPath(L"C:\\a.md") == L"C:\\",
          "a file beside the drive root keeps its root");
    check(getParentPath(L"C:/mixed/path/a.md") == L"C:/mixed/path",
          "forward and backward separators both split");

    check(getDirectoryFromFile("C:\\notes\\a.md") == L"C:\\notes",
          "the directory of a UTF-8 path is returned wide");
    check(getDirectoryFromFile("a.md") == L".",
          "a bare name resolves to the working directory");
}

// The system open dialog hands back either one full path (single pick) or
// "directory<NUL>name1<NUL>name2<NUL><NUL>" (multi pick). Both shapes must
// become real paths. The separator and the NUL are built numerically so this
// file needs no escaped backslash anywhere.
void openDialogPaths() {
    const wchar_t kSep = static_cast<wchar_t>(92);
    const wchar_t kNul = static_cast<wchar_t>(0);

    auto join = [&](const std::vector<std::wstring>& parts) {
        std::wstring out;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) out += kSep;
            out += parts[i];
        }
        return out;
    };
    auto bufferOf = [&](const std::vector<std::wstring>& items) {
        std::vector<wchar_t> buf;
        for (const auto& item : items) {
            buf.insert(buf.end(), item.begin(), item.end());
            buf.push_back(kNul);
        }
        buf.push_back(kNul);
        return buf;
    };

    const std::wstring expected = join({L"C:", L"docs", L"note.md"});

    // Single selection: one complete path, nothing after it
    {
        auto buf = bufferOf({expected});
        auto one = parseMultiSelectPaths(buf.data());
        check(one.size() == 1, "a single pick yields one path");
        check(one.size() == 1 && one[0] == toUtf8(expected),
              "a single pick keeps its full path");
    }

    // Multiple selections: the directory, then bare names
    {
        const std::wstring dir = join({L"C:", L"docs"});
        auto buf = bufferOf({dir, L"a.md", L"b.md", L"c.md"});
        auto many = parseMultiSelectPaths(buf.data());
        check(many.size() == 3, "every selected name is returned");
        check(many.size() == 3 && many[0] == toUtf8(join({dir, L"a.md"})) &&
                  many[1] == toUtf8(join({dir, L"b.md"})) &&
                  many[2] == toUtf8(join({dir, L"c.md"})),
              "names are joined onto the directory");
    }

    // A directory that already ends with a separator must not double it
    {
        const std::wstring dir = join({L"C:", L"docs"}) + kSep;
        auto buf = bufferOf({dir, L"a.md"});
        auto joined = parseMultiSelectPaths(buf.data());
        check(joined.size() == 1 &&
                  joined[0] == toUtf8(join({L"C:", L"docs", L"a.md"})),
              "an existing separator is not doubled");
    }

    // Cancelled dialog / empty buffer: nothing to open
    check(parseMultiSelectPaths(nullptr).empty(), "a null buffer yields no paths");
    check(parseMultiSelectPaths(L"").empty(), "an empty buffer yields no paths");
}

// A multi-select batch opens one tab per file and lands on the first one.
// tabOpenPath's fourth argument is "activate", so passing true for every file
// would leave the LAST file shown and the first one buried.
void batchOpenTabs() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"tinta-batch-" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);

    std::vector<std::string> paths;
    for (const wchar_t* name : {L"one.md", L"two.md", L"three.md"}) {
        const std::wstring full = dir + L"\\" + name;
        FILE* f = _wfopen(full.c_str(), L"wb");
        if (f) { fputs("# doc\n", f); fclose(f); }
        paths.push_back(toUtf8(full));
    }
    if (paths.size() != 3) { check(false, "three fixture files"); return; }

    auto state = std::make_unique<App>();
    App& app = *state;
    app.hwnd = CreateWindowExW(0, L"STATIC", L"Tinta batch open test", WS_POPUP,
                               0, 0, 1050, 900, nullptr, nullptr, nullptr, nullptr);
    if (!app.hwnd) { check(false, "the batch open window"); return; }
    app.width = 1050;
    app.height = 900;

    // Start from the empty launcher: its untitled tab must not survive
    app.tabs.clear();
    app.tabs.push_back({});
    app.tabs[0].id = 1;
    app.activeTab = 0;
    app.currentFile.clear();
    openPathsInTabs(app, app.hwnd, paths);

    check(app.tabs.size() == 3, "each picked file gets a tab");
    check(!app.tabs.empty() && app.tabs[0].path == paths[0] &&
              app.tabs[1].path == paths[1] && app.tabs[2].path == paths[2],
          "the tabs carry the picked paths in order");
    check(app.currentFile == paths[0],
          "the first picked file is the one shown");
    check(app.tabs[app.activeTab].path == paths[0],
          "the active tab is the first picked file");

    // An open document keeps its tab: the batch appends, never replaces
    App second;
    second.hwnd = app.hwnd;
    second.width = 1050;
    second.height = 900;
    second.tabs.clear();
    second.tabs.push_back({});
    second.tabs[0].id = 1;
    second.tabs[0].path = "C:////existing.md";
    second.activeTab = 0;
    second.currentFile = "C:////existing.md";
    openPathsInTabs(second, second.hwnd, {paths[0], paths[1]});
    check(second.tabs.size() == 3, "the open document keeps its tab");
    check(second.tabs[0].path == "C:////existing.md",
          "the existing document stays in the first tab");
    check(second.currentFile == paths[0],
          "the first picked file is shown, not the existing one");

    DestroyWindow(app.hwnd);
}

int main(int argc, char** argv) {
    pathHelpers();
    // Default CTest runs are read-only. The explicit clipboard mode also
    // exercises the real menus/Win32 clipboard and overwrites its contents.
    bool testClipboard = argc == 2 && std::string(argv[1]) == "--clipboard-test";
    launchFolderRelease();
    openDialogPaths();
    batchOpenTabs();
    titleMenuCases(testClipboard);
    auto state = std::make_unique<App>();
    App& app = *state;
    if (testClipboard) {
        app.hwnd = CreateWindowExW(0, L"STATIC", L"Tinta clipboard test", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        if (!app.hwnd) return 2;
    }
    app.width = 1050;
    app.height = 900;
    app.tabs.resize(2);
    app.activeTab = 0;
    app.tabs[0].path = "C:\\old-name.md";  // live path is authoritative after Save As
    app.tabs[1].path = "C:\\Tinta tests\\inactive document.md";

    std::string longPath = "C:\\Tinta tests";
    for (int i = 0; i < 20; ++i) longPath += "\\long folder name";
    longPath += "\\document.md";
    std::wstring relativeExpected =
        (std::filesystem::current_path() / L"tests" / L"fixtures" / L"copy-file-path.md").wstring();
    for (const auto& item : {
            std::pair<std::string, std::wstring>{"tests\\..\\tests\\fixtures\\copy-file-path.md", relativeExpected},
            {"C:/Tinta tests/new name.md", L"C:\\Tinta tests\\new name.md"},
            {u8"C:\\Tinta tests\\\u017divot \u6f22\u5b57.md", L"C:\\Tinta tests\\\u017divot \u6f22\u5b57.md"},
            {"\\\\server\\share\\folder name\\document.md", L"\\\\server\\share\\folder name\\document.md"},
            {longPath, toWide(longPath)}, {"\\\\?\\"+longPath, L"\\\\?\\"+toWide(longPath)}}) {
        app.currentFile = item.first;
        check(absoluteFilePath(item.first) == item.second, "full paths resolve without quoting or truncation");
        if (!testClipboard) continue;
        documentCopy(app);
        check(clipboardText(app.hwnd) == item.second, "document action copies the full Unicode path");
        tabCopy(app, 0);
        check(clipboardText(app.hwnd) == item.second, "active-tab action uses the live path, not its saved snapshot");
    }
    if (!testClipboard) {
        std::cout << "Full file paths: " << failures << " failures (clipboard checks require --clipboard-test)\n";
        return failures ? 1 : 0;
    }
    app.editMode = true;
    app.editorText = L"# Unsaved edits\n\n$x^2$\n";
    app.editorDirty = true;
    tabCopy(app, 1);
    check(clipboardText(app.hwnd) == L"C:\\Tinta tests\\inactive document.md", "inactive-tab action copies the clicked tab");
    check(app.activeTab == 0 && app.editorDirty && app.editorText == L"# Unsaved edits\n\n$x^2$\n",
          "copying an inactive path preserves the active editor and unsaved changes");

    check(copyToClipboard(app.hwnd, L"sentinel"), "test clipboard writes Unicode text");
    check(!copyToClipboard(app.hwnd, L""), "empty clipboard requests do nothing");
    app.currentFile.clear();
    app.signalChips.clear();
    documentCopy(app);
    tabCopy(app, 0);
    check(clipboardText(app.hwnd) == L"sentinel" && app.signalChips.empty(),
          "untitled documents keep the clipboard and show no copy confirmation");
    app.tabs[1].path.clear();
    tabCopy(app, 1);
    check(clipboardText(app.hwnd) == L"sentinel", "untitled inactive tab cannot copy another document's path");
    app.editMode = false;
    app.startPageEmbeddedOpen = true;
    documentCopy(app);
    check(clipboardText(app.hwnd) == L"sentinel", "embedded help/start page has no file path");

    std::promise<bool> opened;
    std::promise<void> release;
    auto released = release.get_future();
    std::thread locker([&] {
        bool locked = OpenClipboard(nullptr) != 0;
        opened.set_value(locked);
        released.wait();
        if (locked) CloseClipboard();
    });
    bool locked = opened.get_future().get();
    check(locked, "another thread can hold the test clipboard");
    if (locked) {
        app.signalChips.clear();
        copyFilePath(app, app.hwnd, "C:\\document.md");
        check(app.signalChips.empty(), "a busy clipboard does not produce a false Copied confirmation");
    }
    release.set_value();
    locker.join();
    check(clipboardText(app.hwnd) == L"sentinel", "clipboard contention preserves its previous content");
    DestroyWindow(app.hwnd);
    app.hwnd = nullptr;
    std::cout << "Copy file path: " << failures << " failures\n";
    return failures ? 1 : 0;
}
