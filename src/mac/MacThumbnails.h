/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Background page thumbnails for the macOS sidebar. Plain C API so Cocoa files
// can use it without base/Base.h. See MacThumbnails.cpp for the threading model.

// Pixels are BGRA, premultiplied alpha, top-down (CGImage: ByteOrder32Little |
// PremultipliedFirst). They stay valid until MacThumbsReleaseImage(ref).
struct MacThumbImage {
    int width;
    int height;
    int stride;
    bool exact;
    const unsigned char* data;
    void* ref;
};

enum class MacThumbPriority {
    Visible,
    Prefetch,
};

using MacThumbReadyCallback = void (*)(void* context, void* document, int pageNo);

void* MacThumbsCreate(MacThumbReadyCallback onReady, void* context, long long maxCacheBytes);
void MacThumbsDestroy(void* thumbs);
void MacThumbsRequest(void* thumbs, void* document, int pageNo, int rotation, int dx, int dy,
                      MacThumbPriority priority);
void MacThumbsPrune(void* thumbs, void* document, int rotation, int firstPage, int lastPage);
bool MacThumbsGet(void* thumbs, void* document, int pageNo, int rotation, int dx, int dy, MacThumbImage* image);
void MacThumbsReleaseImage(void* ref);
void MacThumbsForget(void* thumbs, void* document);
long long MacThumbsCacheBytes(void* thumbs);
int MacThumbsPendingCount(void* thumbs);
