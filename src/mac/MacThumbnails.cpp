/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Pixmap.h"

#include "gui/UIModels.h"
#include "EngineBase.h"
#include "mac/SumatraMacEngine.h"
#include "mac/MacThumbnails.h"

/*
Thumbnails are rendered on one worker thread with the document's own engine
(AddRef()ed while registered; engines support concurrent RenderPage the way the
Windows RenderCache threads use them). Results go into a byte-bounded LRU cache
and the owner is told via onReady, called on the worker thread.

  main thread                         worker thread
  MacThumbsRequest() --> requests --> pick best, RenderPage(abort cookie)
  MacThumbsGet() <------ cache <----- add, evict LRU, onReady(ctx, doc, page)
  MacThumbsForget() aborts and waits for an in-flight render of that document,
  then releases its engine, so MacCloseDocument() frees it on the main thread.

Cached pixels are refcounted blocks, so an image handed out keeps its pixels
after eviction or after the service is destroyed.

Like PageRenderService, the abort cookie is published by the engine without
our lock; we only read it under the lock to call Abort() and delete it after
RenderPage() returns.

Call everything but MacThumbsReleaseImage() from one thread (the main thread).
*/

static const long long kDefaultMaxCacheBytes = 64LL * 1024 * 1024;
static const int kMaxThumbDx = 4096;
static const float kMaxThumbZoom = 64.0f;

struct ThumbPixels {
    AtomicInt refs;
    int dx;
    int dy;
    int stride;
    // pixel rows follow the header
};

struct ThumbKey {
    int docId = 0;
    int pageNo = 0;
    int rotation = 0;
    int dx = 0;
    int dy = 0;
};

struct ThumbDoc {
    void* handle = nullptr;
    EngineBase* engine = nullptr;
    int id = 0;
    bool closing = false;
};

struct ThumbRequest {
    ThumbKey key;
    MacThumbPriority priority = MacThumbPriority::Visible;
    u64 serial = 0;
};

struct ThumbEntry {
    ThumbKey key;
    ThumbPixels* pixels = nullptr;
    i64 bytes = 0;
    u64 lastUse = 0;
};

struct ThumbService {
    Mutex mutex;
    ConditionVariable condition;
    ThreadHandle worker = nullptr;
    bool stopping = false;
    bool workerStopped = false;

    MacThumbReadyCallback onReady = nullptr;
    void* context = nullptr;

    Vec<ThumbDoc*> docs;
    int nextDocId = 0;
    Vec<ThumbRequest> requests;
    u64 serial = 0;

    Vec<ThumbEntry> cache;
    i64 cacheBytes = 0;
    i64 maxBytes = 0;
    u64 useSerial = 0;

    bool busy = false;
    ThumbKey activeKey;
    AbortCookie* activeCookie = nullptr;
};

static bool SameKey(const ThumbKey& a, const ThumbKey& b) {
    return a.docId == b.docId && a.pageNo == b.pageNo && a.rotation == b.rotation && a.dx == b.dx && a.dy == b.dy;
}

static int NormRotation(int rotation) {
    rotation = ((rotation % 360) + 360) % 360;
    return ((rotation + 45) / 90 % 4) * 90;
}

static u8* PixelRows(ThumbPixels* pixels) {
    return (u8*)(pixels + 1);
}

static ThumbPixels* AllocThumbPixels(int dx, int dy) {
    if (dx <= 0 || dy <= 0 || dx > kMaxThumbDx || dy > kMaxThumbDx) {
        return nullptr;
    }
    int stride = dx * 4;
    size_t nBytes = sizeof(ThumbPixels) + (size_t)stride * (size_t)dy;
    auto* pixels = (ThumbPixels*)malloc(nBytes);
    if (!pixels) {
        return nullptr;
    }
    pixels->refs = 1;
    pixels->dx = dx;
    pixels->dy = dy;
    pixels->stride = stride;
    return pixels;
}

static void ReleasePixels(ThumbPixels* pixels) {
    if (pixels && AtomicIntDec(&pixels->refs) == 0) {
        free(pixels);
    }
}

static u8 Premultiply(u8 c, u8 a) {
    return (u8)(((int)c * (int)a + 127) / 255);
}

