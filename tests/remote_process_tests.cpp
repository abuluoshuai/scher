// The SSH exchange itself, driven through a test double.
//
// remote.cpp talks to a real ssh client through three pipes, one script on
// stdin and UTF-8 on both directions. This suite points TINTA_SSH_EXE at a
// stand-in (tools/mingw-port/fake_ssh.cpp) that serves a local folder as the
// "server", so the process plumbing, the listing/read parsers, the error
// paths and the async job hand-off all run for real without a network.

#include "app.h"
#include "remote.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
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

void setEnv(const wchar_t* name, const std::wstring& value) {
    SetEnvironmentVariableW(name, value.c_str());
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), (std::streamsize)text.size());
}

// A message-only window is enough for the worker's completion post
HWND makeHiddenWindow() {
    return CreateWindowExW(0, L"STATIC", L"remote test", WS_POPUP, 0, 0, 10, 10,
                           nullptr, nullptr, nullptr, nullptr);
}

// Pumps messages until the worker posts a finished job (or gives up)
RemoteJob* waitForJob(App& app, int timeoutMs) {
    const DWORD started = GetTickCount();
    for (;;) {
        if (RemoteJob* job = remoteTakeFinished()) return job;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if ((int)(GetTickCount() - started) > timeoutMs) return nullptr;
        Sleep(10);
    }
}

std::wstring widen(const char* text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out((size_t)(size > 0 ? size - 1 : 0), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    return out;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         (int)text.size(), nullptr, 0, nullptr,
                                         nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(),
                        size, nullptr, nullptr);
    return out;
}

}  // namespace

