#ifndef TINTA_REMOTE_H
#define TINTA_REMOTE_H

#include "app.h"

// SSH remote reading (read-only).
//
// A document is addressed as ssh://<alias>/absolute/remote/path, where
// <alias> is a Host entry from ~/.ssh/config. Fetching shells out to the
// system OpenSSH client, so keys, ports, users and jump hosts all come
// from that file -- and only public-key auth is ever attempted
// (BatchMode, PreferredAuthentications=publickey, no password prompts).
//
// Remote bytes travel over the ssh process's stdin/stdout, never through
// the Windows command line: the script is written to stdin and the paths
// inside it are UTF-8, which keeps non-ASCII file names intact end to end
// (a Chinese path in argv would have to survive the Windows ANSI code
// page before ssh even sees it).

bool remotePathIs(const std::string& path);            // starts with ssh://
std::string remotePathHost(const std::string& path);   // alias
std::string remotePathFile(const std::string& path);   // /abs/remote/path
std::string remotePathMake(const std::string& host, const std::string& file);
// Directory part of a remote file URI ("/a/b/c.md" -> "/a/b"), "/" at root
std::string remoteParentDir(const std::string& dir);

struct RemoteHostInfo {
    std::string alias;     // the Host token used on the command line
    std::string hostName;  // HostName when given, else the alias
};
// Host aliases from ~/.ssh/config, in file order, wildcards skipped
std::vector<RemoteHostInfo> remoteConfigHosts();

struct RemoteDirEntry {
    std::wstring name;
    bool isDir = false;
};

// Blocking variants (self-test, and callers already on a worker thread)
bool remoteListBlocking(const std::string& host, const std::string& dir,
                        std::vector<RemoteDirEntry>& out, std::string& err,
                        std::string* resolvedDir = nullptr);
bool remoteReadBlocking(const std::string& host, const std::string& file,
                        std::string& out, std::string& err);

// Executable used for connections: $TINTA_SSH_EXE when set (an override
// for non-standard installs and for tests), else the inbox OpenSSH, else
// "ssh.exe" from PATH
std::wstring remoteSshExe();

// --- asynchronous jobs -------------------------------------------------
// A detached worker runs one ssh exchange and posts WM_APP_REMOTE_TASK
// with the job pointer; the UI thread owns and frees it (mirrors the
// async image loader).
enum RemoteJobKind { REMOTE_JOB_LIST = 1, REMOTE_JOB_READ = 2 };

struct RemoteJob {
    RemoteJobKind kind = REMOTE_JOB_LIST;
    std::string host;
    std::string path;   // directory to list, or file to read
    bool ok = false;
    std::string error;
    // Absolute directory the listing actually came from ("$HOME" resolves
    // to it): child paths must be built from this, never from the request
    std::string resolvedDir;
    std::vector<RemoteDirEntry> entries;
    std::string content;
};

void remoteStartList(App& app, const std::string& host, const std::string& dir);
void remoteStartRead(App& app, const std::string& host, const std::string& file);
// UI thread: the next finished job, or nullptr. Caller frees it.
RemoteJob* remoteTakeFinished();
// Frees a finished job (for shutdown paths that drop it unhandled)
void remoteDiscard(RemoteJob* job);

// --- remote document plumbing (UI thread) ------------------------------
// Document content cache: a remote tab switched away from and back opens
// without another round trip. Dropped when the window closes.
std::string* remoteCachedContent(const std::string& uri);
void remoteStoreContent(const std::string& uri, std::string content);
// Query a remote file right now (background); the completion handler in
// remote_ui.cpp opens it as a tab, or reloads the view when it is the
// document already on screen.
void remoteOpenRequest(App& app, const std::string& uri);

// --- UI (remote_ui.cpp) ------------------------------------------------
bool openRemoteDocumentInViewer(App& app, const std::string& uri);
void openRemoteDocument(App& app, HWND hwnd, const std::string& uri);
// WM_APP_REMOTE_TASK: apply every finished listing/read
void handleRemoteTask(App& app);
// True while any remote overlay owns the window
bool remoteOverlayShown(const App& app);
void closeRemoteOverlays(App& app);

// Remote file browser (hosts, directories, multi-select)
void openRemoteBrowser(App& app);
void remoteBrowserNavigate(App& app, const std::string& dir);
void renderRemoteBrowser(App& app);
bool remoteBrowserMouseDown(App& app, HWND hwnd, float x, float y);

// The refresh button: re-lists the directory the browser is showing. Returns
// false when there is no host or a listing is already in flight.
bool remoteBrowserRefresh(App& app);
// The directory a refresh targets - always the browsed one, never derived
// from the open document or a previous selection.
std::string remoteRefreshDir(const App& app);
bool remoteBrowserWheel(App& app, float delta, float x, float y);
// Starts a read for every selected file and closes the browser
void remoteBrowserOpenSelection(App& app);

// Recent-documents caption button, its three-section menu and the
// two-column "More" dialog
D2D1_RECT_F historyButtonRect(const App& app);
void toggleHistoryMenu(App& app, HWND hwnd);
void renderHistoryMenu(App& app);
bool historyMenuMouseDown(App& app, HWND hwnd, float x, float y);
void renderHistoryDialog(App& app);
bool historyDialogMouseDown(App& app, HWND hwnd, float x, float y);
bool historyDialogWheel(App& app, float delta, float x, float y);

// Self-test (--remote-tests <report>): exercises the config parser, the
// ssh exchange, the URI helpers and the read-only document plumbing
// against whatever ssh executable TINTA_SSH_EXE points at. Writes a
// PASS/FAIL report (UTF-8) and returns the number of failures.
int runRemoteSelfTest(const std::wstring& reportPath);

#endif  // TINTA_REMOTE_H