// Converts an engine pixmap to premultiplied BGRA; nullptr for layouts we can't read.
static ThumbPixels* PixelsFromPixmap(const Pixmap* pixmap) {
    if (!pixmap || !pixmap->data) {
        return nullptr;
    }
    PixmapFormat format = pixmap->format;
    if (format != PixmapFormat::BGRA8 && format != PixmapFormat::BGR8 && format != PixmapFormat::RGBA8) {
        return nullptr;
    }
    ThumbPixels* pixels = AllocThumbPixels(pixmap->width, pixmap->height);
    if (!pixels) {
        return nullptr;
    }

    bool premultiplied = format == PixmapFormat::BGRA8 && pixmap->premultiplied;
    for (int y = 0; y < pixels->dy; y++) {
        const u8* src = pixmap->data + (size_t)y * (size_t)pixmap->stride;
        u8* dst = PixelRows(pixels) + (size_t)y * (size_t)pixels->stride;
        for (int x = 0; x < pixels->dx; x++) {
            u8 b = src[0], g = src[1], r = src[2], a = 255;
            if (format == PixmapFormat::RGBA8) {
                b = src[2];
                r = src[0];
            }
            if (format != PixmapFormat::BGR8) {
                a = src[3];
            }
            if (!premultiplied && a != 255) {
                b = Premultiply(b, a);
                g = Premultiply(g, a);
                r = Premultiply(r, a);
            }
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = a;
            src += format == PixmapFormat::BGR8 ? 3 : 4;
            dst += 4;
        }
    }
    return pixels;
}

static ThumbService* AsService(void* thumbs) {
    return (ThumbService*)thumbs;
}

static ThumbDoc* FindDocByHandle(ThumbService* s, void* handle) {
    for (ThumbDoc* doc : s->docs) {
        if (doc->handle == handle) {
            return doc;
        }
    }
    return nullptr;
}

static ThumbDoc* FindDocById(ThumbService* s, int id) {
    for (ThumbDoc* doc : s->docs) {
        if (doc->id == id) {
            return doc;
        }
    }
    return nullptr;
}

static int FindCached(ThumbService* s, const ThumbKey& key) {
    for (int i = 0; i < len(s->cache); i++) {
        if (SameKey(s->cache[i].key, key)) {
            return i;
        }
    }
    return -1;
}

static void RemoveCacheEntry(ThumbService* s, int idx) {
    s->cacheBytes -= s->cache[idx].bytes;
    ReleasePixels(s->cache[idx].pixels);
    VecRemoveAt(s->cache, idx);
}

// Evicts least recently used entries (never keepIdx) until the cache fits the budget.
static void TrimCache(ThumbService* s, int keepIdx) {
    while (s->cacheBytes > s->maxBytes && len(s->cache) > 1) {
        int evict = -1;
        for (int i = 0; i < len(s->cache); i++) {
            if (i == keepIdx) {
                continue;
            }
            if (evict < 0 || s->cache[i].lastUse < s->cache[evict].lastUse) {
                evict = i;
            }
        }
        if (evict < 0) {
            return;
        }
        RemoveCacheEntry(s, evict);
        if (evict < keepIdx) {
            keepIdx--;
        }
    }
}

static void AddToCache(ThumbService* s, const ThumbKey& key, ThumbPixels* pixels) {
    int old = FindCached(s, key);
    if (old >= 0) {
        RemoveCacheEntry(s, old);
    }
    ThumbEntry entry;
    entry.key = key;
    entry.pixels = pixels;
    entry.bytes = (i64)pixels->stride * (i64)pixels->dy;
    entry.lastUse = ++s->useSerial;
    VecAppend(s->cache, entry);
    s->cacheBytes += entry.bytes;
    TrimCache(s, len(s->cache) - 1);
}

static void RemoveDocCache(ThumbService* s, int docId) {
    for (int i = len(s->cache) - 1; i >= 0; i--) {
        if (s->cache[i].key.docId == docId) {
            RemoveCacheEntry(s, i);
        }
    }
}

static void RemoveDocRequests(ThumbService* s, int docId) {
    for (int i = len(s->requests) - 1; i >= 0; i--) {
        if (s->requests[i].key.docId == docId) {
            VecRemoveAt(s->requests, i);
        }
    }
}

