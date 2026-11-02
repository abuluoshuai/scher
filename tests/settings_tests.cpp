// settings.ini persistence.
//
// The configuration file is the only durable state the app keeps, and every
// preference has to survive a round trip while tolerating hand-edited and
// legacy rows. This suite drives saveSettings/loadSettings over every field,
// the [Positions] merge rules, the recent-file lists (local and ssh), the
// [Session] rows and the persist* helpers that the UI calls.
//
// The same folder holds languages.ini, so the runtime language registry and
// its per-key overrides are covered here too.
//
// It must run with a settings.ini beside the executable (the ctest wrapper
// provides one) so tintaConfigDir() picks the portable folder instead of the
// developer's real %APPDATA%.

#include "app.h"
#include "drafts.h"
#include "i18n.h"
#include "pandoc.h"
#include "settings.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::string read(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void write(const std::wstring& path, const std::string& text) {
    std::ofstream out(std::filesystem::path(path),
                      std::ios::binary | std::ios::trunc);
    out.write(text.data(), (std::streamsize)text.size());
}

bool closeTo(float a, float b) { return std::fabs(a - b) < 0.001f; }

// Every scalar field takes a non-default value, so a field that is written
// but never read back (or read back into the wrong member) shows up.
Settings sample() {
    Settings s;
    s.themeIndex = 3;
    s.zoomFactor = 1.75f;
    s.windowX = -1200;
    s.windowY = 44;
    s.windowWidth = 1440;
    s.windowHeight = 900;
    s.windowMaximized = true;
    s.hasAskedFileAssociation = true;
    s.editorShowPreview = false;
    s.editorWordWrap = true;
    s.editHintsShown = 7;
    s.followSystemTheme = true;
    s.lightThemeIndex = 2;
    s.darkThemeIndex = 6;
    s.folderSearchEnabled = false;
    s.tabSearchEnabled = false;
    s.browserFocusPath = true;
    s.openInTabs = false;
    s.sessionActive = 1;
    s.sessionTabs = {"C:\\notes\\one.md", "ssh://box/home/two.md"};
    s.keyProfile = "vim";
    s.readingWidthPct = 82;
    s.zenWidthPct = 55;
    s.headingRules = false;
    s.tocOnLeft = true;
    s.tabsOnLeft = true;
    s.tabsWidth = 312.5f;
    s.tocPinned = true;
    s.browserPinned = true;
    s.tocWidth = 333.0f;
    s.browserWidth = 355.0f;
    s.language = "zh-tw";
    s.editorAssists = false;
    s.showWordCount = false;
    s.closeKeepsDrafts = true;
    s.printMarginMm = 12.7f;
    s.pandocPath = "C:\\Tools\\pandoc.exe";
    s.frontmatter.shown = false;
    s.frontmatter.showOther = false;
    s.readingPositions = {{"C:\\notes\\one.md", 512.0f, 1.25f, 0.4f}};
    s.recentFiles = {{111ULL, "C:\\notes\\one.md"}, {222ULL, "C:\\notes\\two.md"}};
    s.remoteFiles = {{333ULL, "ssh://box/home/a.md"}};
    return s;
}

