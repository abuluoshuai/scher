// SSH remote reading: config discovery, process plumbing and the worker
// jobs behind remote.h. Everything here stays off the UI thread except the
// finished-job queue and the content cache.

#include "remote.h"

#include "utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kSshTimeoutMs = 25000;

std::wstring userProfileDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    return L"";
}

std::string trimAscii(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') b++;
    while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
    return s.substr(b, e - b);
}

// An alias is passed to ssh as an argument, so it must not look like an
// option and must not need Windows-level quoting
bool aliasSafe(const std::string& alias) {
    if (alias.empty() || alias[0] == '-') return false;
    for (unsigned char c : alias) {
        if (c <= ' ' || c == '"' || c == '\'' || c == '\\' || c == '&' ||
            c == '|' || c == '<' || c == '>' || c == '^' || c == '%') {
            return false;
        }
    }
    return true;
}

// POSIX single-quoting for paths inside the script we feed to `sh`
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

void readAll(HANDLE h, std::string& sink) {
    char buf[8192];
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        sink.append(buf, got);
    }
}

struct ProcResult {
    bool ok = false;
    bool timedOut = false;
    DWORD exitCode = (DWORD)-1;
    std::string out;
    std::string err;
};

// Runs `ssh <flags> <alias> sh`, feeding the shell script through stdin.
// Script text and file names therefore never touch the Windows command
// line, which is what keeps UTF-8 paths (Chinese names) intact.
bool runRemoteScript(const std::string& host, const std::string& script,
                     ProcResult& r) {
    if (!aliasSafe(host)) {
        r.err = "unsafe host alias";
        return false;
    }
    std::wstring exe = remoteSshExe();
    std::wstring wHost = toWide(host);
    std::wstring cmd = L"\"" + exe + L"\"";
    cmd += L" -o BatchMode=yes -o PreferredAuthentications=publickey";
    cmd += L" -o NumberOfPasswordPrompts=0 -o ConnectTimeout=10";
    cmd += L" -o ServerAliveInterval=5 -o LogLevel=ERROR";
    cmd += L" \"" + wHost + L"\" sh";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr,
           errR = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0) ||
        !CreatePipe(&outR, &outW, &sa, 0) ||
        !CreatePipe(&errR, &errW, &sa, 0)) {
        if (inR) CloseHandle(inR);
        if (inW) CloseHandle(inW);
        if (outR) CloseHandle(outR);
        if (outW) CloseHandle(outW);
        if (errR) CloseHandle(errR);
        if (errW) CloseHandle(errW);
        r.err = "pipe setup failed";
        return false;
    }
    // Only the child's ends stay inheritable
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;

    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!started) {
        CloseHandle(inW);
        CloseHandle(outR);
        CloseHandle(errR);
        r.err = "cannot start ssh";
        return false;
    }

    // Three concurrent streams: stdin writer, stdout and stderr readers.
    // Any of them can block, so none may run on the waiting thread.
    std::thread writer([&] {
        DWORD written = 0;
        if (!script.empty()) {
            WriteFile(inW, script.data(), (DWORD)script.size(), &written,
                      nullptr);
        }
        CloseHandle(inW);
    });
    std::thread outReader([&] {
        readAll(outR, r.out);
        CloseHandle(outR);
    });
    std::thread errReader([&] {
        readAll(errR, r.err);
        CloseHandle(errR);
    });

    DWORD wait = WaitForSingleObject(pi.hProcess, kSshTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        r.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    GetExitCodeProcess(pi.hProcess, &r.exitCode);

    writer.join();
    outReader.join();
    errReader.join();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    r.ok = !r.timedOut && r.exitCode == 0;
    if (!r.ok && r.err.empty()) {
        r.err = r.timedOut ? "ssh timed out" : "ssh failed";
    }
    return r.ok;
}

// --- finished jobs, handed from workers to the UI thread ---------------
std::mutex g_jobMutex;
std::vector<RemoteJob*> g_finished;

// --- remote document cache (UI thread only) ---------------------------
std::map<std::string, std::string> g_docCache;

}  // namespace

bool remotePathIs(const std::string& path) {
    return path.size() > 6 && _strnicmp(path.c_str(), "ssh://", 6) == 0;
}

