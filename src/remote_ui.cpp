// Remote (SSH) reading UI: the file browser, the recent-documents menu and
// its two-pane "More" dialog, plus the read-only document plumbing that
// puts fetched remote text on screen. All drawing is theme-native, all hit
// rects are rebuilt per paint and shared with the input handlers.

#include "remote.h"

#include "annotations.h"
#include "d2d_init.h"
#include "document.h"
#include "editor.h"
#include "frontmatter_ui.h"
#include "overlays.h"
#include "selection.h"
#include "i18n.h"
#include "settings.h"
#include "signals.h"
#include "tabs.h"
#include "utils.h"

#include <algorithm>
#include <cmath>

namespace {

// --- shared chrome ------------------------------------------------------

D2D1_COLOR_F panelSurface(const App& app) {
    return app.theme.isDark ? hexColor(0x1C1C1E, 0.99f)
                            : hexColor(0xFBFBFB, 0.99f);
}

D2D1_COLOR_F panelBorder(const App& app) {
    D2D1_COLOR_F c = app.theme.text;
    c.a = app.theme.isDark ? 0.16f : 0.14f;
    return c;
}

D2D1_COLOR_F dimText(const App& app, float alpha) {
    D2D1_COLOR_F c = app.theme.text;
    c.a = alpha;
    return c;
}

void softShadow(App& app, const D2D1_RECT_F& r, float radius) {
    float alpha = app.theme.isDark ? 0.42f : 0.20f;
    for (int ring = 4; ring >= 1; ring--) {
        float spread = dpi(app, (float)ring * 3.0f);
        D2D1_COLOR_F c = D2D1::ColorF(0.0f, 0.0f, 0.0f,
                                      alpha / (float)(ring * ring));
        app.brush->SetColor(c);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(r.left - spread, r.top - spread * 0.5f,
                            r.right + spread, r.bottom + spread),
                radius + spread * 0.5f, radius + spread * 0.5f),
            app.brush);
    }
}

void fillPanel(App& app, const D2D1_RECT_F& r, float radius) {
    softShadow(app, r, radius);
    app.brush->SetColor(panelSurface(app));
    app.renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(r, radius, radius), app.brush);
    app.brush->SetColor(panelBorder(app));
    app.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(r, radius, radius), app.brush, 1.0f);
}

// One line, end-trimmed with an ellipsis; used for every list row and
// path readout so long Chinese names never overrun their column
void drawTrimmed(App& app, const std::wstring& text, const D2D1_RECT_F& r,
                 IDWriteTextFormat* format, const D2D1_COLOR_F& color,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (!format || r.right <= r.left + 1.0f) return;
    IDWriteTextLayout* layout = nullptr;
    app.dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.size(),
                                        format, r.right - r.left,
                                        r.bottom - r.top, &layout);
    if (!layout) return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trim, nullptr);
    app.brush->SetColor(color);
    app.renderTarget->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout,
                                     app.brush);
    layout->Release();
}

void drawCheckGlyph(App& app, float cx, float cy, float s,
                    const D2D1_COLOR_F& color) {
    app.brush->SetColor(color);
    float w = dpi(app, 1.6f);
    app.renderTarget->DrawLine(D2D1::Point2F(cx - s * 0.55f, cy + s * 0.02f),
                               D2D1::Point2F(cx - s * 0.12f, cy + s * 0.45f),
                               app.brush, w);
    app.renderTarget->DrawLine(D2D1::Point2F(cx - s * 0.12f, cy + s * 0.45f),
                               D2D1::Point2F(cx + s * 0.58f, cy - s * 0.42f),
                               app.brush, w);
}

void drawFolderGlyph(App& app, float cx, float cy, float s,
                     const D2D1_COLOR_F& color) {
    app.brush->SetColor(color);
    D2D1_RECT_F body = D2D1::RectF(cx - s * 0.55f, cy - s * 0.30f,
                                   cx + s * 0.55f, cy + s * 0.50f);
    app.renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(body, dpi(app, 1.5f), dpi(app, 1.5f)), app.brush,
        dpi(app, 1.2f));
    app.renderTarget->DrawLine(D2D1::Point2F(cx - s * 0.55f, cy - s * 0.05f),
                               D2D1::Point2F(cx + s * 0.55f, cy - s * 0.05f),
                               app.brush, dpi(app, 1.0f));
}

void drawPageGlyph(App& app, float cx, float cy, float s,
                   const D2D1_COLOR_F& color) {
    app.brush->SetColor(color);
    float l = cx - s * 0.34f, r = cx + s * 0.34f;
    float t = cy - s * 0.48f, b = cy + s * 0.48f;
    float fold = s * 0.22f;
    app.renderTarget->DrawLine(D2D1::Point2F(l, t), D2D1::Point2F(r - fold, t),
                               app.brush, dpi(app, 1.2f));
    app.renderTarget->DrawLine(D2D1::Point2F(r - fold, t),
                               D2D1::Point2F(r, t + fold), app.brush,
                               dpi(app, 1.2f));
    app.renderTarget->DrawLine(D2D1::Point2F(r, t + fold), D2D1::Point2F(r, b),
                               app.brush, dpi(app, 1.2f));
    app.renderTarget->DrawLine(D2D1::Point2F(r, b), D2D1::Point2F(l, b),
                               app.brush, dpi(app, 1.2f));
    app.renderTarget->DrawLine(D2D1::Point2F(l, b), D2D1::Point2F(l, t),
                               app.brush, dpi(app, 1.2f));
}

bool pointIn(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

// A pill button; returns its rect
D2D1_RECT_F drawButton(App& app, const D2D1_RECT_F& r, const std::wstring& label,
                       bool primary, bool enabled) {
    float radius = dpi(app, 6.0f);
    D2D1_COLOR_F textColor;
    if (primary && enabled) {
        D2D1_COLOR_F accent = app.theme.accent;
        accent.a = 0.95f;
        app.brush->SetColor(accent);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(r, radius, radius), app.brush);
        textColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.97f);
    } else {
        D2D1_COLOR_F line = app.theme.text;
        line.a = enabled ? 0.32f : 0.14f;
        app.brush->SetColor(line);
        app.renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(r, radius, radius), app.brush, 1.0f);
        textColor = dimText(app, enabled ? 0.88f : 0.35f);
    }
    drawTrimmed(app, label,
                D2D1::RectF(r.left + dpi(app, 10.0f), r.top + dpi(app, 4.0f),
                            r.right - dpi(app, 10.0f), r.bottom),
                app.folderBrowserFormat, textColor, DWRITE_TEXT_ALIGNMENT_CENTER);
    return r;
}

