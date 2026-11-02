// Unit tests for the SSH remote layer's pure parts: URI parsing/formatting,
// the ~/.ssh/config reader (through TINTA_SSH_CONFIG), the content cache and
// the guards that keep unsafe host aliases off the command line.

#include "fixture_path.h"
#include "remote.h"
#include "input.h"

#include <windowsx.h>
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void uriHelpers() {
    check(!remotePathIs(""), "empty path is not remote");
    check(!remotePathIs("C:/docs/a.md"), "a local path is not remote");
    check(!remotePathIs("ssh:/short"), "a malformed scheme is not remote");
    check(remotePathIs("ssh://host/a.md"), "the ssh scheme is recognised");

    const std::string uri = "ssh://box/home/sch/笔记 一.md";
    check(remotePathHost(uri) == "box", "host parsed");
    check(remotePathFile(uri) == "/home/sch/笔记 一.md", "path parsed");
    check(remotePathHost("ssh://box") == "box", "host without a path");
    check(remotePathFile("ssh://box") == "/", "bare host reads as root");

    check(remotePathMake("box", "/a/b.md") == "ssh://box/a/b.md",
          "make keeps an absolute path");
    check(remotePathMake("box", "rel.md") == "ssh://box/rel.md",
          "make adds the separator for a relative path");
    check(remotePathMake("box", "") == "ssh://box", "make without a path");

    check(remoteParentDir("/a/b/c.md") == "/a/b", "parent of a nested file");
    check(remoteParentDir("/x") == "/", "parent of a top-level entry");
    check(remoteParentDir("/") == "/", "root is its own parent");
    check(remoteParentDir("") == "/", "empty path reads as root");
    check(remoteParentDir("/a/b/") == "/a", "trailing separator ignored");
}

std::string writeConfig(const char* body) {
    char dir[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, dir)) return std::string();
    std::string path = std::string(dir) + "tinta-remote-test-config";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return std::string();
    fwrite(body, 1, strlen(body), f);
    fclose(f);
    return path;
}

void configParsing() {
    const char* body =
        "# a comment\n"
        "Host alpha beta\n"
        "    HostName alpha.example\n"
        "    User someone\n"
        "    IdentityFile C:/keys/id_ed25519\n"
        "\n"
        "Host wide-*\n"
        "    HostName ignored.example\n"
        "\n"
        "Host gamma\n"
        "    Port 2222\n"
        "Host alpha\n"
        "    HostName duplicate.example\n"
        "Host -dash\n"
        "    HostName unsafe.example\n";
    std::string configPath = writeConfig(body);
    if (configPath.empty()) {
        check(false, "temporary ssh config written");
        return;
    }
    _putenv_s("TINTA_SSH_CONFIG", configPath.c_str());

    auto hosts = remoteConfigHosts();
    check(hosts.size() == 3, "two aliases from one Host line, one duplicate dropped");
    if (hosts.size() == 3) {
        check(hosts[0].alias == "alpha" && hosts[1].alias == "beta" &&
                  hosts[2].alias == "gamma",
              "aliases keep file order, wildcards and options are skipped");
        check(hosts[0].hostName == "alpha.example",
              "HostName is kept for a single-alias block");
        check(hosts[1].hostName == "alpha.example",
              "a shared HostName applies to every alias of its block");
        check(hosts[2].hostName == "gamma", "missing HostName falls back to the alias");
    }

    // An alias that looks like an option must never reach the command line
    App app;
    remoteStartList(app, "-oProxyCommand=calc", "/");
    remoteStartRead(app, "bad host", "/x.md");
    check(remoteTakeFinished() == nullptr,
          "unsafe aliases are refused before any process starts");

    _putenv_s("TINTA_SSH_CONFIG", "");
    remove(configPath.c_str());
}

void contentCache() {
    const std::string uri = "ssh://box/cache.md";
    check(remoteCachedContent(uri) == nullptr, "cache starts empty");
    remoteStoreContent(uri, "# cached\n");
    const std::string* hit = remoteCachedContent(uri);
    check(hit && *hit == "# cached\n", "cache returns the stored text");
    remoteStoreContent(uri, "# replaced\n");
    check(*remoteCachedContent(uri) == "# replaced\n", "cache overwrites");

    // The cache is keyed by the whole URI, so two hosts never share a body
    check(remoteCachedContent("ssh://other/cache.md") == nullptr,
          "another host with the same path misses");
}

