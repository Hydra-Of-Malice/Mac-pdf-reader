/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class EngineBase;
struct Gfx;
struct RenderPageArgs;
struct PageRenderKey;
enum class PageRenderPriority;

// Clone: render on a copy of the engine, so rendering never waits on the UI thread's engine calls.
// Shared: render on the engine itself: needed when its page numbering can change after open
// (chaptered reflowable docs lay chapters out lazily; a copy would paginate differently).
enum class PageRenderEngine {
    Clone,
    Shared,
};

// How the worker renders a page; the default is engine->RenderPage(args). The macOS reader passes
// RenderPageWithDocColors (DocColors.h) to apply its dark mode.
using PageRenderFn = Pixmap* (*)(EngineBase* engine, RenderPageArgs& args);

struct PageRenderService {
    void* data = nullptr;

    PageRenderService() = default;
    PageRenderService(const PageRenderService&) = delete;
    PageRenderService& operator=(const PageRenderService&) = delete;
    ~PageRenderService();

    static PageRenderService* Create(EngineBase* engine, const Func0& onPageReady, i64 maxBytes = 96LL * 1024 * 1024,
                                     PageRenderEngine use = PageRenderEngine::Clone, PageRenderFn render = nullptr);

    void NewGeneration();
    void CancelRequests();
    void Request(PageRenderKey key, PageRenderPriority priority);
    Pixmap* CopyPage(PageRenderKey key);
    bool DrawPage(Gfx* gfx, PageRenderKey key, const Rect& target);
    i64 CacheBytes() const;
};
