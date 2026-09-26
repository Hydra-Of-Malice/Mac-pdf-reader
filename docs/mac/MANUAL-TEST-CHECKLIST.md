# macOS manual test checklist

Run on a release build (`bun cmd/build.ts -mac -rel`), on Apple silicon and Intel if possible, and on the oldest
supported macOS (11). Fixtures: `tests/mac/fixtures/` (see [formats.md](formats.md)), `ext/a-zlib/zlib.3.pdf`.
Record the build (`SOURCE.txt`), macOS version, and failures with steps.

## Install and launch

- [ ] Package opens: `.dmg` mounts and shows `SumatraPDF.app` + `Applications`; `.zip` / `.tar.gz` extract.
- [ ] Downloaded (quarantined) copy: Gatekeeper flow from `src/mac/Resources/package-README.md` works.
- [ ] Dock and Finder show the SumatraPDF icon; **About** shows name, version and copyright.
- [ ] Bare launch restores the previous session; first launch shows an empty window.

## Opening documents

- [ ] Finder: double-click after **Get Info > Open with > SumatraPDF**; **Open With** menu lists SumatraPDF.
- [ ] Drag a file onto the Dock icon; drag a file onto the open window.
- [ ] **File > Open** (Cmd+O): panel filters to supported types; cancel does nothing.
- [ ] Command line: `open -a SumatraPDF.app file`, `open SumatraPDF.app --args /abs/path`, running the executable.
- [ ] **Open Recent** lists recent files and reopens them; a deleted recent file fails cleanly.
- [ ] Opening an already open file switches to its tab.
- [ ] File changed on disk while open reloads and keeps the page.

## Formats

For each, open a sample and check page count, rendering, text selection and search where the format has text:

- [ ] PDF (text, scanned, forms, annotations, rotated pages, huge page count)
- [ ] EPUB, MOBI / AZW3 / PRC, FB2 / FBZ, LIT
- [ ] CBZ, CBR, CB7, CBT (image order, mixed image formats)
- [ ] XPS / OXPS, DjVu, CHM
- [ ] Markdown, SVG, images (PNG, JPEG, GIF, TIFF multi-page, BMP, WebP, JPEG 2000)
- [ ] Unsupported or renamed file (e.g. `.txt` renamed `.pdf`): clear error, no crash.

## Navigation and view

- [ ] Scroll (trackpad, wheel, keyboard), Page Up/Down, Home/End, go to page, next/previous page.
- [ ] Zoom in/out, pinch zoom, actual size, fit page, fit width; zoom stays sharp on Retina.
- [ ] Rotate left/right; single page vs continuous; fullscreen enter/exit.
- [ ] Resize the window; move it between Retina and non-Retina displays.
- [ ] Tabs: open several, switch (keyboard and click), close, reopen closed tab, each keeps its own page and zoom.
- [ ] Outline (table of contents) navigates; thumbnails show and navigate; sidebar toggles.
- [ ] Links: internal link jumps, URL opens the browser, file link opens the file.

## Text

- [ ] Mouse selection, **Select All**, **Copy** (Cmd+C) pastes correct text elsewhere, including non-Latin text.
- [ ] Find (Cmd+F), Find Next / Previous (Cmd+G / Shift+Cmd+G), wrap-around, no-match message, match highlight.

## Printing

- [ ] **Print** (Cmd+P): preview, page range, current rotation, cancel; print to PDF matches the document.

## Robustness

- [ ] Password-protected PDF: prompt, wrong password retries, cancel, correct password opens.
- [ ] Malformed / truncated files of each format: error message, no crash or hang.
- [ ] Large document (1000+ pages, big scans): scrolling stays responsive, memory stays bounded.
- [ ] Quit with documents open, relaunch: files, pages, zoom and layout are restored.

## Accessibility and keyboard

- [ ] VoiceOver (Cmd+F5): window, toolbar, tabs, menus and dialogs are announced.
- [ ] All menu items show their shortcuts, and the shortcuts work; standard macOS shortcuts (Cmd+W, Cmd+Q, Cmd+M,
      Cmd+H, Cmd+`) behave as expected.
- [ ] Keyboard-only use: every dialog can be completed and dismissed (Esc / Return).
- [ ] Light and dark appearance; Increase Contrast; larger text in System Settings.