void roundTrip() {
    const std::wstring path = getSettingsPath();
    check(!path.empty(), "the settings path resolves");
    write(path, "");
    check(loadSettings().themeIndex == 5, "an empty file loads the defaults");
    check(std::filesystem::exists(path), "the settings file exists");

    const Settings saved = sample();
    saveSettings(saved);
    const Settings loaded = loadSettings();
    check(loaded.themeIndex == saved.themeIndex, "themeIndex round trips");
    check(closeTo(loaded.zoomFactor, saved.zoomFactor), "zoomFactor round trips");
    check(loaded.windowX == saved.windowX, "windowX round trips");
    check(loaded.windowY == saved.windowY, "windowY round trips");
    check(loaded.windowWidth == saved.windowWidth, "windowWidth round trips");
    check(loaded.windowHeight == saved.windowHeight, "windowHeight round trips");
    check(loaded.windowMaximized, "windowMaximized round trips");
    check(loaded.hasAskedFileAssociation, "hasAskedFileAssociation round trips");
    check(!loaded.editorShowPreview, "editorShowPreview round trips");
    check(loaded.editorWordWrap, "editorWordWrap round trips");
    check(loaded.editHintsShown == 7, "editHintsShown round trips");
    check(loaded.followSystemTheme, "followSystemTheme round trips");
    check(loaded.lightThemeIndex == 2 && loaded.darkThemeIndex == 6,
          "the light and dark theme choices round trip");
    check(!loaded.folderSearchEnabled && !loaded.tabSearchEnabled,
          "the search scopes round trip");
    check(loaded.browserFocusPath, "browserFocusPath round trips");
    check(!loaded.openInTabs, "openInTabs round trips");
    check(loaded.sessionActive == 1, "sessionActive round trips");
    check(loaded.sessionTabs.size() == 2 &&
              loaded.sessionTabs[1] == "ssh://box/home/two.md",
          "session tabs round trip in order");
    check(loaded.keyProfile == "vim", "keyProfile round trips");
    check(loaded.readingWidthPct == 82 && loaded.zenWidthPct == 55,
          "the reading widths round trip");
    check(!loaded.headingRules, "headingRules round trips");
    check(loaded.tocOnLeft && loaded.tabsOnLeft, "the panel sides round trip");
    check(closeTo(loaded.tabsWidth, 312.5f), "tabsWidth round trips");
    check(loaded.tocPinned && loaded.browserPinned, "the pinned panels round trip");
    check(closeTo(loaded.tocWidth, 333.0f) && closeTo(loaded.browserWidth, 355.0f),
          "the panel widths round trip");
    check(loaded.language == "zh-tw", "the language id round trips");
    check(!loaded.editorAssists && !loaded.showWordCount,
          "the editor switches round trip");
    check(loaded.closeKeepsDrafts, "closeKeepsDrafts round trips");
    check(closeTo(loaded.printMarginMm, 12.7f), "printMarginMm round trips");
    check(loaded.pandocPath == "C:\\Tools\\pandoc.exe", "pandocPath round trips");
    check(!loaded.frontmatter.shown && !loaded.frontmatter.showOther,
          "the front matter visibility round trips");
    check(loaded.readingPositions.size() == 1 &&
              closeTo(loaded.readingPositions[0].scrollY, 512.0f) &&
              closeTo(loaded.readingPositions[0].readProgress, 0.4f),
          "the reading position round trips");
    check(loaded.recentFiles.size() == 2 && loaded.recentFiles[0].when == 111ULL,
          "the recent file list round trips");
    check(loaded.remoteFiles.size() == 1 &&
              loaded.remoteFiles[0].path == "ssh://box/home/a.md",
          "the remote recent list round trips");
}

void frontmatterRules() {
    Settings s;
    fm::Rule shown{"status", true, false, true, fm::Format::Chips, 3,
                   fm::Kind::List};
    fm::Rule hidden{"draft", false, true, false, fm::Format::Plain, 0,
                    fm::Kind::Boolean};
    s.frontmatter.rules = {shown, hidden};
    saveSettings(s);
    const Settings loaded = loadSettings();
    check(loaded.frontmatter.rules.size() == 2,
          "front matter rules round trip through their encoded form");
    if (loaded.frontmatter.rules.size() == 2) {
        const fm::Rule& first = loaded.frontmatter.rules[0];
        check(first.key == "status" && first.show && !first.right &&
                  first.label && first.format == fm::Format::Chips &&
                  first.limit == 3 && first.hint == fm::Kind::List,
              "a show rule keeps every field");
        const fm::Rule& second = loaded.frontmatter.rules[1];
        check(second.key == "draft" && !second.show && second.right &&
                  !second.label && second.format == fm::Format::Plain &&
                  second.limit == 0 && second.hint == fm::Kind::Boolean,
              "a hide rule keeps every field");
    }
}

void languageIndex() {
    Settings s;
    s.language = "auto";
    saveSettings(s);
    check(loadSettings().language == "auto", "auto language persists");

    // A registry id maps to its index; a bogus id falls back to the system
    // language instead of failing to save.
    s.language = "de";
    saveSettings(s);
    check(loadSettings().language == "de", "a known language id persists");
    s.language = "not-a-language";
    saveSettings(s);
    check(loadSettings().language == "not-a-language",
          "an unknown language id is kept verbatim");
}