// Visible before prefetch, then oldest first so a screenful fills top to bottom.
static int PickRequest(ThumbService* s) {
    int best = -1;
    for (int i = 0; i < len(s->requests); i++) {
        const ThumbRequest& r = s->requests[i];
        if (best < 0) {
            best = i;
            continue;
        }
        const ThumbRequest& b = s->requests[best];
        if (r.priority < b.priority || (r.priority == b.priority && r.serial < b.serial)) {
            best = i;
        }
    }
    return best;
}

// Zoom that fits the rotated page into dx * dy pixels.
static float ThumbZoom(EngineBase* engine, const ThumbKey& key) {
    RectF mediabox = engine->PageMediabox(key.pageNo);
    float pageDx = mediabox.dx;
    float pageDy = mediabox.dy;
    if (key.rotation == 90 || key.rotation == 270) {
        std::swap(pageDx, pageDy);
    }
    if (pageDx <= 0 || pageDy <= 0) {
        return 0;
    }
    float zoom = std::min((float)key.dx / pageDx, (float)key.dy / pageDy);
    if (!(zoom > 0)) {
        return 0;
    }
    // tiny pages (e.g. 1x1 px comic images) get a smaller thumbnail instead of none
    return std::min(zoom, kMaxThumbZoom);
}

static ThumbPixels* RenderThumb(ThumbService* s, EngineBase* engine, const ThumbKey& key) {
    if (key.pageNo < 1 || key.pageNo > engine->PageCount()) {
        return nullptr;
    }
    float zoom = ThumbZoom(engine, key);
    if (zoom <= 0) {
        return nullptr;
    }
    RenderPageArgs args(key.pageNo, zoom, key.rotation, nullptr, RenderTarget::View, &s->activeCookie);
    Pixmap* pixmap = engine->RenderPage(args);
    ThumbPixels* pixels = PixelsFromPixmap(pixmap);
    FreePixmap(pixmap);
    return pixels;
}

static void ThumbWorker(ThumbService* s) {
    s->mutex.Lock();
    for (;;) {
        while (!s->stopping && len(s->requests) == 0) {
            s->condition.Wait(&s->mutex);
        }
        if (s->stopping) {
            break;
        }

        int idx = PickRequest(s);
        ThumbKey key = s->requests[idx].key;
        VecRemoveAt(s->requests, idx);
        ThumbDoc* doc = FindDocById(s, key.docId);
        if (!doc || doc->closing || FindCached(s, key) >= 0) {
            continue;
        }

        // Forget() waits while busy is set for this document, so doc and its engine stay valid
        s->busy = true;
        s->activeKey = key;
        s->activeCookie = nullptr;
        EngineBase* engine = doc->engine;
        s->mutex.Unlock();

        ThumbPixels* pixels = RenderThumb(s, engine, key);

        s->mutex.Lock();
        AbortCookie* cookie = s->activeCookie;
        s->activeCookie = nullptr;
        s->busy = false;
        s->condition.WakeAll();

        doc = FindDocById(s, key.docId);
        void* handle = nullptr;
        if (pixels && doc && !doc->closing && !s->stopping) {
            AddToCache(s, key, pixels);
            pixels = nullptr;
            handle = doc->handle;
        }
        s->mutex.Unlock();

        delete cookie;
        ReleasePixels(pixels);
        ResetTempArena();
        if (handle && s->onReady) {
            s->onReady(s->context, handle, key.pageNo);
        }
        s->mutex.Lock();
    }
    s->workerStopped = true;
    s->condition.WakeAll();
    s->mutex.Unlock();
}

// Aborts an in-flight render of docId and waits for it. Called with the mutex held.
static void WaitForDocRender(ThumbService* s, int docId) {
    if (!s->busy || s->activeKey.docId != docId) {
        return;
    }
    if (s->activeCookie) {
        s->activeCookie->Abort();
    }
    while (s->busy && s->activeKey.docId == docId) {
        s->condition.Wait(&s->mutex);
    }
}