void settingsRoundTrip() {
    Settings settings;
    settings.recentFiles.clear();
    settings.remoteFiles.clear();
    rememberReadingPosition(settings, "ssh://box/notes.md", 120.0f, 1.5f, 0.25f);
    rememberReadingPosition(settings, "ssh://box/notes.md", 240.0f, 0.0f, 0.75f);
    check(settings.readingPositions.size() == 1,
          "the same document stays one entry");
    const auto& entry = settings.readingPositions.front();
    check(entry.scrollY == 240.0f, "the newest position wins");
    check(entry.zoom == 1.5f, "a known zoom is kept when the new call omits it");
    check(entry.readProgress == 0.75f, "progress follows the newest value");

    rememberReadingPosition(settings, "ssh://box/notes.md", 300.0f, 0.0f, -1.0f);
    check(settings.readingPositions.front().readProgress == 0.75f,
          "an unknown progress keeps the stored one");

    rememberReadingPosition(settings, "ssh://box/notes.md", 100.0f, 0.0f, 0.5f);
    check(settings.readingPositions.front().readProgress == 0.5f,
          "progress is allowed to move backwards");

    for (int i = 0; i < 60; i++)
        rememberReadingPosition(settings, "ssh://box/doc" + std::to_string(i) + ".md",
                                1.0f, 0.0f, 0.1f);
    check(settings.readingPositions.size() == 50, "the position list stays capped");
}

// The browser is a persistent overlay: the wheel scrolls its list and never
// dismisses it (a stray scroll used to close it outright).
void browserWheel() {
    App app;
    app.hwnd = nullptr;              // InvalidateRect(nullptr, ...) is harmless
    app.width = 1200;
    app.height = 820;
    app.contentScale = 1.0f;
    app.showRemoteBrowser = true;
    app.remoteHost = "wsl";
    app.remoteDir = "/home/sch";
    app.remoteRows.clear();
    for (int i = 0; i < 40; i++) {
        App::RemoteRow row;
        row.name = L"file" + std::to_wstring(i) + L".md";
        app.remoteRows.push_back(row);
        // The paint pass flattens the tree; the wheel measures that list
        App::RemoteTreeNode node;
        node.name = row.name;
        node.path = "/home/sch/file" + std::to_string(i) + ".md";
        app.remoteTree.push_back(node);
    }
    // The list rect is normally published by the render pass
    app.remoteListRect = D2D1::RectF(60.0f, 150.0f, 1140.0f, 600.0f);
    app.remoteScroll = 0.0f;

    const LPARAM inside = MAKELPARAM(600, 300);
    handleMouseWheel(app, app.hwnd, MAKEWPARAM(0, (WORD)-WHEEL_DELTA), inside);
    check(app.showRemoteBrowser, "wheeling inside the browser keeps it open");
    check(app.remoteScroll > 0.0f,
          "wheeling inside the browser scrolls its list");

    const float scrolled = app.remoteScroll;
    const LPARAM outside = MAKELPARAM(10, 780);
    handleMouseWheel(app, app.hwnd, MAKEWPARAM(0, (WORD)-WHEEL_DELTA), outside);
    check(app.showRemoteBrowser,
          "a wheel outside the browser leaves the overlay up");
    check(app.remoteScroll == scrolled,
          "a wheel outside the browser does not move its list");
}

// The dispatcher feeds the wheel handlers client coordinates even though
// WM_MOUSEWHEEL carries screen ones. Without that conversion every point
// tested against a panel lands outside it - which is what made the remote
// browser dismiss itself on scroll.
void wheelCoordinates() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ScherWheelPointTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"wheel point test",
                                WS_POPUP, -1200, 100, 800, 600, nullptr,
                                nullptr, wc.hInstance, nullptr);
    check(hwnd != nullptr, "a window for the wheel point test");
    if (hwnd) {
        POINT client{600, 300};
        POINT screen = client;
        ClientToScreen(hwnd, &screen);
        const LPARAM msg = MAKELPARAM(screen.x, screen.y);
        const POINT back = wheelClientPoint(hwnd, msg);
        check(back.x == client.x && back.y == client.y,
              "a screen wheel point converts back to client space");
        // The raw message point is the screen one: proof the conversion is
        // what fixes panel hit testing, not a coincidence.
        check(GET_X_LPARAM(msg) == screen.x,
              "the message carries screen coordinates before conversion");
        DestroyWindow(hwnd);
    }
}