void readingPositions() {
    Settings s;
    s.language = "en";
    saveSettings(s);

    persistReadingPosition("C:\\notes\\a.md", 100.0f, 1.5f, 0.25f);
    Settings loaded = loadSettings();
    check(loaded.readingPositions.size() == 1, "a position is recorded");
    check(closeTo(findReadingPosition(loaded, "C:\\notes\\a.md"), 100.0f),
          "the scroll position is found again");
    check(closeTo(findReadingZoom(loaded, "C:\\notes\\a.md"), 1.5f),
          "the per-document zoom is found again");
    check(closeTo(findReadProgress(loaded, "C:\\notes\\a.md"), 0.25f),
          "the reading progress is found again");
    check(closeTo(findReadingPosition(loaded, "C:\\notes\\missing.md"), -1.0f),
          "an unknown document has no position (-1)");
    check(closeTo(findReadingZoom(loaded, "C:\\notes\\missing.md"), 0.0f),
          "an unknown document has no zoom");
    check(closeTo(findReadProgress(loaded, "C:\\notes\\missing.md"), 0.0f),
          "an unknown document has no progress");

    // Progress moves both ways: scrolling back up pulls the ring back
    persistReadingPosition("C:\\notes\\a.md", 20.0f, 0.0f, 0.10f);
    loaded = loadSettings();
    check(closeTo(findReadingPosition(loaded, "C:\\notes\\a.md"), 20.0f),
          "a later position replaces the earlier one");
    check(closeTo(findReadProgress(loaded, "C:\\notes\\a.md"), 0.10f),
          "progress can go backwards");
    check(closeTo(findReadingZoom(loaded, "C:\\notes\\a.md"), 1.5f),
          "zoom 0 keeps the stored zoom");

    // -1 means "unknown": the stored progress stays
    persistReadingPosition("C:\\notes\\a.md", 30.0f, 0.0f, -1.0f);
    loaded = loadSettings();
    check(closeTo(findReadProgress(loaded, "C:\\notes\\a.md"), 0.10f),
          "an unknown progress keeps the stored value");

    // Mark unread: assignment, not a high-water mark
    persistReadProgress("C:\\notes\\a.md", 0.0f);
    loaded = loadSettings();
    check(closeTo(findReadProgress(loaded, "C:\\notes\\a.md"), 0.0f),
          "marking a document unread clears its progress");

    // A second document becomes the newest entry
    persistReadingPosition("C:\\notes\\b.md", 5.0f);
    loaded = loadSettings();
    check(loaded.readingPositions.size() == 2 &&
              loaded.readingPositions[0].path == "C:\\notes\\b.md",
          "the newest position is first");
    persistReadingPosition("C:\\notes\\b.md", 6.0f);
    loaded = loadSettings();
    check(loaded.readingPositions.size() == 2,
          "re-persisting an entry does not duplicate it");

    // rememberReadingPosition merges into a Settings value in memory
    Settings memory;
    rememberReadingPosition(memory, "C:\\notes\\c.md", 42.0f, 2.0f, 0.5f);
    check(memory.readingPositions.size() == 1, "an in-memory position is added");
    rememberReadingPosition(memory, "C:\\notes\\c.md", 10.0f, 0.0f, 0.9f);
    check(memory.readingPositions.size() == 1, "an in-memory position is merged");
    check(closeTo(memory.readingPositions[0].scrollY, 10.0f) &&
              closeTo(memory.readingPositions[0].readProgress, 0.9f),
          "the merge keeps the newest values");
}

