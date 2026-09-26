# macOS app architecture

The app is an AppKit shell over SumatraPDF's portable C++ engines. Cocoa code can't include `base/Base.h` (Apple
headers clash with Sumatra names such as `Size`), so the two sides meet only through plain C headers.

```
 SumatraMac.mm        app delegate: window, tabs, toolbar, menus, find, printing, file watching
 MacDocumentView.mm   page canvas (drawing, mouse, keys, magnify, accessibility), print view
 MacPanels.mm         command palette, keyboard shortcuts window
 MacSidebar.mm        outline + thumbnails (NSSplitView pane, host protocol in MacSidebar.h)
        |  plain C: mac/SumatraMacEngine.h, mac/MacPrefs.h, mac/MacThumbnails.h
        v
 SumatraMacEngine.cpp MacDocument = ReaderModel + PageRenderService + TextSelection + TextSearch (+ find worker)
 MacPrefs.cpp         GlobalPrefs / FileState / SessionData in ~/Library/Application Support/SumatraPDF
        |
 engines (EngineMupdf, EngineDjvuDec, EngineImages, EngineCbx, LIT), DocumentLayout, PageRenderPolicy
```

`src/gui/mac/` implements the two portable GUI hooks the reader model needs on Cocoa: the password dialog
(`ShowPasswordDialog`) and `PlatformPostTask` (run on the main queue).

## Handles and ownership

- A document handle (`void*`) comes from `MacOpenDocumentAsync()` (or `MacOpenDocumentEx()`) and is owned by one
  `SumatraTabState`. It is closed
  with `MacCloseDocument()` exactly once, on the main thread, after `-[SumatraSidebar documentWillClose:]`.
  Closing stops the document's find worker and render thread (joins them) before freeing anything.
- Strings the bridge returns (`MacCopy*`, `MacPrefsCopy*`) are `malloc`ed; free with `MacFreeString()`.
- `MacDocumentLayout.pages`: free with `MacFreeDocumentLayout()`. `MacLink.value`: `MacFreeLink()`.
- `MacRenderedPage.data` is `malloc`ed BGRA. `SumatraCreateImage()` hands the buffer to a `CGImage` without copying
  and sets `data` to null; `MacFreeRenderedPage()` frees whatever is left, so always call it.
- The engine owns the table of contents (`TocTree`); the bridge only indexes it.
- Cocoa objects are manual retain/release (no ARC). The sidebar and document view keep an unretained (`assign`)
  pointer to the app delegate, which lives for the whole process.

## Threads

- All AppKit and all bridge calls happen on the main thread, except inside the bridge's own workers.
- Rendering: one `PageRenderService` worker per document renders with a cloned engine into a bounded cache
  (256 MB, one page at most 32 Mpx; larger pages render at a capped zoom). Completion is posted to the main queue;
  the service drops the notification once the document is closed. The app then copies finished pages once into a
  small `CGImage` cache of the visible pages (± 2) of the active tab only.
- Every layout pass first drops queued renders (`MacCancelPendingRenders`), then requests the visible pages and
  prefetches two pages on each side, so pages scrolled past are never rendered. Zoom and rotation start a new
  render generation (`MacResetRenderer`), which also aborts the page being rendered. Switching tabs frees the
  inactive document's render cache. While a pinch is in progress existing images are scaled and nothing is
  requested.
- Find: `MacFindStart()` runs `TextSearch` on a per-document worker thread; it cancels a previous search first.
  The result (hit rectangles, page) is copied under a mutex and read by the main thread. The completion callback
  runs on the main queue with a token; the app ignores it unless the active tab still has that document and token.
- Thumbnails: `MacThumbnails` (see `MacThumbnails.h`), owned by the sidebar.
- Opening: `MacOpenDocumentAsync()` opens on its own thread (8 MB stack; the app runs at most 4 at a time). The tab
  exists at once (`loadRequest` set, no document); the result arrives on the main queue and a document whose tab
  was closed meanwhile is closed there. The loader can't be interrupted. Password prompts: the loader calls the
  per-open callback, which runs the app-modal prompt on the main thread with `dispatch_sync`; the main thread never
  waits for a loader, so this can't deadlock. Engines keep asking until the password is right or the prompt is
  cancelled. Reloading a changed file still opens synchronously (`MacOpenDocumentEx()`).
- Select All on long documents: `MacPrepareTextStart()` extracts every page's text on a worker (the engine's text
  cache is thread-safe), progress arrives on the main queue with a token; `MacCloseDocument()` joins the worker.

## Coordinates

- The document view is flipped (y down), in points. `MacLayoutDocument()` returns each page's frame in document
  view coordinates, its `layoutZoom` (points per page unit) and `renderZoom` (= `layoutZoom` × backing scale,
  capped), so pages render at device resolution on Retina displays.
- Page-local coordinates (x, y relative to the page frame's origin, points) are what hit testing, selection and
  highlight functions take and return, together with `layoutZoom` and the rotation (0/90/180/270). The bridge
  converts them to page space with `EngineBase::Transform()`.
- Zoom: a factor (1 = actual size, 72 points per inch for PDFs) or `-1` fit page / `-2` fit width, the same values
  as `DocumentLayoutParams::zoomVirtual` and the settings file.
- `CGImage`s are drawn bottom-up, so drawing flips the context per page.

## Errors

- `MacOpenDocumentEx()` reports `MacOpenError`: not found, unreadable, unsupported format, password cancelled,
  damaged, renderer failed. The app shows a sheet for each (queued, one at a time); the unsupported-format sheet
  lists the formats from `MacCopySupportedFormats()`. That list and the Open panel's extensions come from the same
  predicate as `ReaderModel` (keep `IsReaderSupportedKind()` in sync with `CreateReaderEngine()`).
- A document that fails to reload after a change on disk keeps the old copy open.
- Bridge functions accept null handles and out-of-range pages and return 0 / false / null.
