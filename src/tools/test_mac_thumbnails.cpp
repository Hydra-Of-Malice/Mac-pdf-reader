/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Lifetime / threading test for src/mac/MacThumbnails.cpp (the macOS sidebar's
// background thumbnail service). Portable: builds on macOS and Linux.
//   test_mac_thumbnails                  fake engines, then tests/issue-1201.cbz tests/issue-6132.pdf
//   test_mac_thumbnails <document> ...   fake engines, then the given documents
// Build with -DMAC_THUMBS_TEST_FAKE_ONLY=1 to link without the document engines
// (base + EngineBase.cpp + ChapterTable.cpp + MacThumbnails.cpp are enough).

#include "base/Base.h"
#include "base/Pixmap.h"

#include "gui/UIModels.h"
#include "EngineBase.h"
#include "mac/SumatraMacEngine.h"
#include "mac/MacThumbnails.h"

#ifndef MAC_THUMBS_TEST_FAKE_ONLY
#define MAC_THUMBS_TEST_FAKE_ONLY 0
#endif

#if !MAC_THUMBS_TEST_FAKE_ONLY
#include "base/GuessFileType.h"
#include "EngineAll.h"
#endif

void _uploadDebugReport(Str, Str, bool) {}

void log(Str s) {
    if (len(s) == 0) {
        return;
    }
    fwrite(s.s, 1, (size_t)s.len, stderr);
}

struct EBookUI;
EBookUI* GetEBookUI() {
    return nullptr;
}

struct FileEBookUI;
FileEBookUI* GetFileEBookUI(Str) {
    return nullptr;
}

// Stand-in for a SumatraMacEngine document: MacThumbnails only needs its engine.
struct FakeDoc {
    EngineBase* engine = nullptr;
};

EngineBase* MacDocumentEngine(void* document) {
    return document ? ((FakeDoc*)document)->engine : nullptr;
}

static const int kTimeoutMs = 30000;
static const int kThumbDx = 120;
static const int kThumbDy = 160;

static int gFailures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            gFailures++;                                           \
        }                                                          \
    } while (0)

//--- fake engine

static AtomicInt gFakeLive = 0;
static AtomicInt gFakeRenders = 0;
static AtomicInt gFakeAborts = 0;

struct FakeCookie : AbortCookie {
    AtomicInt aborted = 0;
    void Abort() override { AtomicIntSet(&aborted, 1); }
    void* GetData() override { return (void*)&aborted; }
};

// Renders a pattern (page number in blue) after renderMs, honoring the abort cookie.
class FakeEngine : public EngineBase {
  public:
    int renderMs = 0;
    PixmapFormat format = PixmapFormat::BGRA8;
    bool premultiplied = true;
    u8 alpha = 255;
    AtomicInt rendering = 0;

    FakeEngine(int nPages, int renderMs) {
        pageCount = nPages;
        this->renderMs = renderMs;
        AtomicIntInc(&gFakeLive);
    }

    EngineBase* Clone() override { return nullptr; }

    // odd pages portrait, even pages landscape
    RectF PageMediabox(int pageNo) override { return pageNo % 2 ? RectF(0, 0, 600, 800) : RectF(0, 0, 800, 600); }

    Pixmap* RenderPage(RenderPageArgs& args) override {
        AtomicIntInc(&gFakeRenders);
        AtomicIntInc(&rendering);
        FakeCookie* cookie = nullptr;
        if (args.cookie_out) {
            cookie = new FakeCookie();
            *args.cookie_out = cookie;
        }
        for (int t = 0; t < renderMs; t += 2) {
            if (cookie && AtomicIntGet(&cookie->aborted)) {
                AtomicIntInc(&gFakeAborts);
                AtomicIntDec(&rendering);
                return nullptr;
            }
            SleepInMs(2);
        }
        RectF mb = PageMediabox(args.pageNo);
        float dx = mb.dx * args.zoom;
        float dy = mb.dy * args.zoom;
        if (args.rotation == 90 || args.rotation == 270) {
            std::swap(dx, dy);
        }
        Pixmap* pixmap = AllocPixmap((int)(dx + 0.5f), (int)(dy + 0.5f), format, premultiplied);
        for (int y = 0; pixmap && y < pixmap->height; y++) {
            u8* px = pixmap->data + (size_t)y * (size_t)pixmap->stride;
            for (int x = 0; x < pixmap->width; x++) {
                u8 blue = (u8)args.pageNo;
                u8 red = (u8)(x + y);
                if (format == PixmapFormat::RGBA8) {
                    px[0] = red;
                    px[2] = blue;
                } else {
                    px[0] = blue;
                    px[2] = red;
                }
                px[1] = 7;
                if (format != PixmapFormat::BGR8) {
                    px[3] = alpha;
                }
                px += format == PixmapFormat::BGR8 ? 3 : 4;
            }
        }
        AtomicIntDec(&rendering);
        return pixmap;
    }

