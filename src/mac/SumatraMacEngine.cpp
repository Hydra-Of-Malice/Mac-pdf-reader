#include "base/Base.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/Pixmap.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocumentLayout.h"
#include "DocProperties.h"
#include "gui/UIModels.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "PageRenderPolicy.h"
#include "PageRenderService.h"
#include "ProgressUpdateUI.h"
#include "ReaderModel.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "gui/PlatformWindow.h"
#include "gui/PasswordDialog.h"
#include "mac/SumatraMacEngine.h"

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

static char* DupCString(Str s) {
    char* res = (char*)malloc((size_t)len(s) + 1);
    if (res) {
        memcpy(res, s.s, (size_t)len(s));
        res[len(s)] = 0;
    }
    return res;
}

static char* DupCString(const char* s) {
    return DupCString(Str((char*)s));
}

struct MacFindWorker;
struct MacLayoutNotify;
struct MacTextWorker;

struct MacDocument {
    ReaderModel* model = nullptr;
    PageRenderService* renderer = nullptr;
    TextSelection* textSelection = nullptr;
    TextSearch* textSearch = nullptr;
    MacFindWorker* find = nullptr;
    MacLayoutNotify* layoutNotify = nullptr;
    MacTextWorker* text = nullptr;
    TocTree* toc = nullptr;
    Vec<TocItem*> tocItems;
    Vec<int> tocDepths;
    Props properties;
    bool propertiesLoaded = false;
    MacPageReadyCallback onPageReady = nullptr;
    void* callbackContext = nullptr;
};

static MacDocument* AsDocument(void* document) {
    return (MacDocument*)document;
}

static void StopFindWorker(MacDocument* doc);
static void StopTextWorker(MacDocument* doc);

static void OnPageReady(MacDocument* document) {
    if (document->onPageReady) {
        document->onPageReady(document->callbackContext);
    }
}

// Chaptered (reflowable) engines lay chapters out lazily: after open the page
// count grows and later page numbers shift. Engine callbacks come from any
// thread and can outlive the document, so they go through this object, freed
// once both the document and the engine let go of it.
struct MacLayoutNotify {
    AtomicInt refs = 2; // the document's and the engine's
    Mutex mutex;
    MacDocument* doc = nullptr; // null once the document is closed
};

static void ReleaseLayoutNotify(MacLayoutNotify* n) {
    if (AtomicIntDec(&n->refs) == 0) {
        delete n;
    }
}

// Main thread (only it clears n->doc). Page-ready makes the app re-query
// MacPageCount() and relayout.
static void RunLayoutChangedTask(MacLayoutNotify* n) {
    if (n->doc) {
        OnPageReady(n->doc);
    }
    ReleaseLayoutNotify(n);
}

// Any thread: cached renders are keyed by page numbers that may have shifted.
static void OnEngineLayoutChanged(MacLayoutNotify* n) {
    AutoUnlockMutex lock(&n->mutex);
    if (!n->doc) {
        return;
    }
    if (n->doc->renderer) {
        n->doc->renderer->NewGeneration();
    }
    AtomicIntInc(&n->refs);
    PlatformPostTask(MkFunc0(RunLayoutChangedTask, n));
}

// Main thread: publish the page counts the background pass found; that fires
// OnEngineLayoutChanged.
static void RunPublishChaptersTask(MacLayoutNotify* n) {
    if (n->doc) {
        n->doc->model->GetEngine()->PublishWarmedChapters();
    }
    ReleaseLayoutNotify(n);
}

static void OnChapterLayoutProgress(MacLayoutNotify* n, ChapterLayoutProgress* p) {
    if (!p || !p->finished) {
        return;
    }
    AutoUnlockMutex lock(&n->mutex);
    if (!n->doc) {
        return;
    }
    AtomicIntInc(&n->refs);
    PlatformPostTask(MkFunc0(RunPublishChaptersTask, n));
}

static void OnLayoutEngineDestroyed(MacLayoutNotify* n, EngineBase*) {
    ReleaseLayoutNotify(n);
}

// Counts the remaining chapters in the background (like Windows) so the page
// total converges soon after open.
static void WatchChapterLayout(MacDocument* doc) {
    EngineBase* engine = doc->model->GetEngine();
    auto* n = new MacLayoutNotify();
    n->doc = doc;
    doc->layoutNotify = n;
    engine->SetOnLayoutChanged(MkFunc0(OnEngineLayoutChanged, n));
    engine->SetOnChapterLayoutProgress(MkFunc1(OnChapterLayoutProgress, n));
    engine->SetOnDestroy(MkFunc1(OnLayoutEngineDestroyed, n));
    engine->StartBackgroundChapterLayout();
}

static void UnwatchChapterLayout(MacDocument* doc) {
    MacLayoutNotify* n = doc->layoutNotify;
    if (!n) {
        return;
    }
    doc->model->GetEngine()->CancelBackgroundChapterLayout();
    {
        AutoUnlockMutex lock(&n->mutex);
        n->doc = nullptr;
    }
    doc->layoutNotify = nullptr;
    ReleaseLayoutNotify(n);
}

static void AppendTocItems(MacDocument* document, TocItem* item, int depth) {
    while (item) {
        VecAppend(document->tocItems, item);
        VecAppend(document->tocDepths, depth);
        AppendTocItems(document, item->child, depth + 1);
        item = item->next;
    }
}

static void LoadProperties(MacDocument* document) {
    if (document->propertiesLoaded) {
        return;
    }
    document->propertiesLoaded = true;
    // GetProperties() gives temp-arena values: keep owned copies (freed by FreeProps)
    Props props;
    document->model->GetEngine()->GetProperties(props);
    for (const PropValue& p : props) {
        AddPropOwned(document->properties, p.prop, p.val);
    }
}

// Target page of a link / ToC destination, 0 if none. Chaptered (EPUB) engines
// resolve destinations lazily, so dest->pageNo alone is often unset.
static int DestPageNo(MacDocument* document, IPageDestination* dest) {
    EngineBase* engine = document->model->GetEngine();
    Location loc = engine->ResolveDest(dest);
    if (!loc.IsValid()) {
        return 0;
    }
    return engine->PageNoFromLocation(loc);
}

static PointF ToPagePoint(MacDocument* document, int pageNo, double x, double y, double zoom, int rotation) {
    PointF point((float)x, (float)y);
    return document->model->GetEngine()->Transform(point, pageNo, (float)zoom, rotation, true);
}

static int ResultRectCount(const TextSel& result, int pageNo) {
    int count = 0;
    for (int i = 0; i < result.len; i++) {
        if (result.pages[i] == pageNo) {
            count++;
        }
    }
    return count;
}