// Rounded chip for a host alias
D2D1_RECT_F drawChip(App& app, const D2D1_RECT_F& r, const std::wstring& label,
                     bool selected) {
    float radius = r.bottom - r.top;
    if (selected) {
        D2D1_COLOR_F accent = app.theme.accent;
        accent.a = 0.18f;
        app.brush->SetColor(accent);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(r, radius * 0.5f, radius * 0.5f), app.brush);
        D2D1_COLOR_F edge = app.theme.accent;
        edge.a = 0.55f;
        app.brush->SetColor(edge);
        app.renderTarget->DrawRoundedRectangle(
            D2D1::RoundedRect(r, radius * 0.5f, radius * 0.5f), app.brush,
            1.0f);
    } else {
        D2D1_COLOR_F bg = app.theme.text;
        bg.a = 0.06f;
        app.brush->SetColor(bg);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(r, radius * 0.5f, radius * 0.5f), app.brush);
    }
    drawTrimmed(app, label,
                D2D1::RectF(r.left + dpi(app, 9.0f), r.top + dpi(app, 3.0f),
                            r.right - dpi(app, 9.0f), r.bottom),
                app.folderBrowserFormat,
                selected ? app.theme.accent : dimText(app, 0.8f));
    return r;
}

std::wstring joinHostPath(const std::string& host, const std::string& dir) {
    std::wstring s = toWide(host);
    s += L":";
    s += toWide(dir.empty() ? "/" : dir);
    return s;
}

// Basename for display; works for POSIX and Windows separators
std::wstring leafName(const std::wstring& path) {
    size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

std::wstring parentLabel(const std::wstring& path) {
    size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos || sep == 0) return path;
    return path.substr(0, sep);
}

}  // namespace

// --- remote document plumbing ------------------------------------------

namespace {

// Shared tail of a document switch: remember where the outgoing document
// was left so the next open resumes it
void persistOutgoingDoc(App& app) {
    if (app.currentFile.empty()) return;
    float progress = -1.0f;
    if (!app.tabs.empty() && app.activeTab >= 0 &&
        app.activeTab < (int)app.tabs.size() &&
        _stricmp(app.tabs[app.activeTab].path.c_str(),
                 app.currentFile.c_str()) == 0) {
        progress = app.tabs[app.activeTab].readProgress;
    }
    persistReadingPosition(app.currentFile, app.scrollY, app.zoomFactor,
                           progress);
}

// Reset per-document view state (mirrors openDocumentInViewer's tail)
void resetViewForDocument(App& app) {
    app.scrollY = 0;
    app.scrollX = 0;
    app.targetScrollY = 0;
    app.targetScrollX = 0;
    app.contentHeight = 0;
    app.verticalScrollbarVisible = false;
    app.scrollbarContentHeight = 0.0f;
    app.docText.clear();
    std::wstring().swap(app.docTextLower);
    app.searchMatches.clear();
    clearSelection(app);
    app.layoutDirty = true;
    app.remoteReadOnly = remotePathIs(app.currentFile);
}

}  // namespace

bool openRemoteDocumentInViewer(App& app, const std::string& uri) {
    persistOutgoingDoc(app);
    if (app.showLightbox) closeLightbox(app);
    app.fitBlocks.clear();
    app.startPageEmbeddedOpen = false;

    app.currentFile = uri;
    app.sourceText.clear();
    app.root = nullptr;

    std::string* content = remoteCachedContent(uri);
    if (!content) {
        // Nothing cached (session restore, or the read is still in flight):
        // show an empty page and fetch. The completion handler reloads this
        // view when the bytes arrive.
        auto empty = parseDocument(app.parser, std::string(), toWide(uri));
        if (empty.success) {
            app.root = empty.root;
            app.parseTimeUs = empty.parseTimeUs;
        }
        resetViewForDocument(app);
        updateWindowTitle(app);
        remoteOpenRequest(app, uri);
        return true;
    }

    auto result = parseDocument(app.parser, *content, toWide(uri));
    if (!result.success) return false;
    app.root = result.root;
    app.parseTimeUs = result.parseTimeUs;
    app.sourceText = *content;
    annotationsParseSource(app);
    if (app.annotEditorOpen) annotationEditorCancel(app);
    for (const auto& e : app.root->children)
        if (e->type == ElementType::Properties)
            observeFrontmatter(app, e->properties);

    persistRemoteFile(uri);
    resetViewForDocument(app);
    {
        Settings stored = loadSettings();
        app.pendingScrollRestore = findReadingPosition(stored, uri);
        float zoom = findReadingZoom(stored, uri);
        if (zoom > 0.0f && fabsf(zoom - app.zoomFactor) > 0.01f) {
            app.zoomFactor = zoom;
            app.appliedZoomFactor = zoom;
            updateTextFormats(app);
        }
    }
    updateWindowTitle(app);
    return true;
}

void openRemoteDocument(App& app, HWND hwnd, const std::string& uri) {
    if (!remotePathIs(uri)) return;
    // Already on screen or open in a tab: just show it
    for (size_t i = 0; i < app.tabs.size(); i++) {
        const std::string& p =
            (int)i == app.activeTab ? app.currentFile : app.tabs[i].path;
        if (_stricmp(p.c_str(), uri.c_str()) == 0) {
            tabActivate(app, hwnd, (int)i);
            return;
        }
    }
    if (!remoteCachedContent(uri)) {
        remoteOpenRequest(app, uri);  // the completion re-enters here
        return;
    }
    tabOpenPath(app, hwnd, uri);
}