    RectF Transform(const RectF& rect, int, float, int, bool) override { return rect; }
    Str GetFileData() override { return {}; }
    bool SaveFileAs(Str) override { return false; }
    bool HasClipOptimizations(int) override { return false; }
    TempStr GetPropertyTemp(DocProp) override { return {}; }
    Vec<IPageElement*> GetElements(int) override { return Vec<IPageElement*>(); }
    IPageElement* GetElementAtPos(int, PointF) override { return nullptr; }
    bool BenchLoadPage(int) override { return true; }

  protected:
    ~FakeEngine() override { AtomicIntDec(&gFakeLive); }
};

static EngineBase* OpenFake(Str) {
    return new FakeEngine(7, 3);
}

//--- recording onReady

struct Recorder {
    Mutex mutex;
    int count = 0;
    Vec<void*> docs;
    Vec<int> pages;
};

static void OnThumbReady(void* context, void* document, int pageNo) {
    auto* rec = (Recorder*)context;
    AutoUnlockMutex lock(&rec->mutex);
    rec->count++;
    VecAppend(rec->docs, document);
    VecAppend(rec->pages, pageNo);
}

static int RecordedCount(Recorder* rec) {
    AutoUnlockMutex lock(&rec->mutex);
    return rec->count;
}

// Position of the first onReady for doc / pageNo, or -1.
static int ReadyIndex(Recorder* rec, void* doc, int pageNo) {
    AutoUnlockMutex lock(&rec->mutex);
    for (int i = 0; i < len(rec->pages); i++) {
        if (rec->docs[i] == doc && rec->pages[i] == pageNo) {
            return i;
        }
    }
    return -1;
}

static bool WaitFor(void* thumbs, Recorder* rec, void* doc, int firstPage, int lastPage) {
    for (int waited = 0; waited < kTimeoutMs; waited += 2) {
        bool all = true;
        for (int p = firstPage; p <= lastPage && all; p++) {
            all = ReadyIndex(rec, doc, p) >= 0;
        }
        if (all) {
            return true;
        }
        SleepInMs(2);
    }
    printf("timeout waiting for pages %d-%d (pending %d)\n", firstPage, lastPage, MacThumbsPendingCount(thumbs));
    return false;
}

static void WaitIdle(void* thumbs) {
    for (int waited = 0; waited < kTimeoutMs && MacThumbsPendingCount(thumbs) > 0; waited += 2) {
        SleepInMs(2);
    }
}

// Opaque, within the box and touching it on one side, and not a single color.
static bool CheckImage(const MacThumbImage& img, int dx, int dy) {
    if (!img.data || img.width <= 0 || img.height <= 0 || img.stride < img.width * 4) {
        return false;
    }
    if (img.width > dx + 1 || img.height > dy + 1 || (img.width < dx - 1 && img.height < dy - 1)) {
        printf("  unexpected size %dx%d for box %dx%d\n", img.width, img.height, dx, dy);
        return false;
    }
    bool varied = false;
    const unsigned char* first = img.data;
    for (int y = 0; y < img.height; y++) {
        const unsigned char* row = img.data + (size_t)y * (size_t)img.stride;
        for (int x = 0; x < img.width; x++) {
            const unsigned char* px = row + (x * 4);
            if (px[3] != 255) {
                return false;
            }
            if (px[0] != first[0] || px[1] != first[1] || px[2] != first[2]) {
                varied = true;
            }
        }
    }
    return varied;
}

//--- tests for any engine

using OpenFn = EngineBase* (*)(Str path);