static bool TransformResultRect(MacDocument* document, const TextSel& result, int pageNo, int index, double zoom,
                                int rotation, MacDisplayRect* rect) {
    if (!rect || index < 0) {
        return false;
    }
    for (int i = 0; i < result.len; i++) {
        if (result.pages[i] != pageNo) {
            continue;
        }
        if (index-- != 0) {
            continue;
        }
        RectF transformed =
            document->model->GetEngine()->Transform(ToRectF(result.rects[i]), pageNo, (float)zoom, rotation);
        rect->x = transformed.x;
        rect->y = transformed.y;
        rect->width = transformed.dx;
        rect->height = transformed.dy;
        return true;
    }
    return false;
}

static bool CopyPixmap(Pixmap* pixmap, MacRenderedPage* page) {
    if (!pixmap || !pixmap->data) {
        return false;
    }

    if (pixmap->format == PixmapFormat::BGR8) {
        size_t stride = (((size_t)pixmap->width * 4) + 3) & ~(size_t)3;
        size_t nBytes = stride * (size_t)pixmap->height;
        auto* data = (unsigned char*)malloc(nBytes);
        if (!data) {
            return false;
        }
        for (int y = 0; y < pixmap->height; y++) {
            const unsigned char* src = pixmap->data + ((size_t)y * (size_t)pixmap->stride);
            unsigned char* dst = data + ((size_t)y * stride);
            for (int x = 0; x < pixmap->width; x++) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = 255;
                src += 3;
                dst += 4;
            }
        }
        page->width = pixmap->width;
        page->height = pixmap->height;
        page->stride = (int)stride;
        page->premultiplied = true;
        page->data = data;
        return true;
    }

    if (pixmap->format != PixmapFormat::BGRA8) {
        return false;
    }

    size_t nBytes = (size_t)pixmap->stride * (size_t)pixmap->height;
    auto* data = (unsigned char*)malloc(nBytes);
    if (!data) {
        return false;
    }

    memcpy(data, pixmap->data, nBytes);
    page->width = pixmap->width;
    page->height = pixmap->height;
    page->stride = pixmap->stride;
    page->premultiplied = pixmap->premultiplied;
    page->data = data;
    return true;
}

static MacPasswordCallback gPasswordCallback = nullptr;
static void* gPasswordContext = nullptr;

// Non-interactive PasswordUI: asks callback (gPasswordCallback unless the
// open passed its own); a null answer cancels.
struct CallbackPasswordUI : PasswordUI {
    MacPasswordCallback callback = nullptr;
    void* context = nullptr;
    int attempt = 0;

    Str GetPassword(Str filePath, u8*, u8[32], bool* saveKey) override {
        *saveKey = false;
        TempStr name = path::GetBaseNameTemp(filePath);
        const char* pwd = callback(context, CStrTemp(name), ++attempt);
        return pwd ? str::Dup(Str((char*)pwd)) : Str{};
    }
};

// When set, MacOpenDocument gets passwords from callback instead of a dialog
// (headless tests, automation). Pass nullptr to restore the dialog.
void MacSetPasswordCallback(MacPasswordCallback callback, void* context) {
    gPasswordCallback = callback;
    gPasswordContext = context;
}

// Pages are rendered whole, so cap a render's size (the cache must fit one)
constexpr i64 kMacRenderCacheBytes = 256LL * 1024 * 1024;
constexpr double kMacMaxRenderPixels = 32.0 * 1024 * 1024;

// DialogPasswordUI that tells the user when a prompt follows a rejected password.
struct MacDialogPasswordUI : DialogPasswordUI {
    int prompts = 0;

    explicit MacDialogPasswordUI(NativeWnd parent) : DialogPasswordUI(parent) {}

    Str GetPassword(Str filePath, u8*, u8[32], bool* saveKey) override {
        *saveKey = false;
        PasswordDialogArgs args;
        args.parent = parent;
        args.fileName = path::GetBaseNameTemp(filePath);
        args.canRemember = canRemember;
        args.showPassword = showPassword;
        args.isRetry = prompts > 0;
        prompts++;

        PasswordDialogResult result;
        ShowPasswordDialog(args, &result);
        showPassword = result.showPassword;
        if (!result.accepted) {
            str::Free(result.password);
            return {};
        }
        *saveKey = result.rememberPassword;
        return result.password;
    }
};

static MacOpenError ClassifyOpenFailure(Str path, bool prompted);

enum class ChapterLayout {
    Lazy, // chapters are counted in the background; the page count grows after opening
    Full, // all chapters now (off the main thread): page numbers are final when the document shows
};

// Engines keep prompting until the password is right or the prompt is
// cancelled, so a failure after a prompt means the user gave up.
static MacDocument* OpenDocumentImpl(void* passwordParent, const char* path, MacPageReadyCallback onPageReady,
                                     void* callbackContext, MacOpenError* errorOut,
                                     MacPasswordCallback askPassword = nullptr, void* passwordContext = nullptr,
                                     ChapterLayout chapters = ChapterLayout::Lazy) {
    *errorOut = MacOpenError::None;
    if (!path || !path[0]) {
        *errorOut = MacOpenError::NotFound;
        return nullptr;
    }

    Str filePath((char*)path);
    MacDialogPasswordUI dialogUI((NativeWnd)passwordParent);
    CallbackPasswordUI callbackUI;
    callbackUI.callback = askPassword ? askPassword : gPasswordCallback;
    callbackUI.context = askPassword ? passwordContext : gPasswordContext;
    PasswordUI* pwdUI = callbackUI.callback ? (PasswordUI*)&callbackUI : &dialogUI;
    ReaderModel* model = ReaderModel::Create(filePath, pwdUI);
    if (!model) {
        bool prompted = dialogUI.prompts > 0 || callbackUI.attempt > 0;
        *errorOut = ClassifyOpenFailure(filePath, prompted);
        return nullptr;
    }
    if (chapters == ChapterLayout::Full) {
        model->GetEngine()->EnsureAllChaptersLaidOut();
    }
    auto* document = new MacDocument();
    document->model = model;
    document->textSelection = new TextSelection(model->GetEngine());
    document->textSearch = new TextSearch(model->GetEngine());
    document->toc = model->GetEngine()->GetToc();
    if (document->toc && document->toc->root) {
        AppendTocItems(document, document->toc->root->child, 0);
    }
    document->onPageReady = onPageReady;
    document->callbackContext = callbackContext;
    // a render copy would paginate lazily laid out chapters on its own, so they'd disagree on page numbers
    bool chaptered = model->GetEngine()->HasChapters();
    PageRenderEngine use = chaptered ? PageRenderEngine::Shared : PageRenderEngine::Clone;
    document->renderer =
        PageRenderService::Create(model->GetEngine(), MkFunc0(OnPageReady, document), kMacRenderCacheBytes, use);
    if (!document->renderer) {
        // the engine owns the ToC tree
        delete document->textSelection;
        delete document->textSearch;
        delete model;
        delete document;
        *errorOut = MacOpenError::RendererFailed;
        return nullptr;
    }
    if (chaptered) {
        WatchChapterLayout(document);
    }
    return document;
}

