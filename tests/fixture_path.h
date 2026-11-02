#ifndef TINTA_TESTS_FIXTURE_PATH_H
#define TINTA_TESTS_FIXTURE_PATH_H

// Test fixtures live in the source tree, which may sit under a path with
// non-ASCII characters (this workspace does: "翠花工具包"). A narrow
// std::ifstream goes through the ANSI code page and cannot resolve such a
// path, so every fixture open goes through this helper instead: it walks
// the UTF-8 literal from the compiler definitions into a wide
// std::filesystem::path, which MinGW/libstdc++ opens with _wfopen.

#include <windows.h>

#include <filesystem>
#include <string>

namespace tinta_test {

inline std::filesystem::path fixturePath(const std::string& utf8) {
    if (utf8.empty()) return std::filesystem::path();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 1) return std::filesystem::path();
    std::wstring wide(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    return std::filesystem::path(wide);
}

inline std::filesystem::path fixturePath(const char* utf8) {
    return fixturePath(std::string(utf8 ? utf8 : ""));
}

// Joins a fixture directory with a file name before the wide conversion
inline std::filesystem::path fixtureIn(const char* utf8Dir,
                                       const std::string& name) {
    std::string joined = utf8Dir ? utf8Dir : "";
    if (!joined.empty() && joined.back() != '/' && joined.back() != '\\')
        joined += '/';
    joined += name;
    return fixturePath(joined);
}

}  // namespace tinta_test

using tinta_test::fixtureIn;
using tinta_test::fixturePath;

#endif  // TINTA_TESTS_FIXTURE_PATH_H