// Renders, caches and hands out thumbnails; rotation swaps the aspect ratio.
static void TestBasic(OpenFn open, Str path) {
    FakeDoc doc;
    doc.engine = open(path);
    CHECK(doc.engine);
    if (!doc.engine) {
        return;
    }
    int lastPage = std::min(doc.engine->PageCount(), 4);

    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 0);
    for (int p = 1; p <= lastPage; p++) {
        MacThumbsRequest(thumbs, &doc, p, 0, kThumbDx, kThumbDy, MacThumbPriority::Visible);
    }
    CHECK(WaitFor(thumbs, &rec, &doc, 1, lastPage));
    WaitIdle(thumbs);
    for (int p = 1; p <= lastPage; p++) {
        MacThumbImage img = {};
        CHECK(MacThumbsGet(thumbs, &doc, p, 0, kThumbDx, kThumbDy, &img));
        CHECK(img.exact);
        CHECK(CheckImage(img, kThumbDx, kThumbDy));
        MacThumbsReleaseImage(img.ref);
    }
    CHECK(MacThumbsCacheBytes(thumbs) > 0);

    // a cached request is a no-op: no new render, no new callback
    int before = RecordedCount(&rec);
    MacThumbsRequest(thumbs, &doc, 1, 0, kThumbDx, kThumbDy, MacThumbPriority::Visible);
    CHECK(MacThumbsPendingCount(thumbs) == 0);
    WaitIdle(thumbs);
    CHECK(RecordedCount(&rec) == before);

    // other size: the closest cached one, marked inexact
    MacThumbImage approx = {};
    CHECK(MacThumbsGet(thumbs, &doc, 1, 0, kThumbDx * 2, kThumbDy * 2, &approx));
    CHECK(!approx.exact);
    MacThumbsReleaseImage(approx.ref);

    // rotation is part of the key and swaps the rendered aspect ratio
    MacThumbImage upright = {};
    CHECK(MacThumbsGet(thumbs, &doc, 1, 0, kThumbDx, kThumbDy, &upright));
    MacThumbImage none = {};
    CHECK(!MacThumbsGet(thumbs, &doc, 1, 90, kThumbDy, kThumbDx, &none));
    MacThumbsRequest(thumbs, &doc, 1, 90, kThumbDy, kThumbDx, MacThumbPriority::Visible);
    WaitIdle(thumbs);
    MacThumbImage rotated = {};
    CHECK(MacThumbsGet(thumbs, &doc, 1, 90, kThumbDy, kThumbDx, &rotated));
    CHECK(rotated.exact);
    if (upright.data && rotated.data && upright.width != upright.height) {
        CHECK((upright.height > upright.width) != (rotated.height > rotated.width));
    }
    MacThumbsReleaseImage(upright.ref);
    MacThumbsReleaseImage(rotated.ref);

    MacThumbsForget(thumbs, &doc);
    MacThumbImage gone = {};
    CHECK(!MacThumbsGet(thumbs, &doc, 1, 0, kThumbDx, kThumbDy, &gone));
    CHECK(MacThumbsCacheBytes(thumbs) == 0);
    CHECK(AtomicIntGet(&doc.engine->refCount) == 1);
    MacThumbsDestroy(thumbs);
    doc.engine->Release();
}

// The byte budget holds, and an image handed out survives its eviction.
static void TestCacheBound(OpenFn open, Str path) {
    FakeDoc doc;
    doc.engine = open(path);
    CHECK(doc.engine);
    if (!doc.engine) {
        return;
    }
    const long long budget = 2LL * kThumbDx * kThumbDy * 4;
    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, budget);

    MacThumbsRequest(thumbs, &doc, 1, 0, kThumbDx, kThumbDy, MacThumbPriority::Visible);
    CHECK(WaitFor(thumbs, &rec, &doc, 1, 1));
    MacThumbImage held = {};
    CHECK(MacThumbsGet(thumbs, &doc, 1, 0, kThumbDx, kThumbDy, &held));

    // distinct sizes make distinct cache entries
    for (int i = 1; i <= 8; i++) {
        int pageNo = 1 + (i % doc.engine->PageCount());
        MacThumbsRequest(thumbs, &doc, pageNo, 0, kThumbDx - (i * 4), kThumbDy - (i * 4), MacThumbPriority::Visible);
        WaitIdle(thumbs);
        CHECK(MacThumbsCacheBytes(thumbs) <= budget);
    }
    CHECK(held.data && CheckImage(held, kThumbDx, kThumbDy));
    MacThumbsDestroy(thumbs);
    // still readable after eviction and after the service is gone
    CHECK(held.data && CheckImage(held, kThumbDx, kThumbDy));
    MacThumbsReleaseImage(held.ref);
    doc.engine->Release();
}