std::string remotePathHost(const std::string& path) {
    if (!remotePathIs(path)) return std::string();
    size_t start = 6;
    size_t slash = path.find('/', start);
    if (slash == std::string::npos) return path.substr(start);
    return path.substr(start, slash - start);
}

std::string remotePathFile(const std::string& path) {
    if (!remotePathIs(path)) return std::string();
    size_t slash = path.find('/', 6);
    if (slash == std::string::npos) return "/";
    return path.substr(slash);
}

std::string remotePathMake(const std::string& host, const std::string& file) {
    std::string p = "ssh://" + host;
    if (file.empty()) return p;
    if (file[0] != '/') p += "/";
    p += file;
    return p;
}

std::string remoteParentDir(const std::string& dir) {
    if (dir.empty() || dir == "/") return "/";
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    size_t slash = d.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return d.substr(0, slash);
}

std::wstring remoteSshExe() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TINTA_SSH_EXE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH && GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES) {
        return buf;
    }
    wchar_t win[MAX_PATH];
    UINT m = GetWindowsDirectoryW(win, MAX_PATH);
    if (m > 0 && m < MAX_PATH) {
        std::wstring inbox = std::wstring(win) + L"\\System32\\OpenSSH\\ssh.exe";
        if (GetFileAttributesW(inbox.c_str()) != INVALID_FILE_ATTRIBUTES)
            return inbox;
    }
    return L"ssh.exe";
}

std::vector<RemoteHostInfo> remoteConfigHosts() {
    std::vector<RemoteHostInfo> hosts;
    std::wstring configPath;
    wchar_t override_[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TINTA_SSH_CONFIG", override_, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        configPath = override_;
    } else {
        std::wstring home = userProfileDir();
        if (home.empty()) return hosts;
        configPath = home + L"\\.ssh\\config";
    }
    std::ifstream file(configPath.c_str());
    if (!file) return hosts;

    std::vector<size_t> pending;  // entries of the block being read
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        // "Host a b c" / "HostName example.com" (case-insensitive keyword)
        size_t sp = trimmed.find_first_of(" \t=");
        std::string key = trimmed.substr(0, sp);
        std::string rest = sp == std::string::npos ? "" : trimmed.substr(sp + 1);
        rest = trimAscii(rest);
        for (char& c : key) c = (char)tolower((unsigned char)c);
        if (key == "host") {
            pending.clear();
            size_t i = 0;
            while (i < rest.size()) {
                while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) i++;
                size_t j = i;
                while (j < rest.size() && rest[j] != ' ' && rest[j] != '\t') j++;
                if (j <= i) break;
                std::string alias = rest.substr(i, j - i);
                i = j;
                // Wildcard patterns are matchers, not connectable aliases
                if (alias.find('*') != std::string::npos ||
                    alias.find('?') != std::string::npos ||
                    alias[0] == '!') {
                    continue;
                }
                if (!aliasSafe(alias)) continue;
                bool dup = false;
                for (const auto& h : hosts)
                    if (h.alias == alias) { dup = true; break; }
                if (dup) continue;
                hosts.push_back({alias, alias});
                pending.push_back(hosts.size() - 1);
            }
        } else if (key == "hostname" && !rest.empty()) {
            // A HostName belongs to the whole block, so every alias on the
            // matching Host line shows it (display detail only)
            for (size_t idx : pending) hosts[idx].hostName = rest;
            pending.clear();
        } else if (!pending.empty()) {
            pending.clear();  // any other directive ends the Host block
        }
    }
    return hosts;
}

