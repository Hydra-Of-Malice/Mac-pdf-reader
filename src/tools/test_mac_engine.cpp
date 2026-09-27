/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Headless driver for the macOS engine bridge (src/mac/SumatraMacEngine.h). Uses the bridge
// exactly like SumatraMac.mm (plain C API, no base/Base.h) and prints one JSON object per run.
// Driven by tests/mac/run-engine-tests.ts; runs on macOS and on Linux (-mac-core builds).
//
// usage: test_mac_engine <file> [-password <p1[,p2...]>] [-search <word>] [-stress <nPages>] [-render-all] [-fuzz]
//          [-colors normal|smart|invert [-colors-bg RRGGBB] [-colors-text RRGGBB] [-no-preserve-images]
//          [-region x0,y0,x1,y1] [-colors-page <n>] [-colors-dump <prefix>]]
// -colors: document colors (dark mode) for the whole run, plus a check of page 1 in them vs Normal (in -region,
// fractions of the page, too), of stale renders after a switch, and of thumbnails. -colors-dump <prefix> saves
// page 1 in both as <prefix>-normal.ppm and <prefix>-themed.ppm.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>
#include <execinfo.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "mac/SumatraMacEngine.h"
#include "mac/MacThumbnails.h"

// base/UITask.h: the bridge posts page-ready / find-done / layout tasks to the
// main thread; PumpUiTasks() runs them the way the app's run loop does
namespace uitask {
void Initialize();
void DrainQueue();
} // namespace uitask

constexpr int kMaxPasswords = 4;
constexpr int kAsyncPages = 8;
constexpr int kLinkScanPages = 3;
constexpr int kLinkScanStep = 4;
constexpr double kLinkScanMaxCells = 400;
constexpr double kFuzzLinkScanMaxCells = 60;
constexpr int kMaxLinks = 16;
constexpr int kRenderAllMax = 64;
constexpr int kWaitMs = 30000;
// -fuzz: damaged docs can leave a page that never arrives from the async renderer
constexpr int kFuzzWaitMs = 3000;
constexpr unsigned kFuzzWatchdogSecs = 40;
// -fuzz: damaged headers can claim huge pages (e.g. 50952 x 2400 px), and whole-document
// selection of a big book takes long under ASan; neither is a hang
constexpr double kFuzzMaxRenderPx = 2048;
constexpr double kMaxRenderPixels = 32.0 * 1024 * 1024;
constexpr int kFuzzMaxSelectAllPages = 100;
constexpr int kSettleMs = 20;
constexpr float kZoomFitWidth = -2.f;
constexpr char kMissingWord[] = "xq7zzyNotInAnyFixture";

struct PasswordState {
    const char* passwords[kMaxPasswords] = {};
    int count = 0;
    int prompts = 0;
};

static int gPagesReady = 0;
static int gFindCallbacks = 0;
static bool gFirstField = true;
static int gWaitMs = kWaitMs;
static bool gFuzz = false;
static double gLinkScanMaxCells = kLinkScanMaxCells;

static double NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void PumpUiTasks() {
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
#endif
    uitask::DrainQueue();
}

static void SleepMs(int ms) {
    PumpUiTasks();
    usleep((useconds_t)ms * 1000);
}

// stdout is flushed too, so a crash leaves the report of the stages that ran
static void Stage(const char* name) {
    fflush(stdout);
    fprintf(stderr, "stage: %s\n", name);
    fflush(stderr);
}

//--- JSON output