void handleRemoteTask(App& app) {
    while (RemoteJob* job = remoteTakeFinished()) {
        if (app.remoteInFlight > 0) app.remoteInFlight--;
        if (job->kind == REMOTE_JOB_LIST) {
            if (job->ok) {
                app.remoteDir = job->resolvedDir.empty() ? job->path
                                                         : job->resolvedDir;
                app.remoteRows.clear();
                app.remoteRows.reserve(job->entries.size());
                for (auto& e : job->entries)
                    app.remoteRows.push_back({e.name, e.isDir});
                app.remotePicked.clear();
                app.remoteScroll = 0.0f;
                app.remoteMessage.clear();
            } else {
                app.remoteMessage = toWide(job->error);
            }
        } else if (job->ok) {
            std::string uri = remotePathMake(job->host, job->path);
            std::string content = std::move(job->content);
            remoteStoreContent(uri, content);
            persistRemoteFile(uri);
            // Reload the view when this file is the one waiting on screen
            if (_stricmp(app.currentFile.c_str(), uri.c_str()) == 0) {
                openRemoteDocumentInViewer(app, uri);
            } else {
                openRemoteDocument(app, app.hwnd, uri);
            }
            app.remoteMessage.clear();
            signalPushKey(app, SIG_SUCCESS, SIGI_CHECK, "remote.opened");
        } else {
            app.remoteMessage = toWide(job->error);
            signalPushKey(app, SIG_ERROR, SIGI_WARNING, "remote.failed");
        }
        delete job;
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

// --- remote file browser ------------------------------------------------

namespace {

constexpr int kRBBrowseRow = 1000;
constexpr int kRBHostBase = 1;
constexpr int kRBUp = 400;
constexpr int kRBHome = 401;
constexpr int kRBOpen = 5000;
constexpr int kRBClose = 5001;

float rbRowHeight(const App& app) { return dpi(app, 26.0f); }

D2D1_RECT_F remoteBrowserPanel(const App& app) {
    float w = std::min(dpi(app, 900.0f), (float)app.width - dpi(app, 60.0f));
    float h = std::min(dpi(app, 640.0f), (float)app.height - dpi(app, 90.0f));
    w = std::max(w, dpi(app, 420.0f));
    h = std::max(h, dpi(app, 320.0f));
    float cx = (float)app.width * 0.5f;
    float cy = (float)app.height * 0.5f;
    return D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f,
                       cy + h * 0.5f);
}

}  // namespace

void openRemoteBrowser(App& app) {
    app.showRemoteBrowser = true;
    app.showHistoryMenu = false;
    app.showHistoryDialog = false;
    app.remoteMessage.clear();
    if (app.remoteHost.empty()) {
        auto hosts = remoteConfigHosts();
        if (!hosts.empty()) app.remoteHost = hosts[0].alias;
    }
    if (!app.remoteHost.empty() && app.remoteRows.empty() && app.remoteInFlight == 0) {
        remoteStartList(app, app.remoteHost, app.remoteDir);
        app.remoteInFlight++;
    }
}

void remoteBrowserNavigate(App& app, const std::string& dir) {
    if (app.remoteHost.empty()) return;
    app.remoteMessage.clear();
    remoteStartList(app, app.remoteHost, dir);
    app.remoteInFlight++;
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void renderRemoteBrowser(App& app) {
    app.remoteHits.clear();
    D2D1_RECT_F panel = remoteBrowserPanel(app);
    float radius = dpi(app, 10.0f);
    fillPanel(app, panel, radius);

    float pad = dpi(app, 16.0f);
    float y = panel.top + dpi(app, 13.0f);

    // Title + close
    drawTrimmed(app, tr(app, "remote.title"),
                D2D1::RectF(panel.left + pad, y, panel.right - dpi(app, 46.0f),
                            y + dpi(app, 22.0f)),
                app.folderBrowserFormat, dimText(app, 0.95f));
    {
        D2D1_RECT_F x = D2D1::RectF(panel.right - dpi(app, 34.0f), y - dpi(app, 2.0f),
                                    panel.right - dpi(app, 12.0f), y + dpi(app, 20.0f));
        D2D1_COLOR_F c = dimText(app, 0.55f);
        app.brush->SetColor(c);
        float s = dpi(app, 4.0f);
        float cx = (x.left + x.right) * 0.5f, cy = (x.top + x.bottom) * 0.5f;
        app.renderTarget->DrawLine(D2D1::Point2F(cx - s, cy - s),
                                   D2D1::Point2F(cx + s, cy + s), app.brush,
                                   dpi(app, 1.3f));
        app.renderTarget->DrawLine(D2D1::Point2F(cx - s, cy + s),
                                   D2D1::Point2F(cx + s, cy - s), app.brush,
                                   dpi(app, 1.3f));
        app.remoteHits.push_back({x, kRBClose});
    }
    y += dpi(app, 26.0f);

    // Host chips (from ~/.ssh/config)
    auto hosts = remoteConfigHosts();
    if (hosts.empty()) {
        drawTrimmed(app, tr(app, "remote.nohosts"),
                    D2D1::RectF(panel.left + pad, y, panel.right - pad,
                                y + dpi(app, 20.0f)),
                    app.folderBrowserFormat, dimText(app, 0.55f));
        y += dpi(app, 24.0f);
    } else {
        float chipH = dpi(app, 22.0f);
        float chipX = panel.left + pad;
        for (size_t i = 0; i < hosts.size(); i++) {
            std::wstring label = toWide(hosts[i].alias);
            float w = measureText(app, label, app.folderBrowserFormat) +
                      dpi(app, 20.0f);
            if (chipX + w > panel.right - pad) break;  // one row of hosts
            D2D1_RECT_F chip = D2D1::RectF(chipX, y, chipX + w, y + chipH);
            bool selected = hosts[i].alias == app.remoteHost;
            drawChip(app, chip, label, selected);
            app.remoteHits.push_back({chip, kRBHostBase + (int)i});
            chipX = chip.right + dpi(app, 6.0f);
        }
        y += chipH + dpi(app, 10.0f);
    }

    // Path row: up / home + the current path
    {
        float btnH = dpi(app, 22.0f);
        float upW = measureText(app, tr(app, "remote.up"),
                                app.folderBrowserFormat) + dpi(app, 20.0f);
        float homeW = measureText(app, tr(app, "remote.home"),
                                  app.folderBrowserFormat) + dpi(app, 20.0f);
        D2D1_RECT_F up = D2D1::RectF(panel.left + pad, y, panel.left + pad + upW,
                                     y + btnH);
        D2D1_RECT_F home = D2D1::RectF(up.right + dpi(app, 6.0f), y,
                                       up.right + dpi(app, 6.0f) + homeW,
                                       y + btnH);
        drawButton(app, up, tr(app, "remote.up"), false, !app.remoteHost.empty());
        drawButton(app, home, tr(app, "remote.home"), false, !app.remoteHost.empty());
        if (!app.remoteHost.empty()) {
            app.remoteHits.push_back({up, kRBUp});
            app.remoteHits.push_back({home, kRBHome});
        }
        std::wstring pathText = joinHostPath(app.remoteHost,
                                             app.remoteDir);
        drawTrimmed(app, pathText,
                    D2D1::RectF(home.right + dpi(app, 10.0f), y + dpi(app, 2.0f),
                                panel.right - pad, y + btnH),
                    app.folderBrowserFormat, dimText(app, 0.62f));
        y += btnH + dpi(app, 8.0f);
    }

    // Divider
    {
        D2D1_COLOR_F line = panelBorder(app);
        line.a *= 0.7f;
        app.brush->SetColor(line);
        app.renderTarget->DrawLine(
            D2D1::Point2F(panel.left + pad, y),
            D2D1::Point2F(panel.right - pad, y), app.brush, 1.0f);
        y += dpi(app, 6.0f);
    }

    // List
    float footerH = dpi(app, 40.0f);
    D2D1_RECT_F listRect = D2D1::RectF(panel.left + pad, y,
                                       panel.right - pad,
                                       panel.bottom - footerH - dpi(app, 6.0f));
    app.remoteListRect = listRect;
    float rowH = rbRowHeight(app);
    float contentH = (float)app.remoteRows.size() * rowH;
    float viewH = listRect.bottom - listRect.top;
    float maxScroll = std::max(0.0f, contentH - viewH);
    app.remoteScroll = std::clamp(app.remoteScroll, 0.0f, maxScroll);

    app.renderTarget->PushAxisAlignedClip(listRect,
                                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (app.remoteRows.empty()) {
        drawTrimmed(app, app.remoteInFlight > 0 ? tr(app, "remote.loading")
                                                : tr(app, "remote.empty"),
                    D2D1::RectF(listRect.left + dpi(app, 4.0f),
                                listRect.top + dpi(app, 8.0f),
                                listRect.right, listRect.top + rowH),
                    app.folderBrowserFormat, dimText(app, 0.45f));
    }
    int first = std::max(0, (int)(app.remoteScroll / rowH));
    for (size_t i = (size_t)first; i < app.remoteRows.size(); i++) {
        float ry = listRect.top + rowH * (float)i - app.remoteScroll;
        if (ry > listRect.bottom) break;
        D2D1_RECT_F row = D2D1::RectF(listRect.left, ry, listRect.right,
                                      ry + rowH);
        if (row.bottom < listRect.top) continue;
        const App::RemoteRow& item = app.remoteRows[i];
        bool picked = !item.isDir &&
                      std::find(app.remotePicked.begin(), app.remotePicked.end(),
                                (int)i) != app.remotePicked.end();
        bool hover = pointIn(row, (float)app.mouseX, (float)app.mouseY);
        if (hover || picked) {
            D2D1_COLOR_F bg = app.theme.accent;
            bg.a = picked ? 0.14f : 0.08f;
            app.brush->SetColor(bg);
            app.renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(row, dpi(app, 5.0f), dpi(app, 5.0f)),
                app.brush);
        }
        float gx = row.left + dpi(app, 12.0f);
        float cy = (row.top + row.bottom) * 0.5f;
        if (item.isDir)
            drawFolderGlyph(app, gx, cy, dpi(app, 13.0f), dimText(app, 0.7f));
        else
            drawPageGlyph(app, gx, cy, dpi(app, 13.0f), dimText(app, 0.7f));
        float textLeft = gx + dpi(app, 16.0f);
        drawTrimmed(app, item.name,
                    D2D1::RectF(textLeft, row.top + dpi(app, 3.0f),
                                row.right - dpi(app, 34.0f), row.bottom),
                    app.folderBrowserFormat,
                    dimText(app, item.isDir ? 0.9f : 0.82f));
        // Selection box on the right for files
        if (!item.isDir) {
            float bs = dpi(app, 14.0f);
            D2D1_RECT_F box = D2D1::RectF(row.right - bs - dpi(app, 10.0f),
                                          cy - bs * 0.5f,
                                          row.right - dpi(app, 10.0f),
                                          cy + bs * 0.5f);
            if (picked) {
                app.brush->SetColor(app.theme.accent);
                app.renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(box, dpi(app, 3.0f), dpi(app, 3.0f)),
                    app.brush);
                drawCheckGlyph(app, (box.left + box.right) * 0.5f,
                               (box.top + box.bottom) * 0.5f, dpi(app, 6.0f),
                               D2D1::ColorF(1, 1, 1, 0.97f));
            } else {
                D2D1_COLOR_F edge = dimText(app, 0.35f);
                app.brush->SetColor(edge);
                app.renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(box, dpi(app, 3.0f), dpi(app, 3.0f)),
                    app.brush, 1.0f);
            }
        }
        app.remoteHits.push_back({row, kRBBrowseRow + (int)i});
    }
    app.renderTarget->PopAxisAlignedClip();

    // Footer: status line, open, cancel
    {
        float by = panel.bottom - dpi(app, 12.0f) - dpi(app, 26.0f);
        std::wstring status;
        D2D1_COLOR_F statusColor = dimText(app, 0.5f);
        if (app.remoteInFlight > 0) {
            status = tr(app, "remote.loading");
        } else if (!app.remoteMessage.empty()) {
            status = app.remoteMessage;
            statusColor = D2D1::ColorF(0.86f, 0.42f, 0.38, 0.95f);
        } else if (!app.remotePicked.empty()) {
            status = tr(app, "remote.picked");
        }
        float openW = measureText(app, tr(app, "remote.open"),
                                  app.folderBrowserFormat) + dpi(app, 26.0f);
        float cancelW = measureText(app, tr(app, "remote.cancel"),
                                    app.folderBrowserFormat) + dpi(app, 24.0f);
        D2D1_RECT_F openR = D2D1::RectF(panel.right - pad - openW, by,
                                        panel.right - pad, by + dpi(app, 26.0f));
        D2D1_RECT_F cancelR = D2D1::RectF(openR.left - dpi(app, 8.0f) - cancelW,
                                          by, openR.left - dpi(app, 8.0f),
                                          by + dpi(app, 26.0f));
        bool canOpen = !app.remotePicked.empty() && app.remoteInFlight == 0;
        drawButton(app, openR, tr(app, "remote.open"), true, canOpen);
        drawButton(app, cancelR, tr(app, "remote.cancel"), false, true);
        app.remoteHits.push_back({openR, kRBOpen});
        app.remoteHits.push_back({cancelR, kRBClose});
        drawTrimmed(app, status,
                    D2D1::RectF(panel.left + pad, by + dpi(app, 5.0f),
                                cancelR.left - dpi(app, 12.0f),
                                by + dpi(app, 26.0f)),
                    app.signalSmallFormat, statusColor);
    }
}