void legacyAndBrokenRows() {
    const std::wstring path = getSettingsPath();
    // Older builds wrote a scroll-only position row; zoom and progress must
    // default instead of swallowing the following rows.
    write(path,
          "[Settings]\nlanguage=en\n"
          "[Positions]\n"
          "pos=120|C:\\notes\\legacy.md\n"
          "pos=90|1.50|C:\\notes\\zoomed.md\n"
          "pos=garbage\n");
    Settings loaded = loadSettings();
    check(loaded.readingPositions.size() == 2,
          "legacy position rows with fewer fields still load");
    if (loaded.readingPositions.size() == 2) {
        check(closeTo(loaded.readingPositions[0].scrollY, 120.0f) &&
                  closeTo(loaded.readingPositions[0].zoom, 0.0f),
              "a scroll-only row leaves the zoom unset");
        check(closeTo(loaded.readingPositions[1].zoom, 1.50f),
              "a three-field row reads its zoom");
    }

    // The pre-i18n numeric language row maps back to a registry id
    write(path, "[Settings]\nlanguageIndex=2\n");
    check(loadSettings().language == "ja",
          "the legacy numeric language row maps to its id");
    write(path, "[Settings]\nlanguageIndex=9\n");
    check(loadSettings().language == "auto",
          "an out-of-range numeric language row falls back to auto");

    // Numbers that overflow int keep the default instead of throwing: these
    // rows are what a hand-edited file looks like after a typo.
    write(path,
          "[Settings]\n"
          "windowX=99999999999999999999\n"
          "windowWidth=99999999999999999999\n"
          "zoomFactor=99999999999999999999\n"
          "sessionActive=-5\n"
          "tabsWidth=abc\n"
          "tocWidth=abc\n"
          "editHintsShown=abc\n"
          "language=en\n");
    const Settings overflow = loadSettings();
    check(overflow.windowX == CW_USEDEFAULT,
          "an overflowing window coordinate keeps the default");
    check(overflow.windowWidth == 1024,
          "an overflowing window width keeps the default");
    check(closeTo(overflow.zoomFactor, 1.0f),
          "an overflowing zoom keeps the default");
    check(overflow.sessionActive == 0, "a negative session flag is ignored");

    // Unknown keys, blank lines and comments are ignored
    write(path,
          "; a hand-edited file\n"
          "[Settings]\n\n"
          "unknownKey=1\n"
          "themeIndex=not-a-number\n"
          "readingWidthPct=900\n"
          "printMarginMm=900\n"
          "zoomFactor=0\n"
          "language=en\n");
    loaded = loadSettings();
    check(loaded.themeIndex == 5, "a non-numeric value keeps the default");
    check(loaded.readingWidthPct == 100,
          "an out-of-range reading width falls back to the default");
    check(closeTo(loaded.printMarginMm, 19.05f),
          "an out-of-range print margin falls back to the default");
    check(loaded.zoomFactor > 0.0f && loaded.zoomFactor <= 4.0f,
          "a zero zoom is clamped into range");
}

void recentLists() {
    Settings s;
    s.language = "en";
    saveSettings(s);

    persistRecentFile("C:\\notes\\one.md");
    persistRecentFile("C:\\notes\\two.md");
    persistRecentFile("C:\\notes\\one.md");  // re-open moves it to the front
    Settings loaded = loadSettings();
    check(loaded.recentFiles.size() == 2, "re-opening a file does not duplicate it");
    check(!loaded.recentFiles.empty() &&
              loaded.recentFiles[0].path == "C:\\notes\\one.md",
          "a re-opened file moves to the front");
    check(!loaded.recentFiles.empty() && loaded.recentFiles[0].when > 0,
          "a recency stamp is recorded");

    // The list is capped; the oldest entries fall off the end
    for (int i = 0; i < 70; ++i) {
        persistRecentFile("C:\\notes\\bulk-" + std::to_string(i) + ".md");
    }
    loaded = loadSettings();
    check(loaded.recentFiles.size() <= 60, "the recent list is capped");
    check(!loaded.recentFiles.empty() &&
              loaded.recentFiles[0].path == "C:\\notes\\bulk-69.md",
          "the newest bulk entry is first");

    persistRemoteFile("");
    loaded = loadSettings();
    check(loaded.remoteFiles.empty(), "an empty remote uri is ignored");
    persistRemoteFile("ssh://box/home/a.md");
    persistRemoteFile("ssh://box/home/b.md");
    persistRemoteFile("SSH://BOX/home/a.md");  // case-insensitive duplicate
    loaded = loadSettings();
    check(loaded.remoteFiles.size() == 2,
          "the remote list dedupes case-insensitively");
    check(!loaded.remoteFiles.empty() &&
              loaded.remoteFiles[0].path == "SSH://BOX/home/a.md",
          "the re-opened remote file moves to the front");
    for (int i = 0; i < 70; ++i) {
        persistRemoteFile("ssh://box/home/bulk-" + std::to_string(i) + ".md");
    }
    loaded = loadSettings();
    check(loaded.remoteFiles.size() <= 60, "the remote list is capped");

    clearRecentFiles();
    loaded = loadSettings();
    check(loaded.recentFiles.empty(), "clearing empties the local list");
    check(loaded.remoteFiles.size() == loaded.remoteFiles.size(),
          "clearing leaves the remote list alone");
}

