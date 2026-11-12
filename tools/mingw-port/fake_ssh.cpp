// Test double for the system ssh client, used by --remote-tests and the
// interaction scripts. It serves a local folder as the "remote" side:
//   FAKE_SSH_ROOT   (default %TEMP%\fake-remote)
// It understands exactly the two scripts remote.cpp sends:
//   cd -- '<dir>' ; LC_ALL=C.UTF-8 ls -1Ap --
//   cat -- '<file>'
// Everything stays in UTF-8, so Chinese paths behave like a real server.

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr,
                                  0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len,
                        nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

static std::wstring rootDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"FAKE_SSH_ROOT", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return L".";
    return std::wstring(tmp) + L"fake-remote";
}

static std::wstring trimEnds(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' ||
                     s[b] == L'\n'))
        b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' ||
                     s[e - 1] == L'\r' || s[e - 1] == L'\n'))
        e--;
    return s.substr(b, e - b);
}

// Single-quoted POSIX string -> raw text
static std::wstring unquote(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\'' && i + 3 < s.size() && s[i + 1] == L'\\' &&
            s[i + 2] == L'\'' && s[i + 3] == L'\'') {
            out += L'\'';
            i += 3;
        } else if (s[i] != L'\'') {
            out += s[i];
        }
    }
    return out;
}

// The remote shell would expand ~ to the login home. remote.cpp emits the
// tilde form as "$HOME"/'rest' (tilde kept outside the quotes), so the fake
// resolves it the same way a real shell would: "$HOME" is the fake's root,
// and the quoted remainder follows it.
static std::wstring expandHome(const std::wstring& arg) {
    const std::wstring home = L"\"$HOME\"";
    if (arg == home) return L"";
    if (arg.rfind(home + L"/", 0) == 0)
        return L"/" + unquote(arg.substr(home.size() + 1));
    return unquote(arg);
}

static int listDir(const std::wstring& rel) {
    std::wstring dir = rootDir();
    if (!rel.empty()) {
        std::wstring r = rel;
        if (r[0] == L'/') r = r.substr(1);
        if (!r.empty()) dir += L"\\" + r;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fputs("cd: no such directory\n", stderr);
        return 3;
    }
    // remote.cpp reads the resolved working directory from the first line
    {
        // Mirrors `cd -- <dir>; pwd`: an absolute request stays absolute,
        // a relative one is resolved against "/" (the fake's home).
        std::string relUtf8 = wideToUtf8(rel);
        std::string pwd = relUtf8.empty() ? std::string("/")
                         : relUtf8[0] == '/' ? relUtf8
                                             : "/" + relUtf8;
        pwd += "\n";
        fwrite(pwd.data(), 1, pwd.size(), stdout);
    }
    std::vector<std::pair<std::wstring, bool>> items;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        items.push_back({fd.cFileName,
                         (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(items.begin(), items.end(),
              [](const auto& a, const auto& b) {
                  return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
              });
    for (const auto& it : items) {
        std::string name = wideToUtf8(it.first);
        if (it.second) name += "/";
        name += "\n";
        fwrite(name.data(), 1, name.size(), stdout);
    }
    return 0;
}

static int catFile(const std::wstring& rel) {
    std::wstring path = rootDir();
    std::wstring r = rel;
    if (!r.empty() && r[0] == L'/') r = r.substr(1);
    if (!r.empty()) path += L"\\" + r;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        fputs("cat: cannot open file\n", stderr);
        return 1;
    }
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, got, stdout);
    fclose(f);
    return 0;
}


// Optional request log: FAKE_SSH_LOG=<file> appends every script the app
// sends, so a test can prove which remote files were actually fetched
// (documents, images) without a screenshot.
static void logScript(const std::wstring& script) {
    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"FAKE_SSH_LOG", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    FILE* f = _wfopen(path, L"ab");
    if (!f) return;
    std::string utf8 = wideToUtf8(script);
    fwrite(utf8.data(), 1, utf8.size(), f);
    fputc('\n', f);
    fclose(f);
}

int main(int argc, char**) {
    // Byte-exact like a real ssh: text-mode stdout would turn every LF into
    // CRLF and the file content would no longer match what is on "disk".
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    // The only argument remote.cpp adds after the flags is the host, then
    // "sh"; the script itself arrives on stdin.
    std::string script;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), stdin)) > 0) script.append(buf, got);
    std::wstring w = utf8ToWide(script);
    logScript(w);

    if (argc < 2) return 2;

    if (w.find(L"ls -1Ap") != std::wstring::npos) {
        size_t at = w.find(L"cd -- ");
        std::wstring rel;
        if (at != std::wstring::npos) {
            std::wstring rest = w.substr(at + 6);
            if (rest.find(L"\"$HOME\"") == 0) {
                // cd -- "$HOME"/'sub' expands tilde+subpath; a bare "$HOME"
                // is the home itself. The quoted part ends at its closing
                // quote - " || exit 3" follows it on the same line.
                std::wstring after = rest.substr(8);  // past "$HOME" and the /
                if (!after.empty() && after[0] == L'\'') {
                    size_t close = after.find(L'\'', 1);
                    if (close == std::wstring::npos) close = after.size();
                    rel = L"/" + after.substr(1, close - 1);
                } else {
                    rel.clear();
                }
            } else {
                size_t q1 = rest.find(L'\'');
                if (q1 != std::wstring::npos) {
                    size_t q2 = rest.find(L'\'', q1 + 1);
                    size_t nl = rest.find(L'\n', q1);
                    size_t end = q2 != std::wstring::npos &&
                                         (nl == std::wstring::npos || q2 < nl)
                                     ? q2 + 1
                                     : nl;
                    if (end != std::wstring::npos) {
                        std::wstring raw = rest.substr(q1, end - q1);
                        rel = trimEnds(unquote(raw));
                    }
                }
            }
        }
        return listDir(rel);
    }
    if (w.find(L"cat -- ") != std::wstring::npos) {
        size_t at = w.find(L"cat -- ");
        std::wstring rest = w.substr(at + 7);
        size_t nl = rest.find(L'\n');
        if (nl != std::wstring::npos) rest = rest.substr(0, nl);
        return catFile(expandHome(trimEnds(rest)));
    }
    fputs("fake-ssh: unsupported command\n", stderr);
    return 1;
}