// Starts reads for every picked file, then leaves the browser
void remoteBrowserOpenSelection(App& app) {
    if (app.remotePicked.empty() || app.remoteHost.empty()) return;
    std::vector<std::string> uris;
    for (int idx : app.remotePicked) {
        if (idx < 0 || idx >= (int)app.remoteRows.size()) continue;
        if (app.remoteRows[idx].isDir) continue;
        std::string file = app.remoteDir;
        if (file.empty() || file.back() != '/') file += "/";
        // std::string is bytes; the wide name comes straight from the
        // listing's UTF-8, so convert back to UTF-8 explicitly
        int len = WideCharToMultiByte(CP_UTF8, 0, app.remoteRows[idx].name.c_str(),
                                      -1, nullptr, 0, nullptr, nullptr);
        std::string nameBytes(len > 0 ? len - 1 : 0, '\0');
        if (len > 1)
            WideCharToMultiByte(CP_UTF8, 0, app.remoteRows[idx].name.c_str(), -1,
                                &nameBytes[0], len, nullptr, nullptr);
        file += nameBytes;
        uris.push_back(remotePathMake(app.remoteHost, file));
    }
    app.showRemoteBrowser = false;
    app.remotePicked.clear();
    for (const auto& uri : uris) remoteOpenRequest(app, uri);
    if (!uris.empty()) {
        signalPushKey(app, SIG_INFO, SIGI_FILE, "remote.opening");
    }
}