// Pruning drops queued requests outside the kept range.
static void TestPrune(OpenFn open, Str path) {
    FakeDoc doc;
    doc.engine = open(path);
    CHECK(doc.engine);
    if (!doc.engine) {
        return;
    }
    int pageCount = doc.engine->PageCount();
    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 0);
    for (int i = 0; i < 40; i++) {
        int p = 1 + (i % pageCount);
        MacThumbsRequest(thumbs, &doc, p, 0, kThumbDx + i, kThumbDy + i, MacThumbPriority::Prefetch);
    }
    MacThumbsPrune(thumbs, &doc, 0, 1, 1);
    CHECK(MacThumbsPendingCount(thumbs) <= 2); // page 1 plus one in-flight render
    MacThumbsPrune(thumbs, nullptr, 0, 1, 0);
    CHECK(MacThumbsPendingCount(thumbs) <= 1);
    WaitIdle(thumbs);
    MacThumbsDestroy(thumbs);
    doc.engine->Release();
}

// Closing documents and the service while renders are queued and in flight.
static void TestCloseWhilePending(OpenFn open, Str path) {
    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 0);

    // documentWillClose: forget, then close
    for (int round = 0; round < 5; round++) {
        FakeDoc doc;
        doc.engine = open(path);
        CHECK(doc.engine);
        if (!doc.engine) {
            break;
        }
        for (int p = 1; p <= doc.engine->PageCount(); p++) {
            MacThumbsRequest(thumbs, &doc, p, 0, 600, 800, MacThumbPriority::Visible);
        }
        SleepInMs(round * 3);
        MacThumbsForget(thumbs, &doc);
        CHECK(MacThumbsPendingCount(thumbs) == 0);
        CHECK(AtomicIntGet(&doc.engine->refCount) == 1);
        doc.engine->Release();
    }

    // a caller that closes without MacThumbsForget: our engine ref keeps it alive
    FakeDoc doc;
    doc.engine = open(path);
    CHECK(doc.engine);
    if (doc.engine) {
        MacThumbsRequest(thumbs, &doc, 1, 0, kThumbDx, kThumbDy, MacThumbPriority::Visible);
        MacThumbsRequest(thumbs, &doc, 1 + (1 % doc.engine->PageCount()), 0, kThumbDx, kThumbDy,
                         MacThumbPriority::Visible);
        doc.engine->Release();
        CHECK(WaitFor(thumbs, &rec, &doc, 1, 1));
        WaitIdle(thumbs);
        MacThumbsForget(thumbs, &doc);
    }

    // a handle reused for another document is detected by its engine
    FakeDoc reused;
    reused.engine = open(path);
    if (reused.engine) {
        MacThumbsRequest(thumbs, &reused, 1, 0, kThumbDx, kThumbDy, MacThumbPriority::Visible);
        WaitIdle(thumbs);
        EngineBase* old = reused.engine;
        reused.engine = open(path);
        old->Release();
        MacThumbImage stale = {};
        CHECK(!MacThumbsGet(thumbs, &reused, 1, 0, kThumbDx, kThumbDy, &stale));
        MacThumbsRequest(thumbs, &reused, 1, 0, kThumbDx, kThumbDy, MacThumbPriority::Visible);
        WaitIdle(thumbs);
        MacThumbImage fresh = {};
        CHECK(MacThumbsGet(thumbs, &reused, 1, 0, kThumbDx, kThumbDy, &fresh));
        MacThumbsReleaseImage(fresh.ref);
        MacThumbsForget(thumbs, &reused);
        reused.engine->Release();
    }

    // destroy with work queued: no callback after it returns
    FakeDoc last;
    last.engine = open(path);
    if (last.engine) {
        for (int p = 1; p <= last.engine->PageCount(); p++) {
            MacThumbsRequest(thumbs, &last, p, 0, 500, 700, MacThumbPriority::Visible);
        }
    }
    MacThumbsDestroy(thumbs);
    int after = RecordedCount(&rec);
    SleepInMs(100);
    CHECK(RecordedCount(&rec) == after);
    if (last.engine) {
        CHECK(AtomicIntGet(&last.engine->refCount) == 1);
        last.engine->Release();
    }
}

static void RunEngineTests(OpenFn open, Str path) {
    printf("%.*s\n", len(path), path.s);
    TestBasic(open, path);
    TestCacheBound(open, path);
    TestPrune(open, path);
    TestCloseWhilePending(open, path);
    ResetTempArena();
}