// Opens a document. Returns an opaque handle, or nullptr on failure; on failure
// *errorOut (if non-null) is set to a malloc'd message the caller must free().
void* MacOpenDocument(void* passwordParent, const char* path, MacPageReadyCallback onPageReady, void* callbackContext,
                      char** errorOut) {
    MacOpenError err = MacOpenError::None;
    MacDocument* document = OpenDocumentImpl(passwordParent, path, onPageReady, callbackContext, &err);
    if (document || !errorOut) {
        return document;
    }
    const char* msg = "Could not open the document.";
    if (!path || !path[0]) {
        msg = "Pass a document path on the command line.";
    } else if (err == MacOpenError::RendererFailed) {
        msg = "Could not start the page renderer.";
    }
    *errorOut = DupCString(msg);
    return nullptr;
}

// Number of pages, or 0 if the handle is invalid.
int MacPageCount(void* document) {
    if (!document) {
        return 0;
    }
    return AsDocument(document)->model->PageCount();
}

// False while a chaptered doc (EPUB) still lays chapters out: MacPageCount()
// grows and a page-ready callback follows each change.
bool MacPageCountIsFinal(void* document) {
    if (!document) {
        return true;
    }
    EngineBase* engine = AsDocument(document)->model->GetEngine();
    return !engine->HasChapters() || engine->ChaptersLaidOut() == engine->ChapterCount();
}

// Mediabox size of pageNo (1-based) in points. Returns false if invalid.
bool MacPageSize(void* document, int pageNo, double* widthOut, double* heightOut) {
    if (!document) {
        return false;
    }
    ReaderModel* model = AsDocument(document)->model;
    if (pageNo < 1 || pageNo > model->PageCount()) {
        return false;
    }
    RectF mb = model->PageMediabox(pageNo);
    if (widthOut) {
        *widthOut = mb.dx;
    }
    if (heightOut) {
        *heightOut = mb.dy;
    }
    return true;
}

double MacFileDPI(void* document) {
    if (!document) {
        return 96.0;
    }
    return AsDocument(document)->model->FileDPI();
}

// Pages are rendered whole: at most kMacMaxRenderPixels per page.
static double CapRenderZoom(double zoom, RectF mediaBox) {
    double area = (double)mediaBox.dx * (double)mediaBox.dy;
    if (area > 0 && zoom * zoom * area > kMacMaxRenderPixels) {
        return sqrt(kMacMaxRenderPixels / area);
    }
    return zoom;
}

bool MacLayoutDocument(void* document, const MacLayoutParams* params, MacDocumentLayout* layout) {
    if (!document || !params || !layout) {
        return false;
    }
    *layout = {};

    ReaderModel* model = AsDocument(document)->model;
    int pageCount = model->PageCount();
    if (pageCount <= 0) {
        return false;
    }

    DocumentLayoutParams p;
    p.displayMode = params->continuous ? DisplayMode::Continuous : DisplayMode::SinglePage;
    p.startPage = params->startPage;
    p.viewPortSize = Size(params->viewWidth, params->viewHeight);
    p.viewPortOffset = Point(params->viewX, params->viewY);
    p.zoomVirtual = (float)params->zoomVirtual;
    p.dpiFactor = 72.0f / model->FileDPI();
    p.rotation = params->rotation;
    p.windowMargin = {12, 12, 12, 12};
    p.pageSpacing = Size(0, 14);
    DocumentLayout docLayout;
    if (!model->Layout(p, &docLayout)) {
        return false;
    }

    auto* pages = (MacLayoutPage*)malloc(sizeof(MacLayoutPage) * (size_t)pageCount);
    if (!pages) {
        return false;
    }
    memset(pages, 0, sizeof(MacLayoutPage) * (size_t)pageCount);
    for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
        const DocumentLayoutPage* page = docLayout.GetPage(pageNo);
        MacLayoutPage* dst = &pages[pageNo - 1];
        dst->pageNo = pageNo;
        dst->x = page->pos.x;
        dst->y = page->pos.y;
        dst->width = page->pos.dx;
        dst->height = page->pos.dy;
        dst->screenX = page->pageOnScreen.x;
        dst->screenY = page->pageOnScreen.y;
        dst->screenWidth = page->pageOnScreen.dx;
        dst->screenHeight = page->pageOnScreen.dy;
        dst->visibleRatio = page->visibleRatio;
        dst->layoutZoom = page->zoomReal;
        dst->renderZoom = CapRenderZoom(page->zoomReal * params->backingScale, page->mediaBox);
        dst->shown = page->isShown;
    }

    // pages not laid out (single page mode) get the current page's zoom for
    // prefetching, capped for their own size
    int current = docLayout.CurrentPageNo();
    const DocumentLayoutPage* currentPage = current >= 1 && current <= pageCount ? docLayout.GetPage(current) : nullptr;
    double prefetchZoom = currentPage ? currentPage->zoomReal * params->backingScale : 0;
    for (int pageNo = 1; pageNo <= pageCount && prefetchZoom > 0; pageNo++) {
        if (!pages[pageNo - 1].shown) {
            pages[pageNo - 1].renderZoom = CapRenderZoom(prefetchZoom, docLayout.GetPage(pageNo)->mediaBox);
        }
    }

    layout->pageCount = pageCount;
    layout->currentPage = current;
    layout->canvasWidth = docLayout.canvasSize.dx;
    layout->canvasHeight = docLayout.canvasSize.dy;
    layout->pages = pages;
    return true;
}

// Renders pageNo (1-based) at the given zoom and rotation (0/90/180/270).
// Fills *page (caller frees with MacFreeRenderedPage); returns false on failure.
bool MacRenderPage(void* document, int pageNo, float zoom, int rotation, MacRenderedPage* page) {
    if (!page) {
        return false;
    }
    *page = {};
    if (!document) {
        return false;
    }
    ReaderModel* model = AsDocument(document)->model;
    if (pageNo < 1 || pageNo > model->PageCount()) {
        return false;
    }
    if (zoom <= 0) {
        zoom = 1.0f;
    }

    Pixmap* pixmap = model->RenderPage(pageNo, zoom, rotation);
    bool ok = CopyPixmap(pixmap, page);
    FreePixmap(pixmap);
    return ok;
}