bool remoteBrowserMouseDown(App& app, HWND hwnd, float x, float y) {
    if (!app.showRemoteBrowser) return false;
    for (const auto& hit : app.remoteHits) {
        if (!pointIn(hit.first, x, y)) continue;
        int code = hit.second;
        if (code == kRBClose) {
            app.showRemoteBrowser = false;
            app.remotePicked.clear();
        } else if (code == kRBOpen) {
            remoteBrowserOpenSelection(app);
        } else if (code == kRBUp) {
            remoteBrowserNavigate(app, remoteParentDir(app.remoteDir));
        } else if (code == kRBHome) {
            remoteBrowserNavigate(app, "");
        } else if (code >= kRBBrowseRow) {
            int idx = code - kRBBrowseRow;
            if (idx >= 0 && idx < (int)app.remoteRows.size()) {
                const auto& item = app.remoteRows[idx];
                if (item.isDir) {
                    std::string dir = app.remoteDir;
                    if (dir.empty() || dir.back() != '/') dir += "/";
                    int len = WideCharToMultiByte(CP_UTF8, 0, item.name.c_str(),
                                                  -1, nullptr, 0, nullptr, nullptr);
                    std::string nameBytes(len > 0 ? len - 1 : 0, '\0');
                    if (len > 1)
                        WideCharToMultiByte(CP_UTF8, 0, item.name.c_str(), -1,
                                            &nameBytes[0], len, nullptr, nullptr);
                    dir += nameBytes;
                    remoteBrowserNavigate(app, dir);
                } else {
                    auto it = std::find(app.remotePicked.begin(),
                                        app.remotePicked.end(), idx);
                    if (it == app.remotePicked.end())
                        app.remotePicked.push_back(idx);
                    else
                        app.remotePicked.erase(it);
                }
            }
        } else if (code >= kRBHostBase && code < kRBHostBase + 200) {
            auto hosts = remoteConfigHosts();
            int idx = code - kRBHostBase;
            if (idx >= 0 && idx < (int)hosts.size()) {
                if (app.remoteHost != hosts[idx].alias) {
                    app.remoteHost = hosts[idx].alias;
                    app.remoteDir.clear();
                    app.remoteRows.clear();
                    app.remotePicked.clear();
                    remoteBrowserNavigate(app, "");
                }
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    // Presses inside the panel are inert; outside, the dialog dismisses
    if (!pointIn(remoteBrowserPanel(app), x, y)) {
        app.showRemoteBrowser = false;
        app.remotePicked.clear();
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

bool remoteBrowserWheel(App& app, float delta, float x, float y) {
    if (!app.showRemoteBrowser) return false;
    D2D1_RECT_F panel = remoteBrowserPanel(app);
    // A wheel outside the panel leaves the browser up: it is a persistent
    // overlay, and dismissing it on a stray scroll made the list vanish
    // the moment the pointer drifted off it.
    if (!pointIn(panel, x, y)) return false;
    float rowH = rbRowHeight(app);
    const D2D1_RECT_F& list = app.remoteListRect;
    float viewH = std::max(rowH, list.bottom - list.top);
    float maxScroll = std::max(0.0f, (float)app.remoteRows.size() * rowH - viewH);
    app.remoteScroll = std::clamp(app.remoteScroll + delta, 0.0f, maxScroll);
    InvalidateRect(app.hwnd, nullptr, FALSE);
    return true;
}

// --- recent documents menu ---------------------------------------------

namespace {

constexpr int kHMLocalBase = 1;      // 1..99 local rows
constexpr int kHMRemoteBase = 101;   // 101..199 remote rows
constexpr int kHMMore = 300;
constexpr int kHMClose = 0;
constexpr int kMaxMenuRows = 10;     // per section, as specified

struct HistoryMenuLayout {
    D2D1_RECT_F panel{};
    float localHeaderY = 0, localRowsY = 0;
    float remoteHeaderY = 0, remoteRowsY = 0;
    float moreY = 0;
    int localCount = 0, remoteCount = 0;   // rows actually laid out
    float rowH = 0, headerH = 0;
};

float historyRowHeight(const App& app) { return dpi(app, 26.0f); }
float historyHeaderHeight(const App& app) { return dpi(app, 22.0f); }

HistoryMenuLayout historyMenuLayout(const App& app, int localTotal,
                                    int remoteTotal) {
    HistoryMenuLayout m;
    m.rowH = historyRowHeight(app);
    m.headerH = historyHeaderHeight(app);
    float pad = dpi(app, 8.0f);
    float sep = dpi(app, 9.0f);
    float moreH = dpi(app, 30.0f);
    m.localCount = std::min(localTotal, kMaxMenuRows);
    m.remoteCount = std::min(remoteTotal, kMaxMenuRows);
    int localRows = std::max(1, m.localCount);    // empty state still rows
    int remoteRows = std::max(1, m.remoteCount);
    float w = std::min(dpi(app, 400.0f), (float)app.width - dpi(app, 16.0f));
    float h = pad + m.headerH + localRows * m.rowH + sep + m.headerH +
              remoteRows * m.rowH + sep + moreH + pad;
    D2D1_RECT_F btn = historyButtonRect(app);
    float x = std::max(dpi(app, 8.0f),
                       std::min(btn.right - w, (float)app.width - w - dpi(app, 8.0f)));
    float y = chromeTopHeight(app) + dpi(app, 4.0f);
    m.panel = D2D1::RectF(x, y, x + w, y + h);
    m.localHeaderY = y + pad;
    m.localRowsY = m.localHeaderY + m.headerH;
    m.remoteHeaderY = m.localRowsY + localRows * m.rowH + sep;
    m.remoteRowsY = m.remoteHeaderY + m.headerH;
    m.moreY = m.remoteRowsY + remoteRows * m.rowH + sep;
    return m;
}

void drawSectionHeader(App& app, const std::wstring& text,
                       const D2D1_RECT_F& panel, float y) {
    drawTrimmed(app, text,
                D2D1::RectF(panel.left + dpi(app, 12.0f), y + dpi(app, 3.0f),
                            panel.right - dpi(app, 12.0f), y + dpi(app, 20.0f)),
                app.signalSmallFormat, dimText(app, 0.45f));
}

void drawMenuRow(App& app, const D2D1_RECT_F& row, const std::wstring& name,
                 const std::wstring& hint, bool hot, bool accent) {
    if (hot) {
        D2D1_COLOR_F bg = app.theme.accent;
        bg.a = 0.16f;
        app.brush->SetColor(bg);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(row, dpi(app, 4.0f), dpi(app, 4.0f)), app.brush);
    }
    float hintW = hint.empty() ? 0.0f : dpi(app, 150.0f);
    drawTrimmed(app, name,
                D2D1::RectF(row.left + dpi(app, 12.0f), row.top + dpi(app, 4.0f),
                            row.right - dpi(app, 12.0f) - hintW, row.bottom),
                app.folderBrowserFormat,
                accent ? app.theme.accent : dimText(app, 0.9f));
    if (!hint.empty()) {
        drawTrimmed(app, hint,
                    D2D1::RectF(row.right - dpi(app, 12.0f) - hintW,
                                row.top + dpi(app, 5.0f),
                                row.right - dpi(app, 12.0f), row.bottom),
                    app.signalSmallFormat, dimText(app, 0.35f),
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

// Opens a remembered local file, replacing the launcher tab when that is
// what the window shows (same dance as the start page's recents)
void openLocalFromHistory(App& app, HWND hwnd, const std::string& path) {
    if (GetFileAttributesW(toWide(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
        Settings stored = loadSettings();
        for (size_t i = 0; i < stored.recentFiles.size(); i++) {
            if (_stricmp(stored.recentFiles[i].path.c_str(), path.c_str()) == 0) {
                stored.recentFiles.erase(stored.recentFiles.begin() + i);
                break;
            }
        }
        saveSettings(stored);
        signalPushKey(app, SIG_WARN, SIGI_WARNING, "history.missing");
        return;
    }
    bool launcher = app.currentFile.empty() && !app.editMode;
    tabsInit(app);
    int shellId = launcher ? app.tabs[app.activeTab].id : 0;
    tabOpenPath(app, hwnd, path);
    if (launcher) {
        for (int i = 0; i < (int)app.tabs.size(); i++) {
            if (app.tabs[i].id == shellId) {
                tabCloseIndex(app, hwnd, i);
                break;
            }
        }
    }
}

}  // namespace

D2D1_RECT_F historyButtonRect(const App& app) {
    // Top strip: the slot kept free between the tab controls and the drag
    // gap, so the caption island (and its drag area) stays untouched.
    // Vertical rail: the top-right corner of the rail column.
    if (leftTabsRailShown(app)) {
        float w = leftTabsRailWidth(app);
        return D2D1::RectF(w - dpi(app, 38.0f), dpi(app, 4.0f),
                           w - dpi(app, 4.0f), dpi(app, 36.0f));
    }
    // Between the tab controls and the drag gap, and only when that slack
    // exists: narrow strips and the split editor keep their tab space
    float right = titleDragRect(app).left - dpi(app, 4.0f);
    float left = right - dpi(app, 34.0f);
    if (left < tabStripControlsRight(app) + dpi(app, 6.0f))
        return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    return D2D1::RectF(left, dpi(app, 4.0f), right, dpi(app, 36.0f));
}

void toggleHistoryMenu(App& app, HWND hwnd) {
    app.showHistoryMenu = !app.showHistoryMenu;
    app.showHistoryDialog = false;
    app.historyHoverAt = 0;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void renderHistoryMenu(App& app) {
    app.historyHits.clear();
    if (!app.showHistoryMenu) return;
    Settings stored = loadSettings();
    int localTotal = (int)stored.recentFiles.size();
    int remoteTotal = (int)stored.remoteFiles.size();
    HistoryMenuLayout m = historyMenuLayout(app, localTotal, remoteTotal);

    fillPanel(app, m.panel, dpi(app, 8.0f));
    float pad = dpi(app, 8.0f);

    drawSectionHeader(app, tr(app, "history.local"), m.panel, m.localHeaderY);
    for (int i = 0; i < std::max(1, m.localCount); i++) {
        D2D1_RECT_F row = D2D1::RectF(m.panel.left + pad,
                                      m.localRowsY + i * m.rowH,
                                      m.panel.right - pad,
                                      m.localRowsY + (i + 1) * m.rowH);
        if (i >= m.localCount) {
            drawTrimmed(app, tr(app, "history.none"),
                        D2D1::RectF(row.left + dpi(app, 12.0f),
                                    row.top + dpi(app, 4.0f), row.right,
                                    row.bottom),
                        app.folderBrowserFormat, dimText(app, 0.32f));
            break;
        }
        std::wstring path = toWide(stored.recentFiles[i].path);
        drawMenuRow(app, row, leafName(path), parentLabel(path),
                    pointIn(row, (float)app.mouseX, (float)app.mouseY), false);
        app.historyHits.push_back({row, kHMLocalBase + i});
    }

    {
        D2D1_COLOR_F line = panelBorder(app);
        app.brush->SetColor(line);
        app.renderTarget->DrawLine(
            D2D1::Point2F(m.panel.left + pad, m.remoteHeaderY - dpi(app, 5.0f)),
            D2D1::Point2F(m.panel.right - pad, m.remoteHeaderY - dpi(app, 5.0f)),
            app.brush, 1.0f);
    }
    drawSectionHeader(app, tr(app, "history.remote"), m.panel, m.remoteHeaderY);
    for (int i = 0; i < std::max(1, m.remoteCount); i++) {
        D2D1_RECT_F row = D2D1::RectF(m.panel.left + pad,
                                      m.remoteRowsY + i * m.rowH,
                                      m.panel.right - pad,
                                      m.remoteRowsY + (i + 1) * m.rowH);
        if (i >= m.remoteCount) {
            drawTrimmed(app, tr(app, "history.none"),
                        D2D1::RectF(row.left + dpi(app, 12.0f),
                                    row.top + dpi(app, 4.0f), row.right,
                                    row.bottom),
                        app.folderBrowserFormat, dimText(app, 0.32f));
            break;
        }
        std::string uri = stored.remoteFiles[i].path;
        std::wstring name = leafName(toWide(remotePathFile(uri)));
        std::wstring hint = toWide(remotePathHost(uri));
        drawMenuRow(app, row, name, hint,
                    pointIn(row, (float)app.mouseX, (float)app.mouseY), false);
        app.historyHits.push_back({row, kHMRemoteBase + i});
    }

    {
        D2D1_COLOR_F line = panelBorder(app);
        app.brush->SetColor(line);
        app.renderTarget->DrawLine(
            D2D1::Point2F(m.panel.left + pad, m.moreY - dpi(app, 5.0f)),
            D2D1::Point2F(m.panel.right - pad, m.moreY - dpi(app, 5.0f)),
            app.brush, 1.0f);
    }
    D2D1_RECT_F more = D2D1::RectF(m.panel.left + pad, m.moreY,
                                   m.panel.right - pad, m.moreY + dpi(app, 30.0f));
    bool hot = pointIn(more, (float)app.mouseX, (float)app.mouseY);
    if (hot) {
        D2D1_COLOR_F bg = app.theme.accent;
        bg.a = 0.16f;
        app.brush->SetColor(bg);
        app.renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(more, dpi(app, 4.0f), dpi(app, 4.0f)), app.brush);
    }
    drawTrimmed(app, tr(app, "history.more"),
                D2D1::RectF(more.left + dpi(app, 12.0f), more.top + dpi(app, 5.0f),
                            more.right - dpi(app, 16.0f), more.bottom),
                app.folderBrowserFormat, app.theme.accent);
    app.historyHits.push_back({more, kHMMore});
}

bool historyMenuMouseDown(App& app, HWND hwnd, float x, float y) {
    if (!app.showHistoryMenu) return false;
    Settings stored = loadSettings();
    for (const auto& hit : app.historyHits) {
        if (!pointIn(hit.first, x, y)) continue;
        int code = hit.second;
        app.showHistoryMenu = false;
        if (code >= kHMLocalBase && code < kHMLocalBase + 100) {
            int i = code - kHMLocalBase;
            if (i < (int)stored.recentFiles.size())
                openLocalFromHistory(app, hwnd, stored.recentFiles[i].path);
        } else if (code >= kHMRemoteBase && code < kHMRemoteBase + 100) {
            int i = code - kHMRemoteBase;
            if (i < (int)stored.remoteFiles.size())
                openRemoteDocument(app, hwnd, stored.remoteFiles[i].path);
        } else if (code == kHMMore) {
            app.showHistoryDialog = true;
            app.historyLocalScroll = 0.0f;
            app.historyRemoteScroll = 0.0f;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    // Presses inside the panel are inert; anywhere else dismisses
    Settings live = loadSettings();
    HistoryMenuLayout m = historyMenuLayout(app, (int)live.recentFiles.size(),
                                            (int)live.remoteFiles.size());
    if (!pointIn(m.panel, x, y)) app.showHistoryMenu = false;
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

// --- all history ("More") dialog ---------------------------------------

namespace {

constexpr int kHDLocalBase = 1;
constexpr int kHDRemoteBase = 1001;
constexpr int kHDClose = 3000;

D2D1_RECT_F historyDialogPanel(const App& app) {
    float w = std::min(dpi(app, 1040.0f), (float)app.width - dpi(app, 80.0f));
    float h = std::min(dpi(app, 660.0f), (float)app.height - dpi(app, 110.0f));
    w = std::max(w, dpi(app, 520.0f));
    h = std::max(h, dpi(app, 340.0f));
    float cx = (float)app.width * 0.5f, cy = (float)app.height * 0.5f;
    return D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f,
                       cy + h * 0.5f);
}

struct HistoryDialogColumns {
    D2D1_RECT_F local{};
    D2D1_RECT_F remote{};
    float headerH = 0.0f;
};

HistoryDialogColumns historyDialogColumns(const App& app) {
    HistoryDialogColumns c;
    D2D1_RECT_F p = historyDialogPanel(app);
    float pad = dpi(app, 16.0f);
    float gap = dpi(app, 18.0f);
    float top = p.top + dpi(app, 46.0f);
    float bottom = p.bottom - dpi(app, 14.0f);
    float colW = (p.right - p.left - pad * 2.0f - gap) * 0.5f;
    c.headerH = dpi(app, 24.0f);
    c.local = D2D1::RectF(p.left + pad, top, p.left + pad + colW, bottom);
    c.remote = D2D1::RectF(c.local.right + gap, top, p.right - pad, bottom);
    return c;
}

}  // namespace

void renderHistoryDialog(App& app) {
    app.historyDialogHits.clear();
    if (!app.showHistoryDialog) return;
    Settings stored = loadSettings();
    D2D1_RECT_F panel = historyDialogPanel(app);
    fillPanel(app, panel, dpi(app, 12.0f));

    float pad = dpi(app, 16.0f);
    // Title + close
    drawTrimmed(app, tr(app, "history.dialog_title"),
                D2D1::RectF(panel.left + pad, panel.top + dpi(app, 14.0f),
                            panel.right - dpi(app, 48.0f),
                            panel.top + dpi(app, 38.0f)),
                app.folderBrowserFormat, dimText(app, 0.95f));
    {
        D2D1_RECT_F x = D2D1::RectF(panel.right - dpi(app, 36.0f),
                                    panel.top + dpi(app, 12.0f),
                                    panel.right - dpi(app, 14.0f),
                                    panel.top + dpi(app, 34.0f));
        D2D1_COLOR_F c = dimText(app, 0.55f);
        app.brush->SetColor(c);
        float s = dpi(app, 4.0f);
        float cx = (x.left + x.right) * 0.5f, cy = (x.top + x.bottom) * 0.5f;
        app.renderTarget->DrawLine(D2D1::Point2F(cx - s, cy - s),
                                   D2D1::Point2F(cx + s, cy + s), app.brush,
                                   dpi(app, 1.3f));
        app.renderTarget->DrawLine(D2D1::Point2F(cx - s, cy + s),
                                   D2D1::Point2F(cx + s, cy - s), app.brush,
                                   dpi(app, 1.3f));
        app.historyDialogHits.push_back({x, kHDClose});
    }

    HistoryDialogColumns cols = historyDialogColumns(app);
    float rowH = dpi(app, 28.0f);

    struct Side {
        D2D1_RECT_F rect;
        const std::vector<Settings::RecentFile>* list;
        const char* titleKey;
        bool remote;
        int base;
        float* scroll;
    };
    Side sides[2] = {
        {cols.local, &stored.recentFiles, "history.local", false, kHDLocalBase,
         &app.historyLocalScroll},
        {cols.remote, &stored.remoteFiles, "history.remote", true,
         kHDRemoteBase, &app.historyRemoteScroll},
    };

    for (const Side& side : sides) {
        // Column header with its count
        wchar_t count[32];
        swprintf_s(count, _countof(count), L" (%d)", (int)side.list->size());
        std::wstring title = tr(app, side.titleKey);
        title += count;
        drawTrimmed(app, title,
                    D2D1::RectF(side.rect.left, side.rect.top, side.rect.right,
                                side.rect.top + cols.headerH),
                    app.signalSmallFormat, dimText(app, 0.5f));
        D2D1_RECT_F list = D2D1::RectF(side.rect.left,
                                       side.rect.top + cols.headerH,
                                       side.rect.right, side.rect.bottom);
        D2D1_COLOR_F line = panelBorder(app);
        line.a *= 0.8f;
        app.brush->SetColor(line);
        app.renderTarget->DrawLine(
            D2D1::Point2F(list.left, list.top - dpi(app, 4.0f)),
            D2D1::Point2F(list.right, list.top - dpi(app, 4.0f)), app.brush,
            1.0f);

        float viewH = list.bottom - list.top;
        float maxScroll = std::max(0.0f, (float)side.list->size() * rowH - viewH);
        *side.scroll = std::clamp(*side.scroll, 0.0f, maxScroll);

        app.renderTarget->PushAxisAlignedClip(list,
                                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (side.list->empty()) {
            drawTrimmed(app, tr(app, "history.none"),
                        D2D1::RectF(list.left + dpi(app, 6.0f),
                                    list.top + dpi(app, 4.0f), list.right,
                                    list.top + rowH),
                        app.folderBrowserFormat, dimText(app, 0.3f));
        }
        int first = std::max(0, (int)(*side.scroll / rowH));
        for (size_t i = (size_t)first; i < side.list->size(); i++) {
            float ry = list.top + rowH * (float)i - *side.scroll;
            if (ry > list.bottom) break;
            D2D1_RECT_F row = D2D1::RectF(list.left, ry, list.right, ry + rowH);
            if (row.bottom < list.top) continue;
            const std::string& p = (*side.list)[i].path;
            std::wstring name, hint;
            if (side.remote) {
                name = leafName(toWide(remotePathFile(p)));
                hint = toWide(remotePathHost(p) + ":" +
                              remoteParentDir(remotePathFile(p)));
            } else {
                std::wstring wp = toWide(p);
                name = leafName(wp);
                hint = parentLabel(wp);
            }
            drawMenuRow(app, row, name, hint,
                        pointIn(row, (float)app.mouseX, (float)app.mouseY),
                        false);
            app.historyDialogHits.push_back(
                {row, side.base + (int)i});
        }
        app.renderTarget->PopAxisAlignedClip();
    }
}

bool historyDialogMouseDown(App& app, HWND hwnd, float x, float y) {
    if (!app.showHistoryDialog) return false;
    Settings stored = loadSettings();
    for (const auto& hit : app.historyDialogHits) {
        if (!pointIn(hit.first, x, y)) continue;
        int code = hit.second;
        if (code == kHDClose) {
            app.showHistoryDialog = false;
        } else if (code >= kHDLocalBase && code < kHDLocalBase + 900) {
            int i = code - kHDLocalBase;
            app.showHistoryDialog = false;
            if (i < (int)stored.recentFiles.size())
                openLocalFromHistory(app, hwnd, stored.recentFiles[i].path);
        } else if (code >= kHDRemoteBase && code < kHDRemoteBase + 900) {
            int i = code - kHDRemoteBase;
            app.showHistoryDialog = false;
            if (i < (int)stored.remoteFiles.size())
                openRemoteDocument(app, hwnd, stored.remoteFiles[i].path);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    D2D1_RECT_F panel = historyDialogPanel(app);
    if (!pointIn(panel, x, y)) app.showHistoryDialog = false;
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

bool historyDialogWheel(App& app, float delta, float x, float y) {
    if (!app.showHistoryDialog) return false;
    Settings stored = loadSettings();
    HistoryDialogColumns cols = historyDialogColumns(app);
    float rowH = dpi(app, 28.0f);
    D2D1_RECT_F localList = D2D1::RectF(cols.local.left,
                                        cols.local.top + cols.headerH,
                                        cols.local.right, cols.local.bottom);
    D2D1_RECT_F remoteList = D2D1::RectF(cols.remote.left,
                                         cols.remote.top + cols.headerH,
                                         cols.remote.right, cols.remote.bottom);
    if (pointIn(localList, x, y)) {
        float maxScroll = std::max(0.0f, (float)stored.recentFiles.size() * rowH -
                                            (localList.bottom - localList.top));
        app.historyLocalScroll =
            std::clamp(app.historyLocalScroll + delta, 0.0f, maxScroll);
    } else if (pointIn(remoteList, x, y)) {
        float maxScroll = std::max(0.0f, (float)stored.remoteFiles.size() * rowH -
                                            (remoteList.bottom - remoteList.top));
        app.historyRemoteScroll =
            std::clamp(app.historyRemoteScroll + delta, 0.0f, maxScroll);
    }
    InvalidateRect(app.hwnd, nullptr, FALSE);
    return true;
}

// --- self-test (--remote-tests) ----------------------------------------

int runRemoteSelfTest(const std::wstring& reportPath) {
    std::string report;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        report += ok ? "PASS " : "FAIL ";
        report += what;
        report += "\n";
        if (!ok) failures++;
    };

    // Config: only the concrete alias, wildcard patterns skipped
    auto hosts = remoteConfigHosts();
    check(hosts.size() == 1 && hosts[0].alias == "testbox",
          "config: one concrete Host alias, wildcard skipped");

    // The remote root: "/" on a real server, or an override that points at
    // the test user's home (~/...) when the account cannot write to /
    std::string base;
    {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"TINTA_REMOTE_TEST_BASE", buf,
                                          MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0,
                                          nullptr, nullptr);
            base.assign(len > 0 ? (size_t)len - 1 : 0, '\0');
            if (len > 1)
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, &base[0], len, nullptr,
                                    nullptr);
        }
    }

    // Listing: Chinese names, space in a name, directories first
    std::vector<RemoteDirEntry> entries;
    std::string err;
    check(remoteListBlocking("testbox", "", entries, err),
          "list: home directory readable");
    bool hasDir = false, hasCjkFile = false;
    for (const auto& e : entries) {
        if (e.isDir && e.name == L"\u5B50\u76EE\u5F55") hasDir = true;
        if (!e.isDir && e.name == L"\u7B14\u8BB0 \u4E00.md") hasCjkFile = true;
    }
    check(hasDir, "list: CJK directory name decoded");
    check(hasCjkFile, "list: CJK file name with a space decoded");
    check(!entries.empty() && entries[0].isDir, "list: directories sort first");

    // Reading a file whose path is Chinese
    std::string content;
    err.clear();
    const std::string sample = base + "/\u7B14\u8BB0 \u4E00.md";
    bool readOk = remoteReadBlocking("testbox", sample, content, err);
    check(readOk, "read: CJK path fetched");
    check(content.find("\u8FDC\u7AEF\u4E2D\u6587\u6B63\u6587") !=
              std::string::npos,
          "read: UTF-8 body bytes intact");
    check(content.rfind("# ", 0) == 0, "read: markdown starts at byte 0");

    // A missing remote file reports an error instead of empty content
    std::string dummy;
    err.clear();
    bool bad = remoteReadBlocking("testbox",
                                  base + "/\u4E0D\u5B58\u5728.md", dummy, err);
    check(!bad && !err.empty(), "read: missing file surfaces an error");

    // URI helpers
    std::string uri = remotePathMake("testbox", "/a/\u4E2D\u6587.md");
    check(remotePathIs(uri), "uri: ssh scheme recognised");
    check(remotePathHost(uri) == "testbox", "uri: host parsed");
    check(remotePathFile(uri) == "/a/\u4E2D\u6587.md", "uri: path parsed");
    check(remoteParentDir("/a/b/c.md") == "/a/b", "uri: parent directory");
    check(remoteParentDir("/x") == "/", "uri: parent of top level");

    // Content cache
    remoteStoreContent(uri, "# cached");
    std::string* cached = remoteCachedContent(uri);
    check(cached && *cached == "# cached", "cache: round trip");

    // Document plumbing: a remote document loads read-only with a real tree
    App app;
    bool opened = openRemoteDocumentInViewer(app, uri);
    check(opened, "doc: remote document opened");
    check(app.currentFile == uri, "doc: uri is the document identity");
    check(app.remoteReadOnly, "doc: flagged read-only");
    check(app.root != nullptr, "doc: parsed tree present");
    // Requirement: remote documents are read-only
    enterEditMode(app);
    check(!app.editMode, "doc: edit mode refused on a remote document");

    // Remote recents feed the history menu
    persistRemoteFile(uri);
    Settings stored = loadSettings();
    check(!stored.remoteFiles.empty() && stored.remoteFiles[0].path == uri,
          "recents: newest remote document heads the list");

    // Report (UTF-8; failures in the first line for quick greps)
    std::string header = "remote self-test: " + std::to_string(failures) +
                         " failure(s)\n";
    FILE* f = _wfopen(reportPath.c_str(), L"wb");
    if (f) {
        fputs(header.c_str(), f);
        fwrite(report.data(), 1, report.size(), f);
        fclose(f);
    }
    return failures;
}

void closeRemoteOverlays(App& app) {
    app.showRemoteBrowser = false;
    app.showHistoryMenu = false;
    app.showHistoryDialog = false;
    app.remotePicked.clear();
}