namespace {

bool listDirSync(const std::string& host, const std::string& dir,
                 std::vector<RemoteDirEntry>& out, std::string& err,
                 std::string* resolvedDir) {
    // The script answers with the resolved working directory first: an
    // empty request means the login home, and every child path built from
    // "home" must be absolute or it lands at the filesystem root
    std::string script;
    if (dir.empty()) {
        script = "cd -- \"$HOME\" || exit 3\n";
    } else {
        script = "cd -- " + shellQuote(dir) + " || exit 3\n";
    }
    script += "printf '%s\\n' \"$PWD\"\nLC_ALL=C.UTF-8 ls -1Ap --\n";
    ProcResult r;
    if (!runRemoteScript(host, script, r)) {
        err = trimAscii(r.err);
        if (err.empty()) err = r.timedOut ? "timed out" : "ssh failed";
        return false;
    }
    out.clear();
    size_t pos = 0;
    bool firstLine = true;
    while (pos <= r.out.size()) {
        size_t nl = r.out.find('\n', pos);
        if (nl == std::string::npos) nl = r.out.size();
        std::string line = r.out.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = nl + 1;
        if (firstLine) {
            firstLine = false;
            if (resolvedDir && !line.empty()) *resolvedDir = line;
            if (nl == r.out.size()) break;
            continue;
        }
        if (line.empty() || line == "." || line == "..") continue;
        RemoteDirEntry e;
        if (line.back() == '/') {
            e.isDir = true;
            line.pop_back();
        }
        if (line.empty()) continue;
        e.name = toWide(line);
        out.push_back(std::move(e));
        if (nl == r.out.size()) break;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const RemoteDirEntry& a, const RemoteDirEntry& b) {
                         if (a.isDir != b.isDir) return a.isDir;
                         return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                     });
    return true;
}

bool readFileSync(const std::string& host, const std::string& file,
                  std::string& out, std::string& err) {
    if (file.empty()) {
        err = "no file";
        return false;
    }
    std::string script = "cat -- " + shellQuote(file) + "\n";
    ProcResult r;
    if (!runRemoteScript(host, script, r)) {
        err = trimAscii(r.err);
        if (err.empty()) err = r.timedOut ? "timed out" : "ssh failed";
        return false;
    }
    out = std::move(r.out);
    return true;
}

void startJob(App& app, RemoteJob* job) {
    HWND hwnd = app.hwnd;
    std::thread worker([job, hwnd] {
        if (job->kind == REMOTE_JOB_LIST) {
            job->ok = listDirSync(job->host, job->path, job->entries,
                                  job->error, &job->resolvedDir);
        } else {
            job->ok = readFileSync(job->host, job->path, job->content,
                                   job->error);
        }
        {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_finished.push_back(job);
        }
        if (hwnd) PostMessageW(hwnd, WM_APP_REMOTE_TASK, 0, 0);
    });
    worker.detach();
}

}  // namespace

bool remoteListBlocking(const std::string& host, const std::string& dir,
                        std::vector<RemoteDirEntry>& out, std::string& err,
                        std::string* resolvedDir) {
    return listDirSync(host, dir, out, err, resolvedDir);
}

bool remoteReadBlocking(const std::string& host, const std::string& file,
                        std::string& out, std::string& err) {
    return readFileSync(host, file, out, err);
}

void remoteStartList(App& app, const std::string& host, const std::string& dir) {
    if (host.empty() || !aliasSafe(host)) return;
    RemoteJob* job = new RemoteJob();
    job->kind = REMOTE_JOB_LIST;
    job->host = host;
    job->path = dir;
    startJob(app, job);
}

void remoteStartRead(App& app, const std::string& host, const std::string& file) {
    if (host.empty() || !aliasSafe(host)) return;
    RemoteJob* job = new RemoteJob();
    job->kind = REMOTE_JOB_READ;
    job->host = host;
    job->path = file;
    startJob(app, job);
}

RemoteJob* remoteTakeFinished() {
    std::lock_guard<std::mutex> lock(g_jobMutex);
    if (g_finished.empty()) return nullptr;
    RemoteJob* job = g_finished.front();
    g_finished.erase(g_finished.begin());
    return job;
}

void remoteDiscard(RemoteJob* job) { delete job; }

std::string* remoteCachedContent(const std::string& uri) {
    auto it = g_docCache.find(uri);
    return it == g_docCache.end() ? nullptr : &it->second;
}

void remoteStoreContent(const std::string& uri, std::string content) {
    g_docCache[uri] = std::move(content);
}

void remoteOpenRequest(App& app, const std::string& uri) {
    if (!remotePathIs(uri)) return;
    std::string host = remotePathHost(uri);
    std::string file = remotePathFile(uri);
    if (host.empty() || file.empty()) return;
    app.remoteInFlight++;
    app.remoteMessage.clear();
    remoteStartRead(app, host, file);
}