void persistHelpers() {
    Settings base;
    base.language = "en";
    saveSettings(base);

    App app;
    app.currentThemeIndex = 4;
    app.followSystemTheme = true;
    app.lightThemeIndex = 1;
    app.darkThemeIndex = 7;
    persistThemeChoice(app);
    Settings loaded = loadSettings();
    check(loaded.themeIndex == 4 && loaded.followSystemTheme &&
              loaded.lightThemeIndex == 1 && loaded.darkThemeIndex == 7,
          "the theme choice persists");

    app.editorAssists = false;
    persistEditorMode(app);
    check(!loadSettings().editorAssists, "the editor mode persists");

    app.showWordCount = false;
    persistWordCount(app);
    check(!loadSettings().showWordCount, "the word count switch persists");

    app.closeKeepsDrafts = true;
    persistCloseKeepsDrafts(app);
    check(loadSettings().closeKeepsDrafts, "the draft policy persists");

    app.printMarginMm = 25.4f;
    persistPrintMargin(app);
    check(closeTo(loadSettings().printMarginMm, 25.4f), "the print margin persists");

    app.folderSearchEnabled = false;
    app.tabSearchEnabled = false;
    persistSearchScopes(app);
    const Settings scopes = loadSettings();
    check(!scopes.folderSearchEnabled && !scopes.tabSearchEnabled,
          "the search scopes persist");

    app.openInTabs = false;
    persistOpenInTabs(app);
    check(!loadSettings().openInTabs, "the tab policy persists");
}

void keyOverrides() {
    // Pre-profile builds only had [Keys]; any customized binding now means
    // the custom profile, exactly as the user left it
    const std::wstring path = getSettingsPath();
    write(path,
          "[Settings]\nlanguage=en\n"
          "[Keys]\n"
          "search=x\n");
    const Settings migrated = loadSettings();
    check(migrated.keyProfile == "custom",
          "a customized binding migrates to the custom profile");
    check(migrated.keyOverrides.size() == 1 &&
              migrated.keyOverrides[0].first == "search",
          "the customized binding is kept");

    // A row that only repeats the default binding does not count as custom
    write(path,
          "[Settings]\nlanguage=en\nkeyProfile=vim\n"
          "[Keys]\n"
          "notAnAction=y\n");
    const Settings untouched = loadSettings();
    check(untouched.keyProfile == "vim",
          "an explicit profile survives unknown key rows");
    check(untouched.keyOverrides.empty(),
          "an unknown action name is dropped");
}

void languagesIni() {
    const int before = languageCount();
    const std::filesystem::path ini =
        std::filesystem::path(getSettingsPath()).parent_path() / L"languages.ini";
    {
        std::ofstream out(ini, std::ios::binary);
        out << "; translator file\n"
               "[xx-zz]\n"
               "name=Testish\n"
               "tab.new=Testish new tab\n"
               "\n"
               "[zz-empty]\n"
               "name=Empty\n"
               "\n"
               "[de]\n"
               "tab.new=Neuer Tab (korrigiert)\n"
               "bogus row without equals\n"
               "name=\n";
    }
    loadLanguageOverrides();
    check(languageCount() > before, "an ini language joins the registry");
    const int custom = languageIndexById("xx-zz");
    check(custom >= 0, "the ini language resolves by id");
    if (custom >= 0) {
        check(std::wstring(languageNameAt(custom)) == L"Testish",
              "the ini name becomes the language label");
        check(std::wstring(tr(custom, "tab.new")) == L"Testish new tab",
              "an ini translation overrides the compiled table");
        // Keys the ini does not translate fall back to English, and an
        // unknown key returns itself so breakage stays visible
        check(std::wstring(tr(custom, "toast.copied")) ==
                  std::wstring(tr(0, "toast.copied")),
              "an untranslated key falls back to English");
        check(std::wstring(tr(custom, "no.such.key")) == L"no.such.key",
              "an unknown key comes back as itself");
    }
    const int german = languageIndexById("de");
    if (german >= 0) {
        check(std::wstring(tr(german, "tab.new")) == L"Neuer Tab (korrigiert)",
              "an ini value corrects a built-in translation");
    }
    // A section without translations still joins the registry (so the file
    // can be filled in), but every key falls back to English
    const int empty = languageIndexById("zz-empty");
    if (empty >= 0) {
        check(std::wstring(tr(empty, "tab.new")) ==
                  std::wstring(tr(0, "tab.new")),
              "an untranslated section falls back to English");
    }

    std::error_code ignored;
    std::filesystem::remove(ini, ignored);
    loadLanguageOverrides();
}