//--- fake engine only

// Engine pixel layouts come out as premultiplied BGRA.
static void TestPixelFormats() {
    printf("fake: pixel formats\n");
    struct Case {
        PixmapFormat format;
        bool premultiplied;
        u8 alpha;
    };
    Case cases[] = {
        {PixmapFormat::BGRA8, true, 255},
        {PixmapFormat::BGR8, false, 255},
        {PixmapFormat::RGBA8, false, 255},
        {PixmapFormat::BGRA8, false, 128},
    };
    for (const Case& c : cases) {
        auto* engine = new FakeEngine(1, 0);
        engine->format = c.format;
        engine->premultiplied = c.premultiplied;
        engine->alpha = c.alpha;
        FakeDoc doc;
        doc.engine = engine;
        Recorder rec;
        void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 0);
        MacThumbsRequest(thumbs, &doc, 1, 0, 30, 40, MacThumbPriority::Visible);
        CHECK(WaitFor(thumbs, &rec, &doc, 1, 1));
        MacThumbImage img = {};
        CHECK(MacThumbsGet(thumbs, &doc, 1, 0, 30, 40, &img));
        CHECK(img.width == 30 && img.height == 40);
        if (img.data) {
            const unsigned char* px = img.data + (size_t)5 * (size_t)img.stride + (3 * 4);
            int a = c.alpha;
            CHECK(px[3] == a);
            CHECK(px[0] == (1 * a + 127) / 255);       // blue: page 1
            CHECK(px[1] == (7 * a + 127) / 255);       // green
            CHECK(px[2] == ((3 + 5) * a + 127) / 255); // red: x + y
        }
        MacThumbsReleaseImage(img.ref);
        MacThumbsDestroy(thumbs);
        engine->Release();
    }
}

// Visible requests are served before prefetch ones.
static void TestPriority() {
    printf("fake: priority\n");
    auto* engine = new FakeEngine(10, 20);
    FakeDoc doc;
    doc.engine = engine;
    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 0);
    for (int p = 1; p <= 5; p++) {
        MacThumbsRequest(thumbs, &doc, p, 0, 20, 20, MacThumbPriority::Prefetch);
    }
    MacThumbsRequest(thumbs, &doc, 9, 0, 20, 20, MacThumbPriority::Visible);
    CHECK(WaitFor(thumbs, &rec, &doc, 1, 5));
    CHECK(WaitFor(thumbs, &rec, &doc, 9, 9));
    int visible = ReadyIndex(&rec, &doc, 9);
    for (int p = 2; p <= 5; p++) {
        CHECK(visible < ReadyIndex(&rec, &doc, p));
    }
    MacThumbsDestroy(thumbs);
    engine->Release();
}

// Forget aborts an in-flight render, waits for it and never delivers it.
static void TestForgetAbortsRender() {
    printf("fake: forget aborts render\n");
    auto* engine = new FakeEngine(3, 2000);
    FakeDoc doc;
    doc.engine = engine;
    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 0);
    int abortsBefore = AtomicIntGet(&gFakeAborts);
    MacThumbsRequest(thumbs, &doc, 1, 0, 50, 50, MacThumbPriority::Visible);
    MacThumbsRequest(thumbs, &doc, 2, 0, 50, 50, MacThumbPriority::Visible);
    for (int waited = 0; waited < kTimeoutMs && AtomicIntGet(&engine->rendering) == 0; waited += 1) {
        SleepInMs(1);
    }
    SleepInMs(20); // let the engine hand out its cookie

    int rendersBefore = AtomicIntGet(&gFakeRenders);
    MacThumbsForget(thumbs, &doc);
    CHECK(AtomicIntGet(&engine->rendering) == 0);
    CHECK(AtomicIntGet(&gFakeAborts) == abortsBefore + 1);
    CHECK(AtomicIntGet(&engine->refCount) == 1);
    CHECK(MacThumbsPendingCount(thumbs) == 0);
    SleepInMs(50);
    CHECK(AtomicIntGet(&gFakeRenders) == rendersBefore); // page 2 was dropped, not rendered
    CHECK(RecordedCount(&rec) == 0);
    MacThumbsDestroy(thumbs);
    engine->Release();
}