static void JsonStr(const char* s) {
    if (!s) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

static void Key(const char* name) {
    if (!gFirstField) {
        putchar(',');
    }
    gFirstField = false;
    printf("\n\"%s\":", name);
}

static void KeyInt(const char* name, long long v) {
    Key(name);
    printf("%lld", v);
}

static void KeyNum(const char* name, double v) {
    Key(name);
    printf("%.2f", v);
}

static void KeyBool(const char* name, bool v) {
    Key(name);
    fputs(v ? "true" : "false", stdout);
}

static void KeyStr(const char* name, const char* v) {
    Key(name);
    JsonStr(v);
}

//--- bridge callbacks

static void OnPageReady(void* context) {
    __atomic_add_fetch((int*)context, 1, __ATOMIC_SEQ_CST);
}

// first prompt gets passwords[0], the next passwords[1] ...; then cancel
static const char* OnPassword(void* context, const char*, int attempt) {
    auto* st = (PasswordState*)context;
    st->prompts++;
    if (attempt < 1 || attempt > st->count) {
        return nullptr;
    }
    return st->passwords[attempt - 1];
}

static void OnFindDone(void*, void*, int, bool) {
    __atomic_add_fetch(&gFindCallbacks, 1, __ATOMIC_SEQ_CST);
}

//--- checks

struct RenderStats {
    bool ok = false;
    int width = 0;
    int height = 0;
    double inkRatio = 0;
};

// share of pixels that aren't plain white: 0 means a blank page
static RenderStats Stats(bool ok, const MacRenderedPage& page) {
    RenderStats st;
    st.ok = ok && page.data;
    if (!st.ok) {
        return st;
    }
    st.width = page.width;
    st.height = page.height;
    long long ink = 0;
    for (int y = 0; y < page.height; y++) {
        const unsigned char* row = page.data + (size_t)y * (size_t)page.stride;
        for (int x = 0; x < page.width; x++) {
            const unsigned char* p = row + x * 4;
            if (p[0] < 240 || p[1] < 240 || p[2] < 240) {
                ink++;
            }
        }
    }
    long long total = (long long)page.width * page.height;
    st.inkRatio = total > 0 ? (double)ink / (double)total : 0;
    return st;
}

static void KeyRender(const char* name, const RenderStats& st) {
    Key(name);
    printf("{\"ok\":%s,\"width\":%d,\"height\":%d,\"inkRatio\":%.4f}", st.ok ? "true" : "false", st.width, st.height,
           st.inkRatio);
}

// in -fuzz mode, zoom limited so the page is at most kFuzzMaxRenderPx on a side
static float RenderZoom(void* doc, int pageNo, float zoom) {
    double w = 0;
    double h = 0;
    if (!MacPageSize(doc, pageNo, &w, &h)) {
        return zoom;
    }
    double side = w > h ? w : h;
    if (gFuzz && side * zoom > kFuzzMaxRenderPx) {
        zoom = (float)(kFuzzMaxRenderPx / side);
    }
    // the app's layout never asks for more (kMacMaxRenderPixels in the bridge)
    double area = w * h;
    if (area > 0 && (double)zoom * zoom * area > kMaxRenderPixels) {
        zoom = (float)sqrt(kMaxRenderPixels / area);
    }
    return zoom;
}

static RenderStats RenderSync(void* doc, int pageNo, float zoom, int rotation) {
    zoom = RenderZoom(doc, pageNo, zoom);
    MacRenderedPage page{};
    bool ok = MacRenderPage(doc, pageNo, zoom, rotation, &page);
    RenderStats st = Stats(ok, page);
    MacFreeRenderedPage(&page);
    return st;
}

static void CheckPageSizes(void* doc, int nPages) {
    Stage("page-sizes");
    int bad = 0;
    Key("pageSizes");
    putchar('[');
    for (int pageNo = 1; pageNo <= nPages; pageNo++) {
        double w = 0;
        double h = 0;
        bool ok = MacPageSize(doc, pageNo, &w, &h);
        if (!ok || w <= 0 || h <= 0) {
            bad++;
        }
        if (pageNo <= 5) {
            printf("%s[%.1f,%.1f]", pageNo > 1 ? "," : "", w, h);
        }
    }
    putchar(']');
    KeyInt("badPageSizes", bad);
    KeyNum("fileDpi", MacFileDPI(doc));
}

static void CheckLayout(void* doc, int nPages) {
    Stage("layout");
    MacLayoutParams params{};
    params.continuous = true;
    params.startPage = 1;
    params.viewWidth = 1000;
    params.viewHeight = 800;
    params.zoomVirtual = kZoomFitWidth;
    params.backingScale = 2.0;
    MacDocumentLayout layout{};
    bool ok = MacLayoutDocument(doc, &params, &layout);
    int shown = 0;
    // continuous layout: pages go down the canvas, never at negative or wrapped-around positions
    bool sane = ok && layout.canvasWidth > 0 && layout.canvasHeight > 0;
    int prevY = 0;
    for (int i = 0; ok && i < layout.pageCount; i++) {
        const MacLayoutPage& p = layout.pages[i];
        shown += p.shown ? 1 : 0;
        if (p.y < prevY || p.width < 0 || p.height < 0) {
            sane = false;
        }
        prevY = p.y;
    }
    Key("layout");
    printf("{\"ok\":%s,\"pageCount\":%d,\"canvas\":[%d,%d],\"shown\":%d,\"renderZoom\":%.3f,\"sane\":%s}",
           ok ? "true" : "false", layout.pageCount, layout.canvasWidth, layout.canvasHeight, shown,
           ok ? layout.pages[0].renderZoom : 0.0, sane ? "true" : "false");
    bool countOk = ok && layout.pageCount == nPages;
    MacFreeDocumentLayout(&layout);

    params.continuous = false;
    params.rotation = 90;
    params.startPage = nPages;
    ok = MacLayoutDocument(doc, &params, &layout);
    KeyBool("layoutSinglePageRotated", ok && layout.pageCount == nPages);
    MacFreeDocumentLayout(&layout);
    KeyBool("layoutPageCountOk", countOk);
}

static bool CheckRender(void* doc, int nPages) {
    Stage("render-sync");
    RenderStats first = RenderSync(doc, 1, 1.0f, 0);
    KeyRender("render1", first);
    RenderStats rotated = RenderSync(doc, 1, 0.5f, 90);
    KeyRender("render1Rotated", rotated);
    if (nPages > 1) {
        KeyRender("renderLast", RenderSync(doc, nPages, 1.0f, 0));
    }
    MacRenderedPage page{};
    // chaptered (EPUB) documents can grow while chapters get laid out
    KeyBool("renderOutOfRangeFails", !MacRenderPage(doc, MacPageCount(doc) + 1, 1.0f, 0, &page));
    MacFreeRenderedPage(&page);
    return first.ok;
}

// Request pages through the async PageRenderService and wait for them like the app does.
// Only pages that render synchronously are waited for: a page the engine can't render
// never shows up, and waiting for it would look like a hang.
static void CheckAsyncRender(void* doc, int nPages, bool page1Renders) {
    Stage("render-async");
    int n = nPages < kAsyncPages ? nPages : kAsyncPages;
    float zooms[kAsyncPages + 1] = {};
    bool expected[kAsyncPages + 1] = {};
    int nExpected = 0;
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        zooms[pageNo] = RenderZoom(doc, pageNo, 0.75f);
        expected[pageNo] = RenderSync(doc, pageNo, zooms[pageNo], 0).ok;
        nExpected += expected[pageNo] ? 1 : 0;
    }
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        MacRequestPage(doc, pageNo, zooms[pageNo], 0, pageNo == 1 ? 0 : (pageNo < 4 ? 1 : 2));
    }
    bool got[kAsyncPages + 1] = {};
    int nGot = 0;
    RenderStats first;
    double start = NowMs();
    while (nGot < nExpected && NowMs() - start < gWaitMs) {
        for (int pageNo = 1; pageNo <= n; pageNo++) {
            if (got[pageNo] || !expected[pageNo]) {
                continue;
            }
            MacRenderedPage page{};
            if (MacCopyRenderedPage(doc, pageNo, zooms[pageNo], 0, &page)) {
                got[pageNo] = true;
                nGot++;
                if (pageNo == 1) {
                    first = Stats(true, page);
                }
            }
            MacFreeRenderedPage(&page);
        }
        if (nGot < nExpected) {
            SleepMs(5);
        }
    }
    Key("async");
    printf("{\"requested\":%d,\"expected\":%d,\"copied\":%d,\"callbacks\":%d,\"ms\":%.1f,\"page1Ink\":%.4f}", n,
           nExpected, nGot, __atomic_load_n(&gPagesReady, __ATOMIC_SEQ_CST), NowMs() - start, first.inkRatio);

    // a new generation drops queued requests; requesting again must still work
    MacResetRenderer(doc);
    bool again = false;
    if (page1Renders) {
        float zoom = RenderZoom(doc, 1, 0.5f);
        MacRequestPage(doc, 1, zoom, 0, 0);
        start = NowMs();
        while (!again && NowMs() - start < gWaitMs) {
            MacRenderedPage page{};
            again = MacCopyRenderedPage(doc, 1, zoom, 0, &page);
            MacFreeRenderedPage(&page);
            if (!again) {
                SleepMs(5);
            }
        }
    }
    KeyBool("asyncAfterReset", again);
}