// The refresh button re-lists the directory the window is showing. The trap
// it must avoid: deriving the path from the open document or from a stale
// selection instead of from the browse location.
void browserRefreshTarget() {
    App app;
    app.remoteHost = "wsl";
    app.remoteDir = "/home/sch/project";          // what the window shows
    app.currentFile = "ssh://wsl/home/sch/other/file.md";  // unrelated doc
    app.remotePicked = {"/home/sch/old/a.md"};    // picked earlier
    app.remoteRows.clear();
    app.remoteRows.push_back({L"stale.md", false});

    check(remoteRefreshDir(app) == "/home/sch/project",
          "refresh targets the browsed directory, not the open document");
    check(remoteRefreshDir(app) != "/home/sch/other",
          "refresh never walks up from the open document's path");

    // Guards: nothing to refresh without a host, and no second request while
    // one is still running.
    App idle = app;
    idle.remoteHost.clear();
    check(!remoteBrowserRefresh(idle), "no host means no refresh");
    App busy = app;
    busy.remoteInFlight = 1;
    check(!remoteBrowserRefresh(busy),
          "a refresh in flight blocks a second one");

    // The success path spawns a real ssh job, which would collide with the
    // process suite's job queue - the guards above cover the decision logic,
    // and drive_remote_wsl_real.py covers the rest against real sshd.
}

// Images inside a remote document resolve on the document's machine, never
// against this Windows drive.
void remoteImagePaths() {
    const std::string doc = "ssh://box/home/sch/notes/post.md";
    check(remoteImagePathFor(doc, "shot.png") == "/home/sch/notes/shot.png",
          "a relative image sits next to the document");
    check(remoteImagePathFor(doc, "./img/a.png") == "/home/sch/notes/./img/a.png",
          "a ./ reference keeps its form and stays relative");
    check(remoteImagePathFor(doc, "/var/img/a.png") == "/var/img/a.png",
          "an absolute reference is kept as written");
    check(remoteImagePathFor(doc, "~/pics/a.png") == "~/pics/a.png",
          "a home-relative reference stays home-relative for the shell");
    check(remoteImagePathFor("ssh://box/doc.md", "a.png") == "/a.png",
          "a document at the remote root resolves to the root");
}

// The browser is a tree: a folder expands in place and its listing nests
// under it, instead of replacing the view like a navigation would.
void browserTree() {
    App app;
    app.width = 1200;
    app.height = 820;
    app.remoteHost = "wsl";
    app.remoteDir = "/home/sch";
    app.remoteRows.clear();
    App::RemoteRow dir;
    dir.name = L"docs";
    dir.isDir = true;
    app.remoteRows.push_back(dir);
    App::RemoteRow file;
    file.name = L"note.md";
    app.remoteRows.push_back(file);
    app.remoteChildren["/home/sch/docs"] = {};
    App::RemoteRow inner;
    inner.name = L"inner.md";
    app.remoteChildren["/home/sch/docs"].push_back(inner);
    app.remoteChildReady["/home/sch/docs"] = true;

    app.remoteExpanded = {"/home/sch/docs"};
    app.remoteTree.clear();
    remoteBuildTreeForTest(app);
    check(app.remoteTree.size() == 3, "an open folder contributes its children");
    check(app.remoteTree[0].depth == 0 && app.remoteTree[0].isDir,
          "the folder sits at the root level");
    check(app.remoteTree.size() > 1, "the open folder contributes a child row");
    if (app.remoteTree.size() > 1) {
        check(app.remoteTree[1].depth == 1,
              "the child nests one level deeper");
        check(app.remoteTree[1].path == "/home/sch/docs/inner.md",
              "the child carries its full remote path");
    }
    check(app.remoteTree[2].depth == 0, "later root rows stay at the root");

    app.remoteExpanded.clear();
    app.remoteTree.clear();
    remoteBuildTreeForTest(app);
    check(app.remoteTree.size() == 2, "a collapsed folder hides its children");
}

}  // namespace

// The SSH exchange itself, against a test double: remote_process_tests.cpp
int runRemoteProcessSuite();

int main() {
    uriHelpers();
    configParsing();
    contentCache();
    settingsRoundTrip();
    browserWheel();
    wheelCoordinates();
    browserRefreshTarget();
    remoteImagePaths();
    browserTree();
    failures += runRemoteProcessSuite();
    if (failures) {
        std::cerr << "remote tests failed: " << failures << '\n';
        return 1;
    }
    std::cout << "remote tests passed\n";
    return 0;
}