// Destroy aborts the in-flight render instead of waiting it out.
static void TestDestroyAbortsRender() {
    printf("fake: destroy aborts render\n");
    auto* engine = new FakeEngine(3, 5000);
    FakeDoc doc;
    doc.engine = engine;
    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 0);
    MacThumbsRequest(thumbs, &doc, 1, 0, 50, 50, MacThumbPriority::Visible);
    for (int waited = 0; waited < kTimeoutMs && AtomicIntGet(&engine->rendering) == 0; waited += 1) {
        SleepInMs(1);
    }
    SleepInMs(20);
    MacThumbsDestroy(thumbs);
    CHECK(AtomicIntGet(&engine->rendering) == 0);
    CHECK(AtomicIntGet(&engine->refCount) == 1);
    CHECK(RecordedCount(&rec) == 0);
    engine->Release();
}

// Many documents, interleaved requests / prunes / forgets from the caller thread.
static void TestStress() {
    printf("fake: stress\n");
    const int kDocs = 4;
    Recorder rec;
    void* thumbs = MacThumbsCreate(OnThumbReady, &rec, 256 * 1024);
    FakeDoc docs[kDocs];
    for (int round = 0; round < 300; round++) {
        FakeDoc& doc = docs[round % kDocs];
        if (!doc.engine) {
            doc.engine = new FakeEngine(1 + (round % 13), round % 3);
        }
        int pageCount = doc.engine->PageCount();
        for (int i = 0; i < 6; i++) {
            int pageNo = 1 + ((round + i) % pageCount);
            int size = 16 + ((round * 7 + i) % 64);
            auto priority = i % 2 ? MacThumbPriority::Prefetch : MacThumbPriority::Visible;
            MacThumbsRequest(thumbs, &doc, pageNo, (i % 4) * 90, size, size, priority);
        }
        MacThumbImage img = {};
        if (MacThumbsGet(thumbs, &doc, 1, 0, 32, 32, &img)) {
            CHECK(img.data && img.width > 0);
            MacThumbsReleaseImage(img.ref);
        }
        if (round % 5 == 0) {
            MacThumbsPrune(thumbs, &doc, 0, 1, 2);
        }
        if (round % 7 == 0) {
            MacThumbsForget(thumbs, &doc);
            CHECK(AtomicIntGet(&doc.engine->refCount) == 1);
            doc.engine->Release();
            doc.engine = nullptr;
        }
        if (round % 11 == 0) {
            SleepInMs(1);
        }
    }
    for (FakeDoc& doc : docs) {
        if (doc.engine) {
            MacThumbsForget(thumbs, &doc);
            doc.engine->Release();
        }
    }
    CHECK(MacThumbsCacheBytes(thumbs) == 0);
    MacThumbsDestroy(thumbs);
}

int main(int argc, char** argv) {
    int liveBefore = AtomicIntGet(&gFakeLive);
    RunEngineTests(OpenFake, StrL("fake engine"));
    TestPixelFormats();
    TestPriority();
    TestForgetAbortsRender();
    TestDestroyAbortsRender();
    TestStress();
    CHECK(AtomicIntGet(&gFakeLive) == liveBefore); // every engine released

#if !MAC_THUMBS_TEST_FAKE_ONLY
    auto openDoc = [](Str path) -> EngineBase* {
        FileType kind = GuessFileTypeFromName(path);
        if (IsEngineCbxSupportedFileType(kind)) {
            return CreateEngineCbxFromFile(path, nullptr, kind);
        }
        if (IsEngineImageSupportedFileType(kind)) {
            return CreateEngineImageFromFile(path);
        }
        if (IsEngineDjVuSupportedFileType(kind)) {
            return CreateEngineDjvuDecFromFile(path);
        }
        if (IsEngineMupdfSupportedFileType(kind)) {
            return CreateEngineMupdfFromFile(path, kind, 96, nullptr);
        }
        return (EngineBase*)nullptr;
    };
    StrVec paths;
    for (int i = 1; i < argc; i++) {
        paths.Append(Str(argv[i]));
    }
    if (len(paths) == 0) {
        paths.Append(StrL("tests/issue-1201.cbz"));
        paths.Append(StrL("tests/issue-6132.pdf"));
    }
    for (int i = 0; i < len(paths); i++) {
        RunEngineTests(openDoc, paths[i]);
    }
#else
    (void)argc;
    (void)argv;
#endif

    DestroyTempArena();
    if (gFailures > 0) {
        printf("test_mac_thumbnails: %d failure(s)\n", gFailures);
        return 1;
    }
    printf("test_mac_thumbnails: ok\n");
    return 0;
}