// The done callback is posted to the main run loop, which this driver doesn't
// run, so poll the worker instead; the hit is committed before busy clears.
static bool Find(void* doc, int startPage, const char* word, MacFindDirection dir, bool* timedOut) {
    if (!MacFindStart(doc, startPage, word, dir, MacFindMode::Restart, OnFindDone, nullptr)) {
        *timedOut = false;
        return false;
    }
    double start = NowMs();
    while (MacFindIsBusy(doc) && NowMs() - start < gWaitMs) {
        SleepMs(2);
    }
    *timedOut = MacFindIsBusy(doc);
    if (*timedOut) {
        MacFindCancel(doc);
        return false;
    }
    return MacFindResultPage(doc) > 0;
}

static char* CopySelectionOfRect(void* doc, int pageNo, const MacDisplayRect& r) {
    double y = r.y + r.height / 2;
    if (!MacStartSelection(doc, pageNo, r.x + 1, y, 1.0, 0)) {
        return nullptr;
    }
    MacUpdateSelection(doc, pageNo, r.x + r.width - 1, y, 1.0, 0);
    return MacCopySelectionText(doc);
}

static bool ContainsI(const char* s, const char* word) {
    size_t n = strlen(word);
    for (; s && *s; s++) {
        if (strncasecmp(s, word, n) == 0) {
            return true;
        }
    }
    return false;
}

static void CheckSearch(void* doc, int nPages, const char* word) {
    Stage("search");
    bool timedOut = false;
    bool found = Find(doc, 1, word, MacFindDirection::Forward, &timedOut);
    int page = found ? MacFindResultPage(doc) : 0;
    int rects = page > 0 ? MacFindResultRectCount(doc, page) : 0;
    MacDisplayRect r{};
    bool haveRect = rects > 0 && MacFindResultRect(doc, page, 0, 1.0, 0, &r);
    Key("search");
    printf("{\"word\":");
    JsonStr(word);
    printf(",\"found\":%s,\"page\":%d,\"rects\":%d,\"timedOut\":%s", found ? "true" : "false", page, rects,
           timedOut ? "true" : "false");
    if (haveRect) {
        printf(",\"rect\":[%.1f,%.1f,%.1f,%.1f]", r.x, r.y, r.width, r.height);
    }

    bool backTimedOut = false;
    bool back = Find(doc, nPages, word, MacFindDirection::Backward, &backTimedOut);
    printf(",\"backwardFound\":%s,\"backwardPage\":%d", back ? "true" : "false", back ? MacFindResultPage(doc) : 0);
    bool missTimedOut = false;
    bool miss = Find(doc, 1, kMissingWord, MacFindDirection::Forward, &missTimedOut);
    printf(",\"missingWordFound\":%s,\"anyTimedOut\":%s}", miss ? "true" : "false",
           (timedOut || backTimedOut || missTimedOut) ? "true" : "false");
    MacFindClear(doc);

    Stage("select-word");
    char* sel = haveRect ? CopySelectionOfRect(doc, page, r) : nullptr;
    KeyStr("selectionAtHit", sel);
    MacFreeString(sel);
    MacClearSelection(doc);
}

