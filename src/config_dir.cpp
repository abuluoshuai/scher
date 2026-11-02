#include "settings.h"

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

// Portable mode (#147): a settings.ini beside the executable makes that
// folder the configuration home - settings, themes.ini, languages.ini,
// and drafts all live there, so the whole setup travels with the exe.
// Without it, everything stays in %APPDATA%\Scher. A pre-rename install
// keeps its files under %APPDATA%\Tinta; the first run under Scher copies
// them over once (see migrateLegacyConfigDir below). Resolved once per run.
// Lives in its own translation unit so the test targets that compile
// themes.cpp or i18n.cpp can link it without dragging in settings.cpp.
std::wstring tintaConfigDir() {
    static std::wstring cached;
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;

    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::wstring dir = exePath;
        size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir = dir.substr(0, slash);
            DWORD attrs =
                GetFileAttributesW((dir + L"\\settings.ini").c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES &&
                !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                cached = dir;
                return cached;
            }
        }
    }

    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0,
                                   appDataPath))) {
        std::wstring path = appDataPath;
        path += L"\\Scher";
        // Pre-rename installs keep their settings under %APPDATA%\Tinta.
        // When Scher's own folder has nothing real in it yet, adopt the
        // old folder's contents wholesale; Tinta stays untouched so a
        // rollback to the old binary keeps working.
        std::wstring legacy = appDataPath;
        legacy += L"\\Tinta";
        migrateLegacyConfigDir(legacy, path);
        CreateDirectoryW(path.c_str(), nullptr);  // Create if not exists
        cached = path;
    }
    return cached;
}

void migrateLegacyConfigDir(const std::wstring& legacyDir,
                            const std::wstring& currentDir) {
    // The current folder must not exist yet (or hold nothing but empty
    // subdirectories): if the user already ran Scher once, their files win
    // and the legacy copy is skipped.
    DWORD attrs = GetFileAttributesW(currentDir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return;  // a file? leave it
        WIN32_FIND_DATAW findData;
        HANDLE find = FindFirstFileW((currentDir + L"\\*").c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE) return;
        bool hasContent = false;
        do {
            if (wcscmp(findData.cFileName, L".") == 0 ||
                wcscmp(findData.cFileName, L"..") == 0)
                continue;
            hasContent = true;
            break;
        } while (FindNextFileW(find, &findData));
        FindClose(find);
        if (hasContent) return;
    }

    // Nothing to adopt when the legacy folder is absent or empty.
    attrs = GetFileAttributesW(legacyDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return;
    WIN32_FIND_DATAW findData;
    HANDLE find = FindFirstFileW((legacyDir + L"\\*").c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> entries;
    do {
        if (wcscmp(findData.cFileName, L".") == 0 ||
            wcscmp(findData.cFileName, L"..") == 0)
            continue;
        entries.push_back(findData.cFileName);
    } while (FindNextFileW(find, &findData));
    FindClose(find);
    if (entries.empty()) return;

    CreateDirectoryW(currentDir.c_str(), nullptr);
    // Only file kinds the config home actually holds: INI files at the top
    // level (settings/themes/languages) and the drafts subfolder. Anything
    // else (lock files, caches) is not part of the configuration.
    const wchar_t* kNames[] = {L"settings.ini", L"themes.ini", L"languages.ini"};
    for (const wchar_t* name : kNames) {
        std::wstring from = legacyDir + L"\\" + name;
        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        CopyFileW(from.c_str(), (currentDir + L"\\" + name).c_str(), TRUE);
    }
    std::wstring legacyDrafts = legacyDir + L"\\drafts";
    if (GetFileAttributesW(legacyDrafts.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring drafts = currentDir + L"\\drafts";
        CreateDirectoryW(drafts.c_str(), nullptr);
        HANDLE draftFind =
            FindFirstFileW((legacyDrafts + L"\\*").c_str(), &findData);
        if (draftFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") == 0 ||
                    wcscmp(findData.cFileName, L"..") == 0)
                    continue;
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                CopyFileW((legacyDrafts + L"\\" + findData.cFileName).c_str(),
                          (drafts + L"\\" + findData.cFileName).c_str(), TRUE);
            } while (FindNextFileW(draftFind, &findData));
            FindClose(draftFind);
        }
    }
}

std::wstring configFilePathForShell(const std::wstring& path) {
    if (path.empty()) return path;
    // Resolve the file, not just its parent: MSIX can merge redirected
    // files with existing, unvirtualized files in the same AppData folder.
    HANDLE file = CreateFileW(path.c_str(), 0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return path;

    DWORD size = GetFinalPathNameByHandleW(file, nullptr, 0, VOLUME_NAME_DOS);
    std::wstring resolved(size, L'\0');
    DWORD length = size ? GetFinalPathNameByHandleW(file, resolved.data(), size,
                                                  VOLUME_NAME_DOS) : 0;
    CloseHandle(file);
    if (!length || length >= size) return path;
    resolved.resize(length);

    // Shell file associations expect ordinary DOS/UNC paths, not the
    // extended-path prefix returned by GetFinalPathNameByHandleW.
    if (resolved.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        return L"\\\\" + resolved.substr(8);
    }
    if (resolved.rfind(L"\\\\?\\", 0) == 0 && resolved.size() > 6 &&
        resolved[5] == L':') {
        resolved.erase(0, 4);
    }
    return resolved;
}