void session() {
    Settings s;
    s.language = "en";
    s.sessionActive = 1;
    s.sessionTabs = {"C:\\a.md", "C:\\b.md", "C:\\c.md"};
    saveSettings(s);
    const Settings loaded = loadSettings();
    check(loaded.sessionActive == 1, "the session flag persists");
    check(loaded.sessionTabs.size() == 3 && loaded.sessionTabs[2] == "C:\\c.md",
          "every session tab persists in order");

    // The tab list is capped so a runaway session cannot bloat the file
    s.sessionTabs.clear();
    for (int i = 0; i < 70; ++i) {
        s.sessionTabs.push_back("C:////note-" + std::to_string(i) + ".md");
    }
    saveSettings(s);
    check(loadSettings().sessionTabs.size() <= 64,
          "the session tab list is capped");

    s.sessionActive = 0;
    s.sessionTabs.clear();
    saveSettings(s);
    const Settings cleared = loadSettings();
    check(cleared.sessionActive == 0 && cleared.sessionTabs.empty(),
          "an empty session persists as empty");
}

void pandocBridge() {
    // The flyout labels come from a fixed table; anything outside it is empty
    for (int fmt = 0; fmt < PANDOC_FORMAT_COUNT; ++fmt) {
        check(pandocFormatLabel(fmt) && *pandocFormatLabel(fmt),
              "format " + std::to_string(fmt) + " has a label");
        check(pandocFormatExt(fmt) && *pandocFormatExt(fmt),
              "format " + std::to_string(fmt) + " has an extension");
        check(pandocFormatExt(fmt)[0] == L'.',
              "format " + std::to_string(fmt) + " names a dotted extension");
    }
    check(std::wstring(pandocFormatLabel(-1)).empty(),
          "a negative format has no label");
    check(std::wstring(pandocFormatExt(PANDOC_FORMAT_COUNT)).empty(),
          "an out-of-range format has no extension");

    // The executable is resolved once: a user-chosen path that exists wins
    App app;
    const std::filesystem::path fake = std::filesystem::path(getSettingsPath())
                                           .parent_path() / L"fake-pandoc.exe";
    std::ofstream(fake, std::ios::binary) << "not a real pandoc";
    app.pandocUserPath = fake.wstring();
    pandocResolve(app);
    check(app.pandocExe == fake.wstring(),
          "an existing user-chosen pandoc is used");
    check(pandocAvailable(app), "the bridge reports pandoc as available");
    pandocResolve(app);  // the second call is a no-op
    check(app.pandocExe == fake.wstring(), "resolution happens once");

    // Choosing a path in settings persists it for the next launch
    pandocSetUserPath(app, fake.wstring());
    check(app.pandocExe == fake.wstring(),
          "the chosen path is resolved immediately");
    check(loadSettings().pandocPath ==
              std::filesystem::path(fake).string(),
          "the chosen path persists");

    // A path that does not exist is not used
    App missing;
    missing.pandocUserPath = fake.wstring() + L"-gone";
    pandocResolve(missing);
    check(missing.pandocExe != missing.pandocUserPath,
          "a missing user path is not used");

    std::error_code ignored;
    std::filesystem::remove(fake, ignored);
    Settings cleaned = loadSettings();
    cleaned.pandocPath.clear();
    saveSettings(cleaned);
}

void draftRecovery() {
    // Leftover autosaves from a crashed process are offered back; live ones
    // (this process, or another running tinta.exe) are not
    App app;
    const std::filesystem::path dir =
        std::filesystem::path(getSettingsPath()).parent_path() / L"drafts";
    std::filesystem::create_directories(dir);
    const auto stale = dir / L"draft-999999-1-0.md";
    const auto mine = dir / (L"draft-" + std::to_wstring(GetCurrentProcessId()) +
                             L"-1-0.md");
    std::ofstream(stale, std::ios::binary)
        << "<!-- tinta-draft:C:////notes////a.md -->\nunsaved body\n";
    std::ofstream(mine, std::ios::binary) << "live autosave\n";

    draftsScanForRecovery(app);
    check(!app.recoveredDrafts.empty(), "a stale draft is offered back");
    bool sawStale = false, sawLive = false;
    for (const auto& path : app.recoveredDrafts) {
        if (path.find(L"draft-999999-") != std::wstring::npos) sawStale = true;
        if (path.find(std::to_wstring(GetCurrentProcessId())) !=
            std::wstring::npos) {
            sawLive = true;
        }
    }
    check(sawStale, "the crashed process's draft is listed");
    check(!sawLive, "this process's own draft is not listed");

    draftsDiscardAll(app);
    check(app.recoveredDrafts.empty(), "discarding clears the offer");
    check(!std::filesystem::exists(stale), "discarding deletes the stale draft");
    check(std::filesystem::exists(mine), "a live draft is left alone");

    std::error_code ignored;
    std::filesystem::remove(mine, ignored);
}