void MacRequestPage(void* document, int pageNo, float zoom, int rotation, int priority) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->renderer || pageNo < 1 || pageNo > doc->model->PageCount()) {
        return;
    }
    PageRenderPriority renderPriority = PageRenderPriority::Background;
    if (priority <= 0) {
        renderPriority = PageRenderPriority::Visible;
    } else if (priority == 1) {
        renderPriority = PageRenderPriority::Nearby;
    }
    doc->renderer->Request({pageNo, zoom, rotation}, renderPriority);
}

bool MacCopyRenderedPage(void* document, int pageNo, float zoom, int rotation, MacRenderedPage* page) {
    if (!page) {
        return false;
    }
    *page = {};
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->renderer) {
        return false;
    }
    Pixmap* pixmap = doc->renderer->CopyPage({pageNo, zoom, rotation});
    bool ok = CopyPixmap(pixmap, page);
    FreePixmap(pixmap);
    return ok;
}

void MacResetRenderer(void* document) {
    MacDocument* doc = AsDocument(document);
    if (doc && doc->renderer) {
        doc->renderer->NewGeneration();
    }
}

bool MacTextAtPoint(void* document, int pageNo, double x, double y, double zoom, int rotation) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSelection || pageNo < 1 || pageNo > doc->model->PageCount()) {
        return false;
    }
    PointF point = ToPagePoint(doc, pageNo, x, y, zoom, rotation);
    return doc->textSelection->IsOverGlyph(pageNo, point.x, point.y);
}

bool MacStartSelection(void* document, int pageNo, double x, double y, double zoom, int rotation) {
    MacDocument* doc = AsDocument(document);
    if (!MacTextAtPoint(document, pageNo, x, y, zoom, rotation)) {
        return false;
    }
    PointF point = ToPagePoint(doc, pageNo, x, y, zoom, rotation);
    doc->textSelection->Reset();
    doc->textSelection->StartAt(pageNo, point.x, point.y);
    return true;
}

bool MacUpdateSelection(void* document, int pageNo, double x, double y, double zoom, int rotation) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSelection || doc->textSelection->startPage < 1 || pageNo < 1 ||
        pageNo > doc->model->PageCount()) {
        return false;
    }
    PointF point = ToPagePoint(doc, pageNo, x, y, zoom, rotation);
    doc->textSelection->SelectUpTo(pageNo, point.x, point.y);
    return true;
}

void MacSelectAll(void* document) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSelection) {
        return;
    }
    doc->textSelection->Reset();
    doc->textSelection->StartAt(1, 0);
    doc->textSelection->SelectUpTo(doc->model->PageCount(), -1);
}

bool MacHasSelection(void* document) {
    MacDocument* doc = AsDocument(document);
    return doc && doc->textSelection && doc->textSelection->result.len > 0;
}

int MacSelectionRectCount(void* document, int pageNo) {
    MacDocument* doc = AsDocument(document);
    return doc && doc->textSelection ? ResultRectCount(doc->textSelection->result, pageNo) : 0;
}

bool MacSelectionRect(void* document, int pageNo, int index, double zoom, int rotation, MacDisplayRect* rect) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSelection) {
        return false;
    }
    return TransformResultRect(doc, doc->textSelection->result, pageNo, index, zoom, rotation, rect);
}

// All selection rectangles on pageNo in one pass (MacSelectionRect() scans the
// whole selection per call). Free *rectsOut with free().
int MacCopySelectionRects(void* document, int pageNo, double zoom, int rotation, MacDisplayRect** rectsOut) {
    *rectsOut = nullptr;
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSelection) {
        return 0;
    }
    const TextSel& result = doc->textSelection->result;
    int count = ResultRectCount(result, pageNo);
    if (count == 0) {
        return 0;
    }
    auto* rects = (MacDisplayRect*)malloc(sizeof(MacDisplayRect) * (size_t)count);
    if (!rects) {
        return 0;
    }
    EngineBase* engine = doc->model->GetEngine();
    int n = 0;
    for (int i = 0; i < result.len && n < count; i++) {
        if (result.pages[i] != pageNo) {
            continue;
        }
        RectF r = engine->Transform(ToRectF(result.rects[i]), pageNo, (float)zoom, rotation);
        rects[n++] = {r.x, r.y, r.dx, r.dy};
    }
    *rectsOut = rects;
    return n;
}

char* MacCopySelectionText(void* document) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSelection || doc->textSelection->result.len == 0) {
        return nullptr;
    }
    return DupCString(doc->textSelection->ExtractText(StrL("\n")));
}

bool MacLinkAtPoint(void* document, int pageNo, double x, double y, double zoom, int rotation, MacLink* link) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !link || pageNo < 1 || pageNo > doc->model->PageCount()) {
        return false;
    }
    *link = {};
    PointF point = ToPagePoint(doc, pageNo, x, y, zoom, rotation);
    IPageElement* element = doc->model->GetEngine()->GetElementAtPos(pageNo, point);
    IPageDestination* dest = element ? element->AsLink() : nullptr;
    if (!dest) {
        return false;
    }
    int targetPage = DestPageNo(doc, dest);
    if (targetPage >= 1 && targetPage <= doc->model->PageCount()) {
        link->kind = MacLinkKind::Page;
        link->pageNo = targetPage;
        return true;
    }
    Kind kind = dest->GetKind();
    if (kind == kindDestinationJsMenu) {
        return false;
    }
    Str value = PageDestGetValue(dest);
    if (len(value) == 0) {
        return false;
    }
    link->kind = kind == kindDestinationLaunchFile ? MacLinkKind::File : MacLinkKind::Url;
    link->value = DupCString(value);
    return link->value != nullptr;
}

void MacFreeLink(MacLink* link) {
    if (!link) {
        return;
    }
    free(link->value);
    *link = {};
}

int MacTocItemCount(void* document) {
    MacDocument* doc = AsDocument(document);
    return doc ? len(doc->tocItems) : 0;
}

char* MacCopyTocItemTitle(void* document, int index) {
    MacDocument* doc = AsDocument(document);
    if (!doc || index < 0 || index >= len(doc->tocItems)) {
        return nullptr;
    }
    return DupCString(doc->tocItems[index]->title);
}

int MacTocItemDepth(void* document, int index) {
    MacDocument* doc = AsDocument(document);
    if (!doc || index < 0 || index >= len(doc->tocDepths)) {
        return 0;
    }
    return doc->tocDepths[index];
}

int MacTocItemPage(void* document, int index) {
    MacDocument* doc = AsDocument(document);
    if (!doc || index < 0 || index >= len(doc->tocItems)) {
        return 0;
    }
    TocItem* item = doc->tocItems[index];
    IPageDestination* dest = item->GetPageDestination();
    int pageNo = dest ? DestPageNo(doc, dest) : item->pageNo;
    return pageNo >= 1 && pageNo <= doc->model->PageCount() ? pageNo : 0;
}

