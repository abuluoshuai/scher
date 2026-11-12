#ifndef TINTA_SETTINGS_H
#define TINTA_SETTINGS_H

#include "app.h"

std::wstring getSettingsPath();
void saveSettings(const Settings& settings);
Settings loadSettings();

// Reading position memory (#77): most-recent-first list capped in remember.
// The persist/lookup helpers load and save settings.ini themselves.
// progress = the document's reading progress (0..1) or -1 for "unknown",
// in which case the stored value is kept; known values merge as a
// high-water mark (never backwards).
void rememberReadingPosition(Settings& settings, const std::string& path,
                             float scrollY, float zoom = 0.0f,
                             float progress = -1.0f);
void persistReadingPosition(const std::string& path, float scrollY,
                            float zoom = 0.0f, float progress = -1.0f);
float findReadingPosition(const Settings& settings, const std::string& path);
// Per-document zoom, or 0 when the document has none stored
float findReadingZoom(const Settings& settings, const std::string& path);
// Stored reading progress (0..1), 0 when the document has none
float findReadProgress(const Settings& settings, const std::string& path);
// Set a document's stored progress directly (manual mark read/unread);
// unlike the position persist this assigns instead of max-merging, so
// "mark unread" can pull the ring back to empty.
void persistReadProgress(const std::string& path, float progress);

// Start page recents ([Recent] section): record an open, or clear the list
void persistRecentFile(const std::string& path);
// The same list for documents opened over ssh (remoteFiles, ssh:// URIs)
void persistRemoteFile(const std::string& uri);
void clearRecentFiles();

// Theme changes persist immediately so new windows spawn in the same look
void persistThemeChoice(const App& app);
// Editor mode pill choice (and the assists switch) persist immediately
void persistEditorMode(const App& app);
void persistWordCount(const App& app);
void persistCloseKeepsDrafts(const App& app);
// Print margins persist when picked in the preview (#257)
void persistPrintMargin(const App& app);
void persistSearchScopes(const App& app);
void persistOpenInTabs(const App& app);

// The configuration home: the exe's folder in portable mode (a
// settings.ini beside scher.exe opts in, #147), else %APPDATA%\Scher.
// On first run under Scher the contents of a pre-rename %APPDATA%\Tinta
// are adopted (see migrateLegacyConfigDir).
std::wstring tintaConfigDir();

// One-time adoption of a pre-rename config folder: copies settings.ini,
// themes.ini, languages.ini and the drafts subfolder from legacyDir into
// currentDir, but only when currentDir has no content of its own yet.
// Skipped entirely once Scher has written anything.
void migrateLegacyConfigDir(const std::wstring& legacyDir,
                            const std::wstring& currentDir);

// Resolve the actual file before handing it to an external editor, which
// does not share a Store/MSIX process's virtualized AppData view.
std::wstring configFilePathForShell(const std::wstring& path);

// Resolve the selected profile and custom [Keys] bindings.
void applyKeymap(App& app, const Settings& settings);
bool registerFileAssociation();
bool isRegisteredAsViewer();
void openDefaultAppsSettings();
void askAndRegisterFileAssociation(Settings& settings);

#endif // TINTA_SETTINGS_H