void legacyConfigMigration() {
    // First run under Scher adopts a pre-rename %APPDATA%\Tinta folder:
    // the three INI files plus the drafts subfolder, and nothing else.
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / "scher-migration-suite";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    const fs::path legacy = root / "Tinta";
    const fs::path current = root / "Scher";
    fs::create_directories(legacy / "drafts");

    write(legacy.wstring() + L"\\settings.ini",
          "[Settings]\nthemeIndex=2\nzoomFactor=1.5\n");
    write(legacy.wstring() + L"\\themes.ini",
          "[theme]\nname=OldCustom\nbackground=111111\n");
    write(legacy.wstring() + L"\\languages.ini",
          "[xx]\nname=Custom\nsettings.title=Hello\n");
    write(legacy.wstring() + L"\\drafts\\draft-777-1-0.md",
          "<!-- tinta-draft:C:////old////note.md -->\nold body\n");
    // Not part of the configuration: must NOT be copied
    write(legacy.wstring() + L"\\leftover.tmp", "junk");

    migrateLegacyConfigDir(legacy.wstring(), current.wstring());
    check(read(current.wstring() + L"\\settings.ini")
              .find("themeIndex=2") != std::string::npos,
          "settings.ini is adopted from the legacy folder");
    check(read(current.wstring() + L"\\themes.ini")
              .find("OldCustom") != std::string::npos,
          "themes.ini is adopted");
    check(read(current.wstring() + L"\\languages.ini")
              .find("settings.title=Hello") != std::string::npos,
          "languages.ini is adopted");
    check(read(current.wstring() + L"\\drafts\\draft-777-1-0.md")
              .find("old body") != std::string::npos,
          "drafts are adopted");
    check(!fs::exists(current / "leftover.tmp"),
          "unrelated files are not copied");

    // Re-running is harmless: CopyFileW(..., TRUE) never overwrites
    write(legacy.wstring() + L"\\settings.ini",
          "[Settings]\nthemeIndex=9\n");
    migrateLegacyConfigDir(legacy.wstring(), current.wstring());
    check(read(current.wstring() + L"\\settings.ini")
              .find("themeIndex=2") != std::string::npos,
          "a second migration never overwrites what was adopted");

    // An existing Scher folder with real content blocks the adoption
    const fs::path busy = root / "ScherBusy";
    fs::create_directories(busy);
    write(busy.wstring() + L"\\settings.ini",
          "[Settings]\nthemeIndex=5\n");
    migrateLegacyConfigDir(legacy.wstring(), busy.wstring());
    check(read(busy.wstring() + L"\\settings.ini")
              .find("themeIndex=5") != std::string::npos,
          "an existing configuration wins over the legacy copy");

    fs::remove_all(root, ignored);
}

}  // namespace

int runSettingsTests() {
    namespace fs = std::filesystem;
    const std::wstring path = getSettingsPath();
    if (path.empty()) {
        std::cout << "Settings: skipped, no configuration directory\n";
        return 0;
    }
    // The wrapper writes settings.ini beside the executable so the app runs
    // in portable mode; without it these tests would rewrite the real
    // %APPDATA%\Scher\settings.ini.
    const fs::path expected = fs::current_path() / "settings.ini";
    check(fs::exists(expected),
          "the test runs in a portable configuration folder");
    if (!fs::exists(expected)) return 1;

    const std::string backup = read(path);
    roundTrip();
    frontmatterRules();
    languageIndex();
    readingPositions();
    legacyAndBrokenRows();
    recentLists();
    persistHelpers();
    keyOverrides();
    languagesIni();
    pandocBridge();
    draftRecovery();
    session();
    legacyConfigMigration();
    write(path, backup);

    std::cout << "Settings: " << checks << " checks, " << failures
              << " failures\n";
    return failures ? 1 : 0;
}