//--- sidebar (MacSidebar.mm, MacThumbnails.cpp)

// Whether the ToC item starts expanded (the document's default open state).
bool MacTocItemIsOpen(void* document, int index) {
    MacDocument* doc = AsDocument(document);
    if (!doc || index < 0 || index >= len(doc->tocItems)) {
        return false;
    }
    return doc->tocItems[index]->IsExpanded();
}

// http(s) / mailto target of a ToC item without a page, or nullptr. Free with MacFreeString.
char* MacCopyTocItemUrl(void* document, int index) {
    MacDocument* doc = AsDocument(document);
    if (!doc || index < 0 || index >= len(doc->tocItems)) {
        return nullptr;
    }
    IPageDestination* dest = doc->tocItems[index]->GetPageDestination();
    if (!dest || dest->GetKind() != kindDestinationLaunchURL) {
        return nullptr;
    }
    Str url = PageDestGetValue(dest);
    return IsExternalUrl(url) ? DupCString(url) : nullptr;
}

// The document's engine; used by MacThumbnails.cpp, which AddRef()s it.
EngineBase* MacDocumentEngine(void* document) {
    MacDocument* doc = AsDocument(document);
    return doc && doc->model ? doc->model->GetEngine() : nullptr;
}

//--- end sidebar

int MacPropertyCount(void* document) {
    MacDocument* doc = AsDocument(document);
    if (!doc) {
        return 0;
    }
    LoadProperties(doc);
    return len(doc->properties);
}

char* MacCopyPropertyName(void* document, int index) {
    MacDocument* doc = AsDocument(document);
    if (!doc) {
        return nullptr;
    }
    LoadProperties(doc);
    if (index < 0 || index >= len(doc->properties)) {
        return nullptr;
    }
    return DupCString(PropNameTemp(doc->properties[index].prop));
}

char* MacCopyPropertyValue(void* document, int index) {
    MacDocument* doc = AsDocument(document);
    if (!doc) {
        return nullptr;
    }
    LoadProperties(doc);
    if (index < 0 || index >= len(doc->properties)) {
        return nullptr;
    }
    return DupCString(doc->properties[index].val);
}

void MacFreeString(char* value) {
    free(value);
}

void MacFreeDocumentLayout(MacDocumentLayout* layout) {
    if (!layout) {
        return;
    }
    free(layout->pages);
    *layout = {};
}

void MacFreeRenderedPage(MacRenderedPage* page) {
    if (!page) {
        return;
    }
    free(page->data);
    page->data = nullptr;
}

void MacCloseDocument(void* document) {
    if (!document) {
        return;
    }
    MacDocument* doc = AsDocument(document);
    UnwatchChapterLayout(doc);
    StopFindWorker(doc);
    StopTextWorker(doc);
    delete doc->renderer;
    delete doc->textSelection;
    delete doc->textSearch;
    // doc->toc is owned by the engine
    FreeProps(doc->properties);
    delete doc->model;
    delete doc;
}

void MacShutdown() {
    static bool didShutdown = false;
    if (didShutdown) {
        return;
    }
    didShutdown = true;
    DestroyTempArena();
}

void MacFinalize() {
    DestroyPermArena();
}

//--- UI core (SumatraMac.mm)

// Must match CreateReaderEngine() in ReaderModel.cpp.
static bool IsReaderSupportedKind(FileType kind) {
    if (kind == FileType::Unknown || kind == FileType::Directory) {
        return false;
    }
    if (kind == FileType::Lit || kind == FileType::Mobi || kind == FileType::Chm) {
        return true;
    }
    return IsEngineDjVuSupportedFileType(kind) || IsEngineImageSupportedFileType(kind) ||
           IsEngineCbxSupportedFileType(kind) || IsEngineMupdfSupportedFileType(kind);
}

static bool IsReaderSupportedPath(Str path) {
    if (IsEngineImageDirSupportedFile(path)) {
        return true;
    }
    return IsReaderSupportedKind(GuessFileTypeFromName(path, true));
}

static MacOpenError ClassifyOpenFailure(Str path, bool prompted) {
    if (prompted) {
        return MacOpenError::PasswordCancelled;
    }
    bool isDir = dir::Exists(path);
    if (!isDir && !file::Exists(path)) {
        return MacOpenError::NotFound;
    }
    if (!isDir) {
        FILE* f = fopen(CStrTemp(path), "rb");
        if (!f) {
            return MacOpenError::Unreadable;
        }
        fclose(f);
    }
    if (!IsReaderSupportedPath(path)) {
        return MacOpenError::Unsupported;
    }
    return MacOpenError::Damaged;
}

// Like MacOpenDocument() but reports why opening failed.
void* MacOpenDocumentEx(void* passwordParent, const char* path, MacPageReadyCallback onPageReady, void* callbackContext,
                        MacOpenError* errorOut) {
    MacOpenError err = MacOpenError::None;
    MacDocument* document = OpenDocumentImpl(passwordParent, path, onPageReady, callbackContext, &err);
    if (errorOut) {
        *errorOut = err;
    }
    return document;
}

// Engines recurse deeply (e.g. ebook layout); give the loader the main thread's stack size.
constexpr size_t kOpenThreadStackBytes = 8 * 1024 * 1024;

struct MacOpenJob {
    char* path = nullptr;
    MacPasswordCallback askPassword = nullptr;
    void* passwordContext = nullptr;
    MacPageReadyCallback onPageReady = nullptr;
    void* renderContext = nullptr;
    MacOpenDoneCallback onDone = nullptr;
    void* doneContext = nullptr;
    MacDocument* document = nullptr;
    MacOpenError error = MacOpenError::None;
};

static void FinishOpenJob(MacOpenJob* job) {
    job->onDone(job->doneContext, job->document, job->error);
    free(job->path);
    delete job;
}

static void* RunOpenJob(void* data) {
    auto* job = (MacOpenJob*)data;
    job->document = OpenDocumentImpl(nullptr, job->path, job->onPageReady, job->renderContext, &job->error,
                                     job->askPassword, job->passwordContext, ChapterLayout::Full);
    PlatformPostTask(MkFunc0(FinishOpenJob, job));
    DestroyTempArena();
    return nullptr;
}

// Opens path on a new thread. askPassword runs on that thread (it must not
// wait for anything that waits for this open); onDone runs later on the main
// thread with the handle, or nullptr and the error. Returns false (and never
// calls onDone) if the thread can't be started.
bool MacOpenDocumentAsync(const char* path, MacPasswordCallback askPassword, void* passwordContext,
                          MacPageReadyCallback onPageReady, void* renderContext, MacOpenDoneCallback onDone,
                          void* doneContext) {
    if (!path || !askPassword || !onDone) {
        return false;
    }
    auto* job = new MacOpenJob();
    job->path = DupCString(path);
    job->askPassword = askPassword;
    job->passwordContext = passwordContext;
    job->onPageReady = onPageReady;
    job->renderContext = renderContext;
    job->onDone = onDone;
    job->doneContext = doneContext;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kOpenThreadStackBytes);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int err = job->path ? pthread_create(&thread, &attr, RunOpenJob, job) : -1;
    pthread_attr_destroy(&attr);
    if (err != 0) {
        free(job->path);
        delete job;
        return false;
    }
    return true;
}

