/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Plain C bridge between the Cocoa app (SumatraMac.mm) and the C++ engine/base
// layer. Cocoa files must not include base/Base.h (Apple headers define names
// like Size that clash with Sumatra types), so all engine access goes through
// this header.

struct MacRenderedPage {
    int width;
    int height;
    int stride;
    bool premultiplied;
    unsigned char* data;
};

struct MacLayoutParams {
    bool continuous;
    int startPage;
    int viewX;
    int viewY;
    int viewWidth;
    int viewHeight;
    double zoomVirtual;
    double backingScale;
    int rotation;
};

struct MacLayoutPage {
    int pageNo;
    int x;
    int y;
    int width;
    int height;
    int screenX;
    int screenY;
    int screenWidth;
    int screenHeight;
    double visibleRatio;
    double layoutZoom;
    double renderZoom;
    bool shown;
};

struct MacDisplayRect {
    double x;
    double y;
    double width;
    double height;
};

enum class MacLinkKind {
    None,
    Page,
    Url,
    File,
};

struct MacLink {
    MacLinkKind kind;
    int pageNo;
    char* value;
};

struct MacDocumentLayout {
    int pageCount;
    int currentPage;
    int canvasWidth;
    int canvasHeight;
    MacLayoutPage* pages;
};

using MacPageReadyCallback = void (*)(void* context);
using MacPasswordCallback = const char* (*)(void* context, const char* fileName, int attempt);

void* MacOpenDocument(void* passwordParent, const char* path, MacPageReadyCallback onPageReady, void* callbackContext,
                      char** errorOut);
void MacSetPasswordCallback(MacPasswordCallback callback, void* context);

int MacPageCount(void* document);

bool MacPageSize(void* document, int pageNo, double* widthOut, double* heightOut);

double MacFileDPI(void* document);

bool MacRenderPage(void* document, int pageNo, float zoom, int rotation, MacRenderedPage* page);
void MacRequestPage(void* document, int pageNo, float zoom, int rotation, int priority);
bool MacCopyRenderedPage(void* document, int pageNo, float zoom, int rotation, MacRenderedPage* page);
void MacResetRenderer(void* document);

int MacFindResultPage(void* document);
int MacFindResultRectCount(void* document, int pageNo);
bool MacFindResultRect(void* document, int pageNo, int index, double zoom, int rotation, MacDisplayRect* rect);

bool MacTextAtPoint(void* document, int pageNo, double x, double y, double zoom, int rotation);
bool MacStartSelection(void* document, int pageNo, double x, double y, double zoom, int rotation);
bool MacUpdateSelection(void* document, int pageNo, double x, double y, double zoom, int rotation);
void MacSelectAll(void* document);
bool MacHasSelection(void* document);
int MacSelectionRectCount(void* document, int pageNo);
bool MacSelectionRect(void* document, int pageNo, int index, double zoom, int rotation, MacDisplayRect* rect);
char* MacCopySelectionText(void* document);

bool MacLinkAtPoint(void* document, int pageNo, double x, double y, double zoom, int rotation, MacLink* link);
void MacFreeLink(MacLink* link);

int MacTocItemCount(void* document);
char* MacCopyTocItemTitle(void* document, int index);
int MacTocItemDepth(void* document, int index);
int MacTocItemPage(void* document, int index);

// sidebar (MacSidebar.mm, MacThumbnails.cpp)
bool MacTocItemIsOpen(void* document, int index);
char* MacCopyTocItemUrl(void* document, int index);
class EngineBase;
EngineBase* MacDocumentEngine(void* document);

int MacPropertyCount(void* document);
char* MacCopyPropertyName(void* document, int index);
char* MacCopyPropertyValue(void* document, int index);
void MacFreeString(char* value);

bool MacLayoutDocument(void* document, const MacLayoutParams* params, MacDocumentLayout* layout);
void MacFreeDocumentLayout(MacDocumentLayout* layout);

void MacFreeRenderedPage(MacRenderedPage* page);
void MacCloseDocument(void* document);
void MacShutdown();
void MacFinalize();

//--- UI core (SumatraMac.mm); ownership and threading: docs/mac/architecture.md

enum class MacOpenError {
    None,
    NotFound,
    Unreadable,
    Unsupported,
    PasswordCancelled,
    Damaged,
    RendererFailed,
};

enum class MacSelectUnit {
    Word,
    Line,
};

enum class MacFindDirection {
    Forward,
    Backward,
};

enum class MacFindMode {
    Next,
    Restart,
};

using MacFindDoneCallback = void (*)(void* context, void* document, int token, bool found);

void* MacOpenDocumentEx(void* passwordParent, const char* path, MacPageReadyCallback onPageReady, void* callbackContext,
                        MacOpenError* errorOut);
bool MacIsSupportedPath(const char* path);
char* MacCopySupportedExtensions();
char* MacCopySupportedFormats();

void MacCancelPendingRenders(void* document);
bool MacRenderPageForPrint(void* document, int pageNo, float zoom, int rotation, MacRenderedPage* page);

void MacClearSelection(void* document);
bool MacSelectAt(void* document, int pageNo, double x, double y, double zoom, int rotation, MacSelectUnit unit);

int MacFindStart(void* document, int startPage, const char* text, MacFindDirection direction, MacFindMode mode,
                 MacFindDoneCallback onDone, void* callbackContext);
void MacFindCancel(void* document);
bool MacFindIsBusy(void* document);
void MacFindClear(void* document);