static void CheckSelectAll(void* doc, const char* word) {
    Stage("select-all");
    if (gFuzz && MacPageCount(doc) > kFuzzMaxSelectAllPages) {
        KeyBool("selectAllSkipped", true);
        return;
    }
    MacSelectAll(doc);
    bool has = MacHasSelection(doc);
    char* text = has ? MacCopySelectionText(doc) : nullptr;
    int nBytes = text ? (int)strlen(text) : 0;
    Key("selectAll");
    printf("{\"hasSelection\":%s,\"bytes\":%d,\"containsWord\":%s,\"rectsPage1\":%d}", has ? "true" : "false", nBytes,
           (word && ContainsI(text, word)) ? "true" : "false", MacSelectionRectCount(doc, 1));
    MacFreeString(text);
    MacClearSelection(doc);
}

static void CheckToc(void* doc) {
    Stage("toc");
    int n = MacTocItemCount(doc);
    int badPages = 0;
    Key("toc");
    printf("{\"count\":%d,\"items\":[", n);
    for (int i = 0; i < n; i++) {
        char* title = MacCopyTocItemTitle(doc, i);
        int pageNo = MacTocItemPage(doc, i);
        // resolving an EPUB destination lays out its chapter, which can add pages
        if (pageNo < 0 || pageNo > MacPageCount(doc)) {
            badPages++;
        }
        if (i < 20) {
            printf("%s{\"title\":", i > 0 ? "," : "");
            JsonStr(title);
            printf(",\"depth\":%d,\"page\":%d}", MacTocItemDepth(doc, i), pageNo);
        }
        MacFreeString(title);
    }
    printf("],\"badPages\":%d}", badPages);
}

struct FoundLink {
    MacLinkKind kind;
    int page;
    int targetPage;
    char* value;
};

static const char* LinkKindName(MacLinkKind kind) {
    switch (kind) {
        case MacLinkKind::Page:
            return "page";
        case MacLinkKind::Url:
            return "url";
        case MacLinkKind::File:
            return "file";
        default:
            return "none";
    }
}

// hit-test a grid over the first pages the way mouse clicks do
static void CheckLinks(void* doc, int nPages) {
    Stage("links");
    FoundLink links[kMaxLinks] = {};
    int nLinks = 0;
    int n = nPages < kLinkScanPages ? nPages : kLinkScanPages;
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        double w = 0;
        double h = 0;
        MacPageSize(doc, pageNo, &w, &h);
        // coarser grid on huge (e.g. corrupt) pages so the scan stays bounded
        double stepX = w / kLinkScanStep > gLinkScanMaxCells ? w / gLinkScanMaxCells : kLinkScanStep;
        double stepY = h / kLinkScanStep > gLinkScanMaxCells ? h / gLinkScanMaxCells : kLinkScanStep;
        for (double y = 1; y < h; y += stepY) {
            for (double x = 1; x < w; x += stepX) {
                MacLink link{};
                if (!MacLinkAtPoint(doc, pageNo, x, y, 1.0, 0, &link)) {
                    continue;
                }
                bool dup = false;
                for (int i = 0; i < nLinks && !dup; i++) {
                    FoundLink& f = links[i];
                    dup = f.kind == link.kind && f.page == pageNo && f.targetPage == link.pageNo &&
                          ((!f.value && !link.value) || (f.value && link.value && !strcmp(f.value, link.value)));
                }
                if (!dup && nLinks < kMaxLinks) {
                    links[nLinks++] = {link.kind, pageNo, link.pageNo, link.value ? strdup(link.value) : nullptr};
                }
                MacFreeLink(&link);
            }
        }
    }
    Key("links");
    putchar('[');
    for (int i = 0; i < nLinks; i++) {
        printf("%s{\"page\":%d,\"kind\":\"%s\",\"targetPage\":%d,\"value\":", i > 0 ? "," : "", links[i].page,
               LinkKindName(links[i].kind), links[i].targetPage);
        JsonStr(links[i].value);
        putchar('}');
        free(links[i].value);
    }
    putchar(']');
}

static void CheckProperties(void* doc) {
    Stage("properties");
    int n = MacPropertyCount(doc);
    Key("properties");
    putchar('{');
    for (int i = 0; i < n; i++) {
        char* name = MacCopyPropertyName(doc, i);
        char* value = MacCopyPropertyValue(doc, i);
        if (i > 0) {
            putchar(',');
        }
        JsonStr(name ? name : "?");
        putchar(':');
        JsonStr(value);
        MacFreeString(name);
        MacFreeString(value);
    }
    putchar('}');
}

//--- document colors (-colors): dark mode

constexpr int kEpochRounds = 4;
constexpr int kThumbDx = 120;
constexpr int kThumbDy = 160;
// Rec. 709 luminance (0..255) above / below which a pixel counts as light / dark
constexpr double kLightLum = 160;
constexpr double kDarkLum = 60;

struct PageRegion {
    // fractions of the page, e.g. where a fixture's photo is
    double x0 = 0;
    double y0 = 0;
    double x1 = 1;
    double y1 = 1;
};

struct ColorStats {
    bool ok = false;
    double meanLum = 0;
    double lightFrac = 0;
    double darkFrac = 0;
    double minLum = 0;
    double maxLum = 0;
};

