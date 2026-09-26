/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Headless driver for the macOS engine bridge (src/mac/SumatraMacEngine.h). Uses the bridge
// exactly like SumatraMac.mm (plain C API, no base/Base.h) and prints one JSON object per run.
// Driven by tests/mac/run-engine-tests.ts; runs on macOS and on Linux (-mac-core builds).
//
// usage: test_mac_engine <file> [-password <p1[,p2...]>] [-search <word>] [-stress <nPages>]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mac/SumatraMacEngine.h"

constexpr int kMaxPasswords = 4;
constexpr int kAsyncPages = 8;
constexpr int kLinkScanPages = 3;
constexpr int kLinkScanStep = 4;
constexpr int kMaxLinks = 16;
constexpr int kWaitMs = 30000;
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

static double NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void SleepMs(int ms) {
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

static RenderStats RenderSync(void* doc, int pageNo, float zoom, int rotation) {
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
    for (int i = 0; ok && i < layout.pageCount; i++) {
        shown += layout.pages[i].shown ? 1 : 0;
    }
    Key("layout");
    printf("{\"ok\":%s,\"pageCount\":%d,\"canvas\":[%d,%d],\"shown\":%d,\"renderZoom\":%.3f}", ok ? "true" : "false",
           layout.pageCount, layout.canvasWidth, layout.canvasHeight, shown, ok ? layout.pages[0].renderZoom : 0.0);
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

static void CheckRender(void* doc, int nPages) {
    Stage("render-sync");
    KeyRender("render1", RenderSync(doc, 1, 1.0f, 0));
    RenderStats rotated = RenderSync(doc, 1, 0.5f, 90);
    KeyRender("render1Rotated", rotated);
    if (nPages > 1) {
        KeyRender("renderLast", RenderSync(doc, nPages, 1.0f, 0));
    }
    MacRenderedPage page{};
    KeyBool("renderOutOfRangeFails", !MacRenderPage(doc, nPages + 1, 1.0f, 0, &page));
    MacFreeRenderedPage(&page);
}

// request pages through the async PageRenderService and wait for them like the app does
static void CheckAsyncRender(void* doc, int nPages) {
    Stage("render-async");
    int n = nPages < kAsyncPages ? nPages : kAsyncPages;
    const float zoom = 0.75f;
    for (int pageNo = 1; pageNo <= n; pageNo++) {
        MacRequestPage(doc, pageNo, zoom, 0, pageNo == 1 ? 0 : (pageNo < 4 ? 1 : 2));
    }
    bool got[kAsyncPages + 1] = {};
    int nGot = 0;
    RenderStats first;
    double start = NowMs();
    while (nGot < n && NowMs() - start < kWaitMs) {
        for (int pageNo = 1; pageNo <= n; pageNo++) {
            if (got[pageNo]) {
                continue;
            }
            MacRenderedPage page{};
            if (MacCopyRenderedPage(doc, pageNo, zoom, 0, &page)) {
                got[pageNo] = true;
                nGot++;
                if (pageNo == 1) {
                    first = Stats(true, page);
                }
            }
            MacFreeRenderedPage(&page);
        }
        if (nGot < n) {
            SleepMs(5);
        }
    }
    Key("async");
    printf("{\"requested\":%d,\"copied\":%d,\"callbacks\":%d,\"ms\":%.1f,\"page1Ink\":%.4f}", n, nGot,
           __atomic_load_n(&gPagesReady, __ATOMIC_SEQ_CST), NowMs() - start, first.inkRatio);

    // a new generation drops queued requests; requesting again must still work
    MacResetRenderer(doc);
    MacRequestPage(doc, 1, 0.5f, 0, 0);
    bool again = false;
    start = NowMs();
    while (!again && NowMs() - start < kWaitMs) {
        MacRenderedPage page{};
        again = MacCopyRenderedPage(doc, 1, 0.5f, 0, &page);
        MacFreeRenderedPage(&page);
        if (!again) {
            SleepMs(5);
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
    while (MacFindIsBusy(doc) && NowMs() - start < kWaitMs) {
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

static void CheckToc(void* doc, int nPages) {
    Stage("toc");
    int n = MacTocItemCount(doc);
    int badPages = 0;
    Key("toc");
    printf("{\"count\":%d,\"items\":[", n);
    for (int i = 0; i < n; i++) {
        char* title = MacCopyTocItemTitle(doc, i);
        int pageNo = MacTocItemPage(doc, i);
        if (pageNo < 0 || pageNo > nPages) {
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
        for (int y = 1; y < (int)h; y += kLinkScanStep) {
            for (int x = 1; x < (int)w; x += kLinkScanStep) {
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

static void Usage() {
    fprintf(stderr, "usage: test_mac_engine <file> [-password <p1[,p2...]>] [-search <word>] [-stress <nPages>]\n");
}

int main(int argc, char** argv) {
    const char* path = nullptr;
    const char* search = nullptr;
    char* passwordList = nullptr;
    int stress = 0;
    for (int i = 1; i < argc; i++) {
        bool hasArg = i + 1 < argc;
        if (!strcmp(argv[i], "-password") && hasArg) {
            passwordList = strdup(argv[++i]);
        } else if (!strcmp(argv[i], "-search") && hasArg) {
            search = argv[++i];
        } else if (!strcmp(argv[i], "-stress") && hasArg) {
            stress = atoi(argv[++i]);
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
        int nPages = MacPageCount(doc);
        KeyInt("pages", nPages);
        CheckPageSizes(doc, nPages);
        CheckLayout(doc, nPages);
        CheckRender(doc, nPages);
        CheckAsyncRender(doc, nPages);
        if (search) {
            CheckSearch(doc, nPages, search);
        }
        CheckSelectAll(doc, search);
        CheckToc(doc, nPages);
        CheckLinks(doc, nPages);
        CheckProperties(doc);
        if (stress > 0) {
            Stress(doc, nPages, stress);
        }
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