int runRemoteProcessSuite() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "tinta-remote-suite";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    // The fake server sees this folder as "/"
    writeFile(root / L"note.md", "# 远程笔记\r\n\r\nBody with 中文.\r\n");
    writeFile(root / L"plain.txt", "plain\n");
    writeFile(root / L"测试目录" / L"inner.md", "inner\n");
    writeFile(root / L"space dir" / L"deep.md", "deep\n");
    // A quote in a name is the one character the POSIX shell quoting must
    // escape ("'\''"); it must still round trip.
    writeFile(root / L"it's here" / L"quoted.md", "quoted\n");

    setEnv(L"FAKE_SSH_ROOT", root.wstring());
    const std::wstring doubleExe = widen(REMOTE_FAKE_SSH_EXE);
    setEnv(L"TINTA_SSH_EXE", doubleExe);

    check(remoteSshExe() == doubleExe,
          "TINTA_SSH_EXE overrides the ssh client");

    std::string error;
    std::string resolved;

    // --- listing ---------------------------------------------------------
    std::vector<RemoteDirEntry> entries;
    check(remoteListBlocking("box", "", entries, error, &resolved),
          "the home directory lists (" + error + ")");
    check(resolved == "/", "the server's resolved home is reported back");
    bool sawNote = false, sawDir = false, sawSpaceDir = false;
    for (const auto& entry : entries) {
        if (entry.name == L"note.md" && !entry.isDir) sawNote = true;
        if (entry.name == L"测试目录" && entry.isDir) sawDir = true;
        if (entry.name == L"space dir" && entry.isDir) sawSpaceDir = true;
    }
    check(sawNote, "a file is listed with its UTF-8 name");
    check(sawDir, "a Chinese directory name survives the exchange");
    check(sawSpaceDir, "a directory name with a space survives");

    // A child directory: the resolved directory must be absolute, never
    // "/" + child (that bug opened /测试目录 from the home directory)
    entries.clear();
    error.clear();
    resolved.clear();
    check(remoteListBlocking("box", "/测试目录", entries, error, &resolved),
          "a Chinese subdirectory lists");
    check(resolved == "/测试目录",
          "the resolved child directory is absolute (" + resolved + ")");
    check(entries.size() == 1 && entries[0].name == L"inner.md" &&
              !entries[0].isDir,
          "the child directory shows its file");

    entries.clear();
    error.clear();
    check(!remoteListBlocking("box", "/missing", entries, error, nullptr),
          "a missing directory fails");
    check(!error.empty(), "a failed listing explains itself");

    // --- reading ---------------------------------------------------------
    std::string content;
    error.clear();
    check(remoteReadBlocking("box", "/note.md", content, error),
          "a file reads (" + error + ")");
    check(content.find("# 远程笔记") != std::string::npos,
          "the file content arrives as UTF-8");

    content.clear();
    error.clear();
    check(remoteReadBlocking("box", "/测试目录/inner.md", content, error),
          "a file inside a Chinese directory reads");
    check(content == "inner\n",
          "the content matches exactly (\"" + content + "\")");

    content.clear();
    error.clear();
    check(remoteReadBlocking("box", "/it's here/quoted.md", content, error),
          "a path with an apostrophe reads (" + error + ")");
    check(content == "quoted\n", "the apostrophe path returns its content");

    content.clear();
    error.clear();
    check(remoteReadBlocking("box", "~/note.md", content, error),
          "a tilde path expands to the home (" + error + ")");
    check(content == "# 远程笔记\r\n\r\nBody with 中文.\r\n",
          "the tilde read returns the home file's content");

    content.clear();
    error.clear();
    check(remoteReadBlocking("box", "~/测试目录/inner.md", content, error),
          "a tilde path with a subdirectory reads");
    check(content == "inner\n", "the tilde subpath content matches");

    entries.clear();
    error.clear();
    check(remoteListBlocking("box", "~", entries, error, &resolved),
          "listing ~ works (" + error + ")");
    check(resolved == "/", "listing ~ resolves to the home /");
    check(!entries.empty(), "the home listing is not empty");

    entries.clear();
    error.clear();
    resolved.clear();
    check(remoteListBlocking("box", "~/测试目录", entries, error, &resolved),
          "listing ~/dir works");
    check(resolved == "/测试目录",
          "the tilde listing resolves to the absolute subdirectory (" +
              resolved + ")");
    bool sawInner = false;
    for (const auto& e : entries)
        if (e.name == L"inner.md") sawInner = true;
    check(sawInner, "the tilde listing shows the directory contents");

    content.clear();
    error.clear();
    check(!remoteReadBlocking("box", "/nope.md", content, error),
          "a missing file fails to read");
    check(!error.empty(), "a failed read explains itself");

    // --- guards ----------------------------------------------------------
    entries.clear();
    error.clear();
    check(!remoteListBlocking("bo x", "", entries, error, nullptr),
          "an alias with a space is refused");
    check(error == "unsafe host alias", "the refusal names the reason");
    error.clear();
    check(!remoteReadBlocking("-oProxyCommand", "/note.md", content, error),
          "an alias that looks like an option is refused");

    // --- async jobs ------------------------------------------------------
    App app;
    app.hwnd = makeHiddenWindow();
    check(app.hwnd != nullptr, "the test window is created");
    if (app.hwnd) {
        remoteStartList(app, "box", "/测试目录");
        RemoteJob* job = waitForJob(app, 20000);
        check(job != nullptr, "the listing worker finishes");
        if (job) {
            check(job->kind == REMOTE_JOB_LIST, "the finished job is a listing");
            check(job->host == "box", "the job keeps its host");
            check(job->ok, "the background listing succeeds (" + job->error + ")");
            check(job->resolvedDir == "/测试目录", "the job reports the directory");
            check(!job->entries.empty(), "the job carries the entries");
            remoteDiscard(job);
        }

        remoteStartRead(app, "box", "/note.md");
        job = waitForJob(app, 20000);
        check(job != nullptr, "the read worker finishes");
        if (job) {
            check(job->kind == REMOTE_JOB_READ, "the finished job is a read");
            check(job->ok, "the background read succeeds");
            check(job->content.find("Body with 中文") != std::string::npos,
                  "the job carries the content");
            remoteDiscard(job);
        }

        // A failing job still completes, carrying its error
        remoteStartRead(app, "box", "/nope.md");
        job = waitForJob(app, 20000);
        check(job != nullptr, "a failing read still completes");
        if (job) {
            check(!job->ok, "the failing job reports failure");
            check(!job->error.empty(), "the failing job carries an error");
            remoteDiscard(job);
        }

        DestroyWindow(app.hwnd);
        app.hwnd = nullptr;
    }

    // --- a client that is not there --------------------------------------
    // The override is only used when the file exists; otherwise the inbox
    // OpenSSH takes over.
    const std::wstring missingExe = root.wstring() + L"\\no-such-ssh.exe";
    setEnv(L"TINTA_SSH_EXE", missingExe);
    check(remoteSshExe() != missingExe,
          "a missing client override is ignored");
    setEnv(L"TINTA_SSH_EXE", doubleExe);

    // --- no config home means no hosts -----------------------------------
    {
        wchar_t saved[MAX_PATH];
        const DWORD had = GetEnvironmentVariableW(L"USERPROFILE", saved, MAX_PATH);
        SetEnvironmentVariableW(L"USERPROFILE", nullptr);
        SetEnvironmentVariableW(L"TINTA_SSH_CONFIG", nullptr);
        const auto hosts = remoteConfigHosts();
        check(hosts.empty(), "a missing user profile yields no hosts");
        if (had > 0 && had < MAX_PATH) SetEnvironmentVariableW(L"USERPROFILE", saved);
    }

    // --- cache -----------------------------------------------------------
    remoteStoreContent("ssh://box/home/note.md", "cached body");
    std::string* cached = remoteCachedContent("ssh://box/home/note.md");
    check(cached && *cached == "cached body", "the content cache returns a hit");
    check(remoteCachedContent("ssh://box/other.md") == nullptr,
          "the cache misses on an unknown uri");

    fs::remove_all(root, ignored);
    if (failures != 0) {
        std::cerr << failures << " remote process test(s) failed\n";
    }
    return failures;
}