bool MacIsSupportedPath(const char* path) {
    if (!path || !path[0]) {
        return false;
    }
    return IsReaderSupportedPath(Str((char*)path));
}

// document extensions offered by the Open panel; GuessFileType has more (e.g. .json)
static const char* gMacOpenExts[] = {
    "pdf", "ai",  "xps",  "oxps", "xod",  "dwfx", "epub", "mobi", "prc",      "azw",  "azw1", "azw3",  "azw4",
    "pdb", "fb2", "fb2z", "fbz",  "zfb2", "lit",  "chm",  "djvu", "djv",      "cbz",  "cbr",  "cb7",   "cbt",
    "zip", "rar", "7z",   "tar",  "ora",  "ps",   "eps",  "md",   "markdown", "html", "htm",  "xhtml", "svg",
    "txt", "nfo", "png",  "jpg",  "jpeg", "jfif", "gif",  "tif",  "tiff",     "bmp",  "ico",  "tga",   "jxr",
    "hdp", "wdp", "webp", "jxl",  "jp2",  "j2k",  "jpx",  "jpf",  "jpm",      "j2c",  "heic", "heif",  "avif",
};

// ';'-separated extensions (without the dot) the reader can open. Free with MacFreeString.
char* MacCopySupportedExtensions() {
    str::Builder out;
    for (const char* ext : gMacOpenExts) {
        Str extStr((char*)ext);
        if (!IsReaderSupportedKind(GuessFileTypeFromName(fmt("x.%s", extStr), true))) {
            continue;
        }
        if (len(out) > 0) {
            out.AppendChar(';');
        }
        out.Append(extStr);
    }
    return DupCString(ToStrTemp(out));
}

struct MacFormatName {
    FileType kind;
    const char* name;
};

static const MacFormatName gDocFormatNames[] = {
    {FileType::PDF, "PDF"},           {FileType::Xps, "XPS"},        {FileType::Epub, "EPUB"},
    {FileType::Mobi, "MOBI"},         {FileType::Fb2, "FB2"},        {FileType::Lit, "LIT"},
    {FileType::Chm, "CHM"},           {FileType::DjVu, "DjVu"},      {FileType::PS, "PostScript"},
    {FileType::Markdown, "Markdown"}, {FileType::HTML, "HTML"},      {FileType::Svg, "SVG"},
    {FileType::PalmDoc, "PalmDoc"},   {FileType::Txt, "plain text"},
};

static const MacFormatName gComicFormatNames[] = {
    {FileType::Cbz, "CBZ"}, {FileType::Cbr, "CBR"}, {FileType::Cb7, "CB7"},   {FileType::Cbt, "CBT"},
    {FileType::Zip, "ZIP"}, {FileType::Rar, "RAR"}, {FileType::SevenZ, "7Z"}, {FileType::Tar, "TAR"},
};

static const MacFormatName gImageFormatNames[] = {
    {FileType::Png, "PNG"},       {FileType::Jpeg, "JPEG"},   {FileType::Gif, "GIF"},   {FileType::Tiff, "TIFF"},
    {FileType::Bmp, "BMP"},       {FileType::Tga, "TGA"},     {FileType::Webp, "WebP"}, {FileType::Jxr, "JPEG XR"},
    {FileType::Jp2, "JPEG 2000"}, {FileType::Jxl, "JPEG XL"}, {FileType::Heic, "HEIC"}, {FileType::Avif, "AVIF"},
    {FileType::Ico, "ICO"},
};

static int AppendSupportedNames(str::Builder& out, const MacFormatName* names, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!IsReaderSupportedKind(names[i].kind)) {
            continue;
        }
        if (count > 0) {
            out.Append(StrL(", "));
        }
        out.Append(Str((char*)names[i].name));
        count++;
    }
    return count;
}

static void AppendFormatGroup(str::Builder& out, Str title, const MacFormatName* names, int n) {
    str::Builder list;
    if (AppendSupportedNames(list, names, n) == 0) {
        return;
    }
    if (len(out) > 0) {
        out.Append(StrL(", "));
    }
    out.Append(title);
    out.Append(StrL(" ("));
    out.Append(ToStrTemp(list));
    out.AppendChar(')');
}

// Human-readable list of the supported formats, e.g. for error messages.
// Free with MacFreeString.
char* MacCopySupportedFormats() {
    str::Builder out;
    AppendSupportedNames(out, gDocFormatNames, dimofi(gDocFormatNames));
    AppendFormatGroup(out, StrL("comic books"), gComicFormatNames, dimofi(gComicFormatNames));
    AppendFormatGroup(out, StrL("images"), gImageFormatNames, dimofi(gImageFormatNames));
    out.Append(StrL(" and folders of images"));
    return DupCString(ToStrTemp(out));
}

// Drops queued (not yet started) renders, e.g. for pages scrolled out of view.
// The render in progress and the cache are kept.
void MacCancelPendingRenders(void* document) {
    MacDocument* doc = AsDocument(document);
    if (doc && doc->renderer) {
        doc->renderer->CancelRequests();
    }
}

// Like MacRenderPage() but with print-only content (RenderTarget::Print).
bool MacRenderPageForPrint(void* document, int pageNo, float zoom, int rotation, MacRenderedPage* page) {
    if (!page) {
        return false;
    }
    *page = {};
    MacDocument* doc = AsDocument(document);
    if (!doc || pageNo < 1 || pageNo > doc->model->PageCount()) {
        return false;
    }
    Pixmap* pixmap = doc->model->RenderPageForPrint(pageNo, zoom > 0 ? zoom : 1.0f, rotation);
    bool ok = CopyPixmap(pixmap, page);
    FreePixmap(pixmap);
    return ok;
}

void MacClearSelection(void* document) {
    MacDocument* doc = AsDocument(document);
    if (doc && doc->textSelection) {
        doc->textSelection->Reset();
    }
}