static MacDocColorScheme gColors = {MacDocColors::Normal, 0x1E1E1E, 0xE6E6E6, true};
// -colors-dump <prefix>: page 1 in Normal and in the scheme, as <prefix>-normal.ppm / <prefix>-themed.ppm
static const char* gColorsDump = nullptr;
// -colors-page <n>: the page the colors check renders (default 1)
static int gColorsPage = 1;
static int gThumbsReady = 0;

static double Lum(const unsigned char* bgra) {
    return 0.2126 * bgra[2] + 0.7152 * bgra[1] + 0.0722 * bgra[0];
}

struct PixelRect {
    int x0, y0, x1, y1;
};

static PixelRect ToPixels(const PageRegion& r, int w, int h) {
    auto clamp = [](double v, int max) { return v < 0 ? 0 : (v > max ? max : (int)v); };
    PixelRect px = {clamp(r.x0 * w, w), clamp(r.y0 * h, h), clamp(r.x1 * w, w), clamp(r.y1 * h, h)};
    if (px.x1 <= px.x0 || px.y1 <= px.y0) {
        px = {0, 0, w, h};
    }
    return px;
}

static ColorStats StatsOf(const unsigned char* data, int w, int h, int stride, const PageRegion& r) {
    ColorStats st;
    if (!data || w <= 0 || h <= 0) {
        return st;
    }
    PixelRect px = ToPixels(r, w, h);
    long long n = 0, light = 0, dark = 0;
    double sum = 0;
    st.minLum = 255;
    for (int y = px.y0; y < px.y1; y++) {
        const unsigned char* row = data + (size_t)y * (size_t)stride;
        for (int x = px.x0; x < px.x1; x++) {
            double l = Lum(row + x * 4);
            sum += l;
            light += l > kLightLum ? 1 : 0;
            dark += l < kDarkLum ? 1 : 0;
            st.minLum = l < st.minLum ? l : st.minLum;
            st.maxLum = l > st.maxLum ? l : st.maxLum;
            n++;
        }
    }
    st.ok = n > 0;
    if (n > 0) {
        st.meanLum = sum / (double)n;
        st.lightFrac = (double)light / (double)n;
        st.darkFrac = (double)dark / (double)n;
    }
    return st;
}

static ColorStats PageStats(const MacRenderedPage& p, const PageRegion& r) {
    return StatsOf(p.data, p.width, p.height, p.stride, r);
}

// mean absolute channel difference of a and b (or of a and b's inverse) in region r; -1 if sizes differ
static double MeanAbsDiff(const MacRenderedPage& a, const MacRenderedPage& b, const PageRegion& r, bool invertB) {
    if (!a.data || !b.data || a.width != b.width || a.height != b.height) {
        return -1;
    }
    PixelRect px = ToPixels(r, a.width, a.height);
    double sum = 0;
    long long n = 0;
    for (int y = px.y0; y < px.y1; y++) {
        const unsigned char* ra = a.data + (size_t)y * (size_t)a.stride;
        const unsigned char* rb = b.data + (size_t)y * (size_t)b.stride;
        for (int x = px.x0; x < px.x1; x++) {
            for (int c = 0; c < 3; c++) {
                int vb = rb[x * 4 + c];
                sum += abs((int)ra[x * 4 + c] - (invertB ? 255 - vb : vb));
            }
            n += 3;
        }
    }
    return n > 0 ? sum / (double)n : -1;
}

static bool SamePixels(const MacRenderedPage& a, const MacRenderedPage& b) {
    if (!a.data || !b.data || a.width != b.width || a.height != b.height) {
        return false;
    }
    for (int y = 0; y < a.height; y++) {
        if (memcmp(a.data + (size_t)y * a.stride, b.data + (size_t)y * b.stride, (size_t)a.width * 4) != 0) {
            return false;
        }
    }
    return true;
}