// Drops everything about doc: queued requests, in-flight render, cache, engine ref.
static void ForgetDoc(ThumbService* s, ThumbDoc* doc) {
    EngineBase* engine = nullptr;
    {
        AutoUnlockMutex lock(&s->mutex);
        doc->closing = true;
        RemoveDocRequests(s, doc->id);
        WaitForDocRender(s, doc->id);
        RemoveDocCache(s, doc->id);
        VecRemove(s->docs, doc);
        engine = doc->engine;
    }
    delete doc;
    if (engine) {
        engine->Release();
    }
}

// Registered record for document, (re)created if the handle now names a
// different engine (a closed document's address reused without MacThumbsForget).
static ThumbDoc* GetDoc(ThumbService* s, void* document) {
    EngineBase* engine = MacDocumentEngine(document);
    if (!engine) {
        return nullptr;
    }
    s->mutex.Lock();
    ThumbDoc* doc = FindDocByHandle(s, document);
    s->mutex.Unlock();
    if (doc && doc->engine == engine) {
        return doc;
    }
    if (doc) {
        ForgetDoc(s, doc);
    }

    doc = new ThumbDoc();
    doc->handle = document;
    doc->engine = engine;
    engine->AddRef();
    AutoUnlockMutex lock(&s->mutex);
    doc->id = ++s->nextDocId;
    VecAppend(s->docs, doc);
    return doc;
}

// Existing record for document, or nullptr. Doesn't register.
static ThumbDoc* PeekDoc(ThumbService* s, void* document) {
    EngineBase* engine = MacDocumentEngine(document);
    AutoUnlockMutex lock(&s->mutex);
    ThumbDoc* doc = FindDocByHandle(s, document);
    return doc && doc->engine == engine && !doc->closing ? doc : nullptr;
}

// Creates the thumbnail service. onReady(context, document, pageNo) runs on the
// worker thread after a thumbnail was cached; hop to the main thread there.
void* MacThumbsCreate(MacThumbReadyCallback onReady, void* context, long long maxCacheBytes) {
    auto* s = new ThumbService();
    s->onReady = onReady;
    s->context = context;
    s->maxBytes = maxCacheBytes > 0 ? maxCacheBytes : kDefaultMaxCacheBytes;
    return s;
}

// Stops the worker (aborting its render), then frees the cache and engine refs.
// No onReady call happens after this returns.
void MacThumbsDestroy(void* thumbs) {
    ThumbService* s = AsService(thumbs);
    if (!s) {
        return;
    }
    s->mutex.Lock();
    s->stopping = true;
    VecReset(s->requests);
    if (s->activeCookie) {
        s->activeCookie->Abort();
    }
    s->condition.WakeAll();
    while (s->worker && !s->workerStopped) {
        s->condition.Wait(&s->mutex);
    }
    s->mutex.Unlock();
    SafeCloseThreadHandle(&s->worker);

    while (len(s->cache) > 0) {
        RemoveCacheEntry(s, len(s->cache) - 1);
    }
    for (ThumbDoc* doc : s->docs) {
        doc->engine->Release();
        delete doc;
    }
    VecReset(s->docs);
    delete s;
}