// Selects the word or line under (x, y) in page-local view coordinates.
bool MacSelectAt(void* document, int pageNo, double x, double y, double zoom, int rotation, MacSelectUnit unit) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSelection || pageNo < 1 || pageNo > doc->model->PageCount()) {
        return false;
    }
    PointF pt = ToPagePoint(doc, pageNo, x, y, zoom, rotation);
    if (!doc->textSelection->IsOverGlyph(pageNo, pt.x, pt.y)) {
        return false;
    }
    doc->textSelection->Reset();
    if (unit == MacSelectUnit::Line) {
        doc->textSelection->SelectLineAt(pageNo, pt.x, pt.y);
    } else {
        doc->textSelection->SelectWordAt(pageNo, pt.x, pt.y);
    }
    return doc->textSelection->result.len > 0;
}

//--- asynchronous find

// One worker thread per document runs TextSearch so the UI never blocks.
// doc->textSearch is only touched by the worker; the main thread reads the
// committed hit (hitPages/hitRects) under the mutex.
struct MacFindWorker {
    Mutex mutex;
    ConditionVariable condition;
    ThreadHandle thread = nullptr;
    MacDocument* doc = nullptr;
    AtomicInt cancel = 0;
    bool stopping = false;
    bool stopped = false;
    bool pending = false;
    bool busy = false;

    Str text;
    int startPage = 1;
    bool forward = true;
    bool restart = true;
    int token = 0;
    MacFindDoneCallback onDone = nullptr;
    void* callbackContext = nullptr;

    Vec<int> hitPages;
    Vec<Rect> hitRects;
    int hitPage = 0;
};

struct MacFindDone {
    MacFindDoneCallback onDone = nullptr;
    void* context = nullptr;
    void* document = nullptr;
    int token = 0;
    bool found = false;
};

static int gFindToken = 0;

static void RunFindDone(MacFindDone* done) {
    done->onDone(done->context, done->document, done->token, done->found);
    delete done;
}

static void OnFindProgress(MacFindWorker* w, ProgressUpdateData* data) {
    if (data->wasCancelled) {
        *data->wasCancelled = AtomicIntGet(&w->cancel) != 0;
    }
}

static bool SearchOnce(MacFindWorker* w, Str query, int startPage, bool forward, bool restart) {
    TextSearch* search = w->doc->textSearch;
    bool newText = !str::Eq(search->lastText, query);
    search->SetDirection(forward ? TextSearch::Direction::Forward : TextSearch::Direction::Backward);
    TextSel* result = nullptr;
    if (restart || newText || len(search->findText) == 0) {
        result = search->FindFirst(startPage, query);
    } else {
        result = search->FindNext();
    }
    if (!result && AtomicIntGet(&w->cancel) == 0) {
        // wrap around
        int wrapPage = forward ? search->RestrictFirst() : search->RestrictLast();
        result = search->FindFirst(wrapPage, query);
    }
    if (!result) {
        search->Reset();
    }
    return result != nullptr;
}

// Called with w->mutex held.
static void CommitFindResult(MacFindWorker* w, bool found) {
    VecReset(w->hitPages);
    VecReset(w->hitRects);
    w->hitPage = 0;
    if (!found) {
        return;
    }
    TextSearch* search = w->doc->textSearch;
    const TextSel& r = search->result;
    for (int i = 0; i < r.len; i++) {
        VecAppend(w->hitPages, r.pages[i]);
        VecAppend(w->hitRects, r.rects[i]);
    }
    w->hitPage = search->GetSearchHitStartPageNo();
}

static void FindWorkerLoop(MacFindWorker* w) {
    for (;;) {
        w->mutex.Lock();
        while (!w->stopping && !w->pending) {
            w->condition.Wait(&w->mutex);
        }
        if (w->stopping) {
            w->stopped = true;
            w->condition.WakeAll();
            w->mutex.Unlock();
            return;
        }
        w->pending = false;
        Str text = w->text;
        int startPage = w->startPage;
        bool forward = w->forward;
        bool restart = w->restart;
        w->mutex.Unlock();

        bool found = SearchOnce(w, text, startPage, forward, restart);

        w->mutex.Lock();
        bool cancelled = AtomicIntGet(&w->cancel) != 0;
        if (!cancelled) {
            CommitFindResult(w, found);
        }
        MacFindDone* done = nullptr;
        if (!cancelled && w->onDone) {
            done = new MacFindDone();
            done->onDone = w->onDone;
            done->context = w->callbackContext;
            done->document = w->doc;
            done->token = w->token;
            done->found = found;
        }
        w->busy = false;
        w->condition.WakeAll();
        w->mutex.Unlock();
        if (done) {
            PlatformPostTask(MkFunc0(RunFindDone, done));
        }
    }
}

static MacFindWorker* EnsureFindWorker(MacDocument* doc) {
    if (doc->find) {
        return doc->find;
    }
    auto* w = new MacFindWorker();
    w->doc = doc;
    doc->textSearch->progressCb = MkFunc1(OnFindProgress, w);
    w->thread = StartThread(MkFunc0(FindWorkerLoop, w), StrL("find"));
    if (!w->thread) {
        doc->textSearch->progressCb = {};
        delete w;
        return nullptr;
    }
    doc->find = w;
    return w;
}

// Cancels a queued or running search and waits until the worker is idle.
static void CancelFindAndWait(MacFindWorker* w) {
    AtomicIntSet(&w->cancel, 1);
    w->mutex.Lock();
    if (w->pending) {
        w->pending = false;
        w->busy = false;
    }
    while (w->busy) {
        w->condition.Wait(&w->mutex);
    }
    AtomicIntSet(&w->cancel, 0);
    w->mutex.Unlock();
}

static void StopFindWorker(MacDocument* doc) {
    MacFindWorker* w = doc->find;
    if (!w) {
        return;
    }
    AtomicIntSet(&w->cancel, 1);
    w->mutex.Lock();
    w->stopping = true;
    w->pending = false;
    w->condition.WakeAll();
    while (!w->stopped) {
        w->condition.Wait(&w->mutex);
    }
    w->mutex.Unlock();
    SafeCloseThreadHandle(&w->thread);
    doc->textSearch->progressCb = {};
    str::Free(w->text);
    delete w;
    doc->find = nullptr;
}

// Starts a search on the worker thread and returns its token (0 if not
// started). onDone runs on the main thread unless the search is cancelled;
// it may arrive after the document was closed, so check the token first.
int MacFindStart(void* document, int startPage, const char* text, MacFindDirection direction, MacFindMode mode,
                 MacFindDoneCallback onDone, void* callbackContext) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->textSearch || !text || !text[0]) {
        return 0;
    }
    MacFindWorker* w = EnsureFindWorker(doc);
    if (!w) {
        return 0;
    }
    CancelFindAndWait(w);

    AutoUnlockMutex lock(&w->mutex);
    str::ReplaceWithCopy(&w->text, Str((char*)text));
    w->startPage = limitValue(startPage, 1, doc->model->PageCount());
    w->forward = direction == MacFindDirection::Forward;
    w->restart = mode == MacFindMode::Restart;
    w->token = ++gFindToken;
    w->onDone = onDone;
    w->callbackContext = callbackContext;
    w->pending = true;
    w->busy = true;
    w->condition.WakeAll();
    return w->token;
}