static void WritePpm(const MacRenderedPage& p, const char* suffix) {
    if (!gColorsDump || !p.data) {
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s-%s.ppm", gColorsDump, suffix);
    FILE* f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", p.width, p.height);
    for (int y = 0; y < p.height; y++) {
        const unsigned char* row = p.data + (size_t)y * p.stride;
        for (int x = 0; x < p.width; x++) {
            unsigned char rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4]};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void SetColors(MacDocColors mode) {
    MacDocColorScheme s = gColors;
    s.mode = mode;
    MacSetDocColors(&s);
}

static void PrintColorStats(const char* name, const ColorStats& st) {
    printf("\"%s\":{\"ok\":%s,\"meanLum\":%.1f,\"lightFrac\":%.6f,\"darkFrac\":%.6f,\"minLum\":%.1f,\"maxLum\":%.1f}",
           name, st.ok ? "true" : "false", st.meanLum, st.lightFrac, st.darkFrac, st.minLum, st.maxLum);
}

// Waits for page 1 at zoom from the async renderer, requesting it again like the app
// does on page-ready (a colors change drops queued requests).
static bool WaitForAsyncPage(void* doc, float zoom, MacRenderedPage* page) {
    double start = NowMs();
    while (NowMs() - start < gWaitMs) {
        MacRequestPage(doc, gColorsPage, zoom, 0, 0);
        if (MacCopyRenderedPage(doc, gColorsPage, zoom, 0, page)) {
            return true;
        }
        SleepMs(2);
    }
    return false;
}

// Page 1 requested under one scheme and the colors switched right away (at various
// points of the render): the first page the renderer delivers must be the new colors'.
static void CheckColorsEpoch(void* doc, float zoom, const MacRenderedPage& refThemed,
                             const MacRenderedPage& refNormal) {
    bool testable = MeanAbsDiff(refThemed, refNormal, PageRegion{}, false) > 2;
    int stale = 0, missing = 0, droppedAtSwitch = 0;
    for (int i = 0; testable && i < kEpochRounds; i++) {
        bool toThemed = i % 2 == 0;
        SetColors(toThemed ? MacDocColors::Normal : gColors.mode);
        MacRequestPage(doc, gColorsPage, zoom, 0, 0);
        SleepMs(i * 3);
        SetColors(toThemed ? gColors.mode : MacDocColors::Normal);
        MacRenderedPage page{};
        // the colors change dropped the cache
        droppedAtSwitch += MacCopyRenderedPage(doc, gColorsPage, zoom, 0, &page) ? 0 : 1;
        MacFreeRenderedPage(&page);
        if (!WaitForAsyncPage(doc, zoom, &page)) {
            missing++;
            continue;
        }
        const MacRenderedPage& want = toThemed ? refThemed : refNormal;
        const MacRenderedPage& old = toThemed ? refNormal : refThemed;
        if (MeanAbsDiff(page, want, PageRegion{}, false) >= MeanAbsDiff(page, old, PageRegion{}, false)) {
            stale++;
        }
        MacFreeRenderedPage(&page);
    }
    MacSetDocColors(&gColors);
    printf(",\"epoch\":{\"testable\":%s,\"rounds\":%d,\"stale\":%d,\"missing\":%d,\"droppedAtSwitch\":%d}",
           testable ? "true" : "false", testable ? kEpochRounds : 0, stale, missing, droppedAtSwitch);
}

static void OnThumbReady(void*, void*, int) {
    __atomic_add_fetch(&gThumbsReady, 1, __ATOMIC_SEQ_CST);
}

static bool WaitForThumb(void* thumbs, void* doc, ColorStats* st) {
    double start = NowMs();
    while (NowMs() - start < gWaitMs) {
        MacThumbsRequest(thumbs, doc, gColorsPage, 0, kThumbDx, kThumbDy, MacThumbPriority::Visible);
        MacThumbImage img{};
        if (MacThumbsGet(thumbs, doc, gColorsPage, 0, kThumbDx, kThumbDy, &img)) {
            *st = StatsOf(img.data, img.width, img.height, img.stride, PageRegion{});
            MacThumbsReleaseImage(img.ref);
            return true;
        }
        SleepMs(2);
    }
    return false;
}

// Thumbnails render with the colors too, and a change drops the old ones.
static void CheckColorsThumbnails(void* doc) {
    void* thumbs = MacThumbsCreate(OnThumbReady, nullptr, 0);
    ColorStats themed, normal;
    bool okThemed = WaitForThumb(thumbs, doc, &themed);
    SetColors(MacDocColors::Normal);
    MacThumbImage img{};
    bool dropped = !MacThumbsGet(thumbs, doc, gColorsPage, 0, kThumbDx, kThumbDy, &img);
    if (!dropped) {
        MacThumbsReleaseImage(img.ref);
    }
    bool okNormal = WaitForThumb(thumbs, doc, &normal);
    MacSetDocColors(&gColors);
    MacThumbsForget(thumbs, doc);
    MacThumbsDestroy(thumbs);
    printf(",\"thumbs\":{\"ok\":%s,\"droppedOnChange\":%s,", okThemed && okNormal ? "true" : "false",
           dropped ? "true" : "false");
    PrintColorStats("themed", themed);
    putchar(',');
    PrintColorStats("normal", normal);
    putchar('}');
}

static const char* ColorsModeName(MacDocColors mode) {
    return mode == MacDocColors::SmartDark ? "smart" : (mode == MacDocColors::Inverted ? "invert" : "normal");
}

// The page in Normal and in the -colors scheme, compared (in region too); switching back
// to Normal must render exactly as before; stale renders of an old scheme are dropped.
static void CheckColors(void* doc, const PageRegion& region) {
    Stage("colors");
    if (gColorsPage < 1 || gColorsPage > MacPageCount(doc)) {
        gColorsPage = 1;
    }
    float zoom = RenderZoom(doc, gColorsPage, 1.0f);
    SetColors(MacDocColors::Normal);
    MacRenderedPage normal{};
    bool okNormal = MacRenderPage(doc, gColorsPage, zoom, 0, &normal);
    MacSetDocColors(&gColors);
    MacRenderedPage themed{};
    bool okThemed = MacRenderPage(doc, gColorsPage, zoom, 0, &themed);
    SetColors(MacDocColors::Normal);
    MacRenderedPage normal2{};
    MacRenderPage(doc, gColorsPage, zoom, 0, &normal2);
    MacSetDocColors(&gColors);
    // printing ignores the document colors
    MacRenderedPage print{};
    MacRenderPageForPrint(doc, gColorsPage, zoom, 0, &print);

    WritePpm(normal, "normal");
    WritePpm(themed, "themed");

    MacDocColorScheme current{};
    MacGetDocColors(&current);
    Key("colors");
    printf("{\"mode\":\"%s\",\"page\":%d,\"modeSet\":%s,\"ok\":%s,", ColorsModeName(gColors.mode), gColorsPage,
           current.mode == gColors.mode ? "true" : "false", okNormal && okThemed ? "true" : "false");
    PrintColorStats("normal", PageStats(normal, PageRegion{}));
    putchar(',');
    PrintColorStats("themed", PageStats(themed, PageRegion{}));
    putchar(',');
    PrintColorStats("normalRegion", PageStats(normal, region));
    putchar(',');
    PrintColorStats("themedRegion", PageStats(themed, region));
    printf(",\"diff\":%.2f,\"diffInverse\":%.2f,\"regionDiff\":%.2f,\"regionDiffInverse\":%.2f,\"normalRestored\":%s",
           MeanAbsDiff(themed, normal, PageRegion{}, false), MeanAbsDiff(themed, normal, PageRegion{}, true),
           MeanAbsDiff(themed, normal, region, false), MeanAbsDiff(themed, normal, region, true),
           SamePixels(normal, normal2) ? "true" : "false");
    printf(",\"printDiff\":%.2f", MeanAbsDiff(print, normal, PageRegion{}, false));
    if (okNormal && okThemed) {
        Stage("colors-epoch");
        CheckColorsEpoch(doc, zoom, themed, normal);
        Stage("colors-thumbnails");
        CheckColorsThumbnails(doc);
    }
    putchar('}');
    MacFreeRenderedPage(&normal);
    MacFreeRenderedPage(&normal2);
    MacFreeRenderedPage(&print);
    MacFreeRenderedPage(&themed);
}

static bool ParseRgb(const char* s, unsigned int* rgb) {
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (!s[0] || *end || v > 0xFFFFFF) {
        return false;
    }
    *rgb = (unsigned int)v;
    return true;
}

// Chaptered docs (EPUB) lay out the first chapter at open and count the rest in
// the background, so the page count grows. Wait until it's final, delivering the
// page-ready callbacks the app relayouts on.
static int SettlePageCount(void* doc) {
    Stage("settle");
    double start = NowMs();
    while (!MacPageCountIsFinal(doc) && NowMs() - start < gWaitMs) {
        SleepMs(10);
    }
    // the last change's page-ready task may still be queued
    SleepMs(kSettleMs);
    KeyBool("pageCountFinal", MacPageCountIsFinal(doc));
    KeyInt("pagesSettled", MacPageCount(doc));
    KeyInt("pageReadyCallbacksAtSettle", __atomic_load_n(&gPagesReady, __ATOMIC_SEQ_CST));
    KeyNum("settleMs", NowMs() - start);
    return MacPageCount(doc);
}

// every page once (up to kRenderAllMax), for fuzzing: damage often only shows on later pages
static void RenderAll(void* doc) {
    Stage("render-all");
    int n = MacPageCount(doc);
    int nOk = 0;
    int nInk = 0;
    for (int pageNo = 1; pageNo <= n && pageNo <= kRenderAllMax; pageNo++) {
        RenderStats st = RenderSync(doc, pageNo, 0.4f, (pageNo % 4) * 90);
        nOk += st.ok ? 1 : 0;
        nInk += st.inkRatio > 0 ? 1 : 0;
    }
    Key("renderAll");
    printf("{\"pages\":%d,\"ok\":%d,\"withInk\":%d}", n, nOk, nInk);
}

// many pending renders, a generation reset, a cancel, then close while the
// renderer is still busy
static void Stress(void* doc, int nPages, int nRequests) {
    Stage("stress");
    int n = nRequests < nPages ? nRequests : nPages;
    int nRequested = 0;
    double start = NowMs();
    for (int pageNo = 1; pageNo <= n; pageNo++, nRequested++) {
        MacRequestPage(doc, pageNo, 1.0f, 0, pageNo % 3);
    }
    MacResetRenderer(doc);
    for (int pageNo = n; pageNo >= 1; pageNo--, nRequested++) {
        MacRequestPage(doc, pageNo, 1.5f, 90, pageNo % 3);
    }
    MacCancelPendingRenders(doc);
    for (int pageNo = 1; pageNo <= n; pageNo += 7, nRequested++) {
        MacRequestPage(doc, pageNo, 2.0f, 0, 0);
    }
    KeyInt("stressRequested", nRequested);
    KeyNum("stressRequestMs", NowMs() - start);
}

// ASan / UBSan runtime's __sanitizer_print_stack_trace, when linked. Looked up at runtime:
// a weak undefined reference links on Linux but not with Apple's ld64.
using PrintStackFn = void (*)();
static PrintStackFn gPrintStack = nullptr;

// -fuzz: a stuck engine call gets its stack printed instead of just a timeout
static void OnWatchdog(int) {
    const char msg[] = "watchdog: no progress, stack of the stuck call:\n";
    write(2, msg, sizeof(msg) - 1);
    if (gPrintStack) {
        gPrintStack();
    }
    _exit(3);
}

// Without a sanitizer (e.g. the macOS CI debug build) print a raw stack on a crash, so
// test logs and fuzz reproducer notes say where it happened. With ASan, its report wins.
static void OnCrash(int sig) {
    const char msg[] = "crash: fatal signal, stack:\n";
    write(2, msg, sizeof(msg) - 1);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void Usage() {
    fprintf(stderr,
            "usage: test_mac_engine <file> [-password <p1[,p2...]>] [-search <word>] [-stress <nPages>] [-render-all] "
            "[-fuzz] [-colors normal|smart|invert [-colors-bg RRGGBB] [-colors-text RRGGBB] [-no-preserve-images] "
            "[-region x0,y0,x1,y1] [-colors-page <n>] [-colors-dump <prefix>]]\n");
}

int main(int argc, char** argv) {
    const char* path = nullptr;
    const char* search = nullptr;
    char* passwordList = nullptr;
    int stress = 0;
    bool renderAll = false;
    bool colors = false;
    PageRegion region;
    for (int i = 1; i < argc; i++) {
        bool hasArg = i + 1 < argc;
        if (!strcmp(argv[i], "-colors") && hasArg) {
            const char* mode = argv[++i];
            colors = true;
            if (!strcmp(mode, "smart")) {
                gColors.mode = MacDocColors::SmartDark;
            } else if (!strcmp(mode, "invert")) {
                gColors.mode = MacDocColors::Inverted;
            } else if (strcmp(mode, "normal") != 0) {
                Usage();
                return 2;
            }
        } else if ((!strcmp(argv[i], "-colors-bg") || !strcmp(argv[i], "-colors-text")) && hasArg) {
            bool bg = !strcmp(argv[i], "-colors-bg");
            if (!ParseRgb(argv[++i], bg ? &gColors.backgroundRgb : &gColors.textRgb)) {
                Usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "-colors-page") && hasArg) {
            gColorsPage = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-colors-dump") && hasArg) {
            gColorsDump = argv[++i];
        } else if (!strcmp(argv[i], "-no-preserve-images")) {
            gColors.preserveImages = false;
        } else if (!strcmp(argv[i], "-region") && hasArg) {
            if (sscanf(argv[++i], "%lf,%lf,%lf,%lf", &region.x0, &region.y0, &region.x1, &region.y1) != 4) {
                Usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "-password") && hasArg) {
            passwordList = strdup(argv[++i]);
        } else if (!strcmp(argv[i], "-search") && hasArg) {
            search = argv[++i];
        } else if (!strcmp(argv[i], "-stress") && hasArg) {
            stress = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-render-all")) {
            renderAll = true;
        } else if (!strcmp(argv[i], "-fuzz")) {
            // damaged input: render every page, short waits, coarse link scan
            renderAll = true;
            gWaitMs = kFuzzWaitMs;
            gFuzz = true;
            gLinkScanMaxCells = kFuzzLinkScanMaxCells;
            signal(SIGALRM, OnWatchdog);
            alarm(kFuzzWatchdogSecs);
        } else if (argv[i][0] != '-' && !path) {
            path = argv[i];
        } else {
            Usage();
            return 2;
        }
    }
    if (!path) {
        Usage();
        return 2;
    }

    PasswordState pwd;
    for (char* p = passwordList ? strtok(passwordList, ",") : nullptr; p && pwd.count < kMaxPasswords;
         p = strtok(nullptr, ",")) {
        pwd.passwords[pwd.count++] = p;
    }
    MacSetPasswordCallback(OnPassword, &pwd);
    uitask::Initialize();
    gPrintStack = (PrintStackFn)dlsym(RTLD_DEFAULT, "__sanitizer_print_stack_trace");
    if (!gPrintStack) {
        signal(SIGSEGV, OnCrash);
        signal(SIGBUS, OnCrash);
        signal(SIGILL, OnCrash);
        signal(SIGFPE, OnCrash);
        signal(SIGABRT, OnCrash);
    }

    // every check (and -fuzz's render of every page) runs with these colors
    MacSetDocColors(&gColors);

    putchar('{');
    KeyStr("file", path);
    Stage("open");
    double start = NowMs();
    char* error = nullptr;
    void* doc = MacOpenDocument(nullptr, path, OnPageReady, &gPagesReady, &error);
    KeyNum("openMs", NowMs() - start);
    KeyBool("open", doc != nullptr);
    KeyStr("error", error);
    KeyInt("passwordPrompts", pwd.prompts);
    free(error);

    if (doc) {
        KeyInt("pages", MacPageCount(doc));
        int nPages = SettlePageCount(doc);
        CheckPageSizes(doc, nPages);
        CheckLayout(doc, nPages);
        bool page1Renders = CheckRender(doc, nPages);
        CheckAsyncRender(doc, nPages, page1Renders);
        if (search) {
            CheckSearch(doc, nPages, search);
        }
        CheckSelectAll(doc, search);
        CheckToc(doc);
        CheckLinks(doc, nPages);
        CheckProperties(doc);
        if (colors) {
            CheckColors(doc, region);
        }
        if (renderAll) {
            RenderAll(doc);
        }
        if (stress > 0) {
            Stress(doc, nPages, stress);
        }
        KeyInt("pagesAtEnd", MacPageCount(doc));
        // informational: both are posted to the main run loop, which only the app runs
        KeyInt("pageReadyCallbacks", __atomic_load_n(&gPagesReady, __ATOMIC_SEQ_CST));
        KeyInt("findCallbacks", __atomic_load_n(&gFindCallbacks, __ATOMIC_SEQ_CST));
        Stage("close");
        start = NowMs();
        MacCloseDocument(doc);
        KeyNum("closeMs", NowMs() - start);
    }

    Stage("shutdown");
    MacSetPasswordCallback(nullptr, nullptr);
    MacShutdown();
    MacFinalize();
    free(passwordList);
    printf("\n}\n");
    fflush(stdout);
    Stage("done");
    return 0;
}