// Queues a render of pageNo fitting dx * dy pixels (the caller's backing pixels).
// A newer request for the same page replaces an older one. No-op if cached.
void MacThumbsRequest(void* thumbs, void* document, int pageNo, int rotation, int dx, int dy,
                      MacThumbPriority priority) {
    ThumbService* s = AsService(thumbs);
    if (!s || !document || pageNo < 1 || dx <= 0 || dy <= 0) {
        return;
    }
    ThumbDoc* doc = GetDoc(s, document);
    if (!doc) {
        return;
    }

    ThumbKey key;
    key.docId = doc->id;
    key.pageNo = pageNo;
    key.rotation = NormRotation(rotation);
    key.dx = std::min(dx, kMaxThumbDx);
    key.dy = std::min(dy, kMaxThumbDx);

    AutoUnlockMutex lock(&s->mutex);
    if (s->stopping || doc->closing || FindCached(s, key) >= 0 || (s->busy && SameKey(s->activeKey, key))) {
        return;
    }
    ThumbRequest request;
    request.key = key;
    request.priority = priority;
    request.serial = ++s->serial;
    bool replaced = false;
    for (ThumbRequest& r : s->requests) {
        if (r.key.docId == key.docId && r.key.pageNo == key.pageNo) {
            r = request;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        VecAppend(s->requests, request);
    }
    if (!s->worker && !s->workerStopped) {
        s->worker = StartThread(MkFunc0(ThumbWorker, s), StrL("mac-thumbnails"));
    }
    s->condition.Wake();
}

// Drops queued requests except those for document at rotation within
// [firstPage, lastPage]. A null document drops all. Doesn't touch an in-flight render.
void MacThumbsPrune(void* thumbs, void* document, int rotation, int firstPage, int lastPage) {
    ThumbService* s = AsService(thumbs);
    if (!s) {
        return;
    }
    ThumbDoc* doc = document ? PeekDoc(s, document) : nullptr;
    int docId = doc ? doc->id : 0;
    rotation = NormRotation(rotation);
    AutoUnlockMutex lock(&s->mutex);
    for (int i = len(s->requests) - 1; i >= 0; i--) {
        const ThumbKey& key = s->requests[i].key;
        bool keep = key.docId == docId && key.rotation == rotation && key.pageNo >= firstPage && key.pageNo <= lastPage;
        if (!keep) {
            VecRemoveAt(s->requests, i);
        }
    }
}

// Best cached thumbnail of pageNo at rotation: the exact dx * dy one if cached
// (image->exact), else the one closest in width. Release with MacThumbsReleaseImage().
bool MacThumbsGet(void* thumbs, void* document, int pageNo, int rotation, int dx, int dy, MacThumbImage* image) {
    if (!image) {
        return false;
    }
    *image = {};
    ThumbService* s = AsService(thumbs);
    if (!s || !document) {
        return false;
    }
    ThumbDoc* doc = PeekDoc(s, document);
    if (!doc) {
        return false;
    }
    rotation = NormRotation(rotation);
    dx = std::min(dx, kMaxThumbDx);
    dy = std::min(dy, kMaxThumbDx);

    AutoUnlockMutex lock(&s->mutex);
    int best = -1;
    int bestDiff = 0;
    for (int i = 0; i < len(s->cache); i++) {
        const ThumbKey& key = s->cache[i].key;
        if (key.docId != doc->id || key.pageNo != pageNo || key.rotation != rotation) {
            continue;
        }
        int diff = key.dx == dx && key.dy == dy ? -1 : std::abs(key.dx - dx);
        if (best < 0 || diff < bestDiff) {
            best = i;
            bestDiff = diff;
        }
    }
    if (best < 0) {
        return false;
    }
    ThumbEntry& entry = s->cache[best];
    entry.lastUse = ++s->useSerial;
    ThumbPixels* pixels = entry.pixels;
    AtomicIntInc(&pixels->refs);
    image->width = pixels->dx;
    image->height = pixels->dy;
    image->stride = pixels->stride;
    image->exact = bestDiff < 0;
    image->data = PixelRows(pixels);
    image->ref = pixels;
    return true;
}

// Releases pixels from MacThumbsGet(). Safe on any thread, also after MacThumbsDestroy().
void MacThumbsReleaseImage(void* ref) {
    ReleasePixels((ThumbPixels*)ref);
}

// Call before closing document: drops its requests and cache, aborts and waits
// for its in-flight render and releases the engine reference.
void MacThumbsForget(void* thumbs, void* document) {
    ThumbService* s = AsService(thumbs);
    if (!s || !document) {
        return;
    }
    s->mutex.Lock();
    ThumbDoc* doc = FindDocByHandle(s, document);
    s->mutex.Unlock();
    if (doc) {
        ForgetDoc(s, doc);
    }
}

long long MacThumbsCacheBytes(void* thumbs) {
    ThumbService* s = AsService(thumbs);
    if (!s) {
        return 0;
    }
    AutoUnlockMutex lock(&s->mutex);
    return s->cacheBytes;
}

// Queued plus in-flight renders.
int MacThumbsPendingCount(void* thumbs) {
    ThumbService* s = AsService(thumbs);
    if (!s) {
        return 0;
    }
    AutoUnlockMutex lock(&s->mutex);
    return len(s->requests) + (s->busy ? 1 : 0);
}