void MacFindCancel(void* document) {
    MacDocument* doc = AsDocument(document);
    if (doc && doc->find) {
        CancelFindAndWait(doc->find);
    }
}

bool MacFindIsBusy(void* document) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->find) {
        return false;
    }
    AutoUnlockMutex lock(&doc->find->mutex);
    return doc->find->busy;
}

void MacFindClear(void* document) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->find) {
        return;
    }
    AutoUnlockMutex lock(&doc->find->mutex);
    VecReset(doc->find->hitPages);
    VecReset(doc->find->hitRects);
    doc->find->hitPage = 0;
}

// First page of the last found hit, 0 if none.
int MacFindResultPage(void* document) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->find) {
        return 0;
    }
    AutoUnlockMutex lock(&doc->find->mutex);
    return doc->find->hitPage;
}

int MacFindResultRectCount(void* document, int pageNo) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->find) {
        return 0;
    }
    AutoUnlockMutex lock(&doc->find->mutex);
    int count = 0;
    for (int page : doc->find->hitPages) {
        count += page == pageNo ? 1 : 0;
    }
    return count;
}

// index-th hit rectangle on pageNo in page-local view coordinates.
bool MacFindResultRect(void* document, int pageNo, int index, double zoom, int rotation, MacDisplayRect* rect) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !doc->find || !rect || index < 0) {
        return false;
    }
    bool found = false;
    Rect hit;
    {
        AutoUnlockMutex lock(&doc->find->mutex);
        for (int i = 0; i < len(doc->find->hitPages); i++) {
            if (doc->find->hitPages[i] != pageNo || index-- != 0) {
                continue;
            }
            hit = doc->find->hitRects[i];
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    RectF r = doc->model->GetEngine()->Transform(ToRectF(hit), pageNo, (float)zoom, rotation);
    rect->x = r.x;
    rect->y = r.y;
    rect->width = r.dx;
    rect->height = r.dy;
    return true;
}

//--- scroll position (FileState.ScrollPos is in page units, like the Windows app)

// Page-local view point (x, y at zoom and rotation) to page units.
bool MacPagePointFromView(void* document, int pageNo, double x, double y, double zoom, int rotation, double* pageX,
                          double* pageY) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !pageX || !pageY || zoom <= 0 || pageNo < 1 || pageNo > doc->model->PageCount()) {
        return false;
    }
    PointF p = ToPagePoint(doc, pageNo, x, y, zoom, rotation);
    *pageX = p.x;
    *pageY = p.y;
    return true;
}

// Page units to a page-local view point at zoom and rotation.
bool MacViewPointFromPage(void* document, int pageNo, double pageX, double pageY, double zoom, int rotation, double* x,
                          double* y) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !x || !y || zoom <= 0 || pageNo < 1 || pageNo > doc->model->PageCount()) {
        return false;
    }
    PointF p((float)pageX, (float)pageY);
    PointF v = doc->model->GetEngine()->Transform(p, pageNo, (float)zoom, rotation);
    *x = v.x;
    *y = v.y;
    return true;
}

//--- Select All on long documents: page text is extracted on a worker first

struct MacTextWorker {
    pthread_t thread;
    AtomicInt cancel = 0;
    MacDocument* doc = nullptr;
    int token = 0;
    MacTextProgressCallback onProgress = nullptr;
    void* context = nullptr;
};

struct MacTextProgress {
    MacTextProgressCallback onProgress = nullptr;
    void* context = nullptr;
    void* document = nullptr;
    int token = 0;
    int done = 0;
    int total = 0;
};

constexpr int kTextProgressPages = 25;
static int gTextToken = 0;

static void RunTextProgress(MacTextProgress* p) {
    p->onProgress(p->context, p->document, p->token, p->done, p->total);
    delete p;
}

static void PostTextProgress(MacTextWorker* w, int done, int total) {
    auto* p = new MacTextProgress();
    p->onProgress = w->onProgress;
    p->context = w->context;
    p->document = w->doc;
    p->token = w->token;
    p->done = done;
    p->total = total;
    PlatformPostTask(MkFunc0(RunTextProgress, p));
}

static void* TextWorkerMain(void* data) {
    auto* w = (MacTextWorker*)data;
    EngineBase* engine = w->doc->model->GetEngine();
    int total = engine->PageCount();
    for (int pageNo = 1; pageNo <= total; pageNo++) {
        if (AtomicIntGet(&w->cancel) != 0) {
            DestroyTempArena();
            return nullptr;
        }
        // the engine caches it (thread-safe, like the find worker's reads)
        engine->GetTextForPage(pageNo);
        ResetTempArena();
        if (pageNo % kTextProgressPages == 0 && pageNo < total) {
            PostTextProgress(w, pageNo, total);
        }
    }
    PostTextProgress(w, total, total);
    DestroyTempArena();
    return nullptr;
}

// Cancels and joins; the page being extracted finishes first.
static void StopTextWorker(MacDocument* doc) {
    MacTextWorker* w = doc->text;
    if (!w) {
        return;
    }
    AtomicIntSet(&w->cancel, 1);
    pthread_join(w->thread, nullptr);
    delete w;
    doc->text = nullptr;
}

// Extracts every page's text on a worker thread so MacSelectAll() is quick
// afterwards. onProgress runs on the main thread with the returned token:
// done < total while working, done == total at the end. A cancelled run may
// still deliver, so check the token. Returns 0 if not started.
int MacPrepareTextStart(void* document, MacTextProgressCallback onProgress, void* context) {
    MacDocument* doc = AsDocument(document);
    if (!doc || !onProgress) {
        return 0;
    }
    StopTextWorker(doc);
    auto* w = new MacTextWorker();
    w->doc = doc;
    w->token = ++gTextToken;
    w->onProgress = onProgress;
    w->context = context;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kOpenThreadStackBytes);
    int err = pthread_create(&w->thread, &attr, TextWorkerMain, w);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        delete w;
        return 0;
    }
    doc->text = w;
    return w->token;
}

void MacPrepareTextCancel(void* document) {
    MacDocument* doc = AsDocument(document);
    if (doc) {
        StopTextWorker(doc);
    }
}

// Lays out every chapter of a chaptered ebook (printing needs the final page
// count); the engine then reports the new count like the background pass.
void MacLayOutAllPages(void* document) {
    MacDocument* doc = AsDocument(document);
    if (doc) {
        doc->model->GetEngine()->EnsureAllChaptersLaidOut();
    }
}

// The main thread's temp arena, reset between events like the Windows message loop.
void MacResetTempArena() {
    ResetTempArena();
}
