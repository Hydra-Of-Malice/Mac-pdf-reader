# macOS port: document formats

"Supported" means a representative fixture opened, rendered and (where it has text) was searched through
`src/mac/SumatraMacEngine.h` in a recorded run of `tests/mac/run-engine-tests.ts`. A registered extension is not
support. Last run: GitHub Actions macOS 15.7.9 runners, Apple clang 17, arm64 (macos-15) and x86_64 (macos-15-intel),
commit 440be2f of the port mirror: 56/58 fixtures pass on both architectures. The
in-app self-test (`-self-test`, see BUILDING.md) additionally opens every fixture in the real Cocoa app and snapshots
the rendered page.

The Cocoa app opens files through `MacOpenDocument()` → `ReaderModel::Create()` (`src/ReaderModel.cpp`), not
Windows' `CreateEngineFromFile()`. `ReaderModel` picks the engine by file extension (no content sniffing). The ebook
engines in `src/EngineEbook.cpp` and the CHM viewers (`ChmModel`, `EngineChm`) are Windows-only (GDI+ / IE), so on
macOS MOBI, CHM and LIT are converted to an in-memory EPUB that MuPDF lays out.

## Formats (macOS CI run 2026-09-27: 56/58 fixtures pass on arm64 and x64)

| Format                                                                                     | Engine on mac                                                                           | Fixtures                                                                                                                                                                                                                                        | Result                               | Verdict                                                                                                                 |
| ------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------ | ----------------------------------------------------------------------------------------------------------------------- |
| PDF (incl. AES-128/256 passwords, malformed, 2000 pages)                                   | EngineMupdf                                                                             | text.pdf, encrypted-aes\*.pdf, truncated/garbage/empty/header-only, generated 2000-page, 3 repo PDFs                                                                                                                                            | 14/14                                | supported                                                                                                               |
| XPS / OXPS                                                                                 | EngineMupdf                                                                             | sample.xps (text, outline, internal + URL links), corrupt.xps                                                                                                                                                                                   | 2/2                                  | supported                                                                                                               |
| EPUB                                                                                       | EngineMupdf (chaptered, lazy layout)                                                    | sample.epub, chapters.epub (page count grows 4 → 12 after open), issue-5846/6095.epub, corrupt, empty, regress/epub-link-invalid-utf8.epub                                                                                                      | macOS 5/5 (round 7); Linux 7/7       | supported; the bridge reports page-count growth (page-ready callback, `MacPageCountIsFinal()`)                          |
| FB2 / FBZ                                                                                  | EngineMupdf (FBZ unzipped first)                                                        | sample.fb2/.fbz, issue-2254/5792.fb2                                                                                                                                                                                                            | 4/4                                  | supported                                                                                                               |
| MOBI / AZW / AZW3 / PRC                                                                    | MobiDoc → in-memory EPUB (split into chapters above 512 KB of text) → EngineMupdf       | sample.mobi (PalmDOC + image), uncompressed.azw, issue-4315.azw3 (KF8 images), huffcdic.mobi (HUFF/CDIC, ToC, filepos links), drm.azw + encrypted.mobi, generated 500-chapter MOBI, corrupt                                                     | macOS: 5/5 (round 7); Linux: 8/8     | supported; DRM books open to a page that says they can't be shown; no KF8 text-book fixture                             |
| AZW4 (Print Replica)                                                                       | embedded PDF → EngineMupdf                                                              | print-replica.azw4 (PalmDOC-compressed %MOP + PDF)                                                                                                                                                                                              | macOS 1/1 (round 7); Linux 1/1       | supported                                                                                                               |
| CBZ / CB7 / CBT                                                                            | EngineCbx (libarchive)                                                                  | sample.cbz/.cb7/.cbt, issue-1201.cbz, password-zipcrypto.cbz, password-aes.cbz, huge-pages.cbz (headers claim 48 × 2130706432 px), corrupt, empty                                                                                               | Linux 8/9                            | supported; ZipCrypto-encrypted CBZ opens, WinZip-AES CBZ fails on Linux (libarchive without a crypto backend)           |
| CBR                                                                                        | EngineCbx (UnRAR, as on Windows)                                                        | RAR4 + RAR5 samples, password.cbr, corrupt                                                                                                                                                                                                      | 5/5                                  | supported, including password-protected RAR                                                                             |
| CHM                                                                                        | ChmFile → in-memory EPUB → EngineMupdf                                                  | issue-2737.chm, issue-chm-lzx.chm (malformed), sample.chm (ToC, image, CSS, links), large.chm (1025 topics), tricky.chm (windows-1253 and `<?xml encoding?>` pages, `%` in a name, empty page + CSS, backslash / case ToC links, 100-level ToC) | macOS: 4/4 (round 7); Linux: 8/8     | supported; pages are converted to UTF-8                                                                                 |
| DjVu                                                                                       | EngineDjvuDec                                                                           | sample.djvu (IW44 + JB2 pages, text layer, outline), corrupt, regress/djvu-huge-iw44-layer.djvu                                                                                                                                                 | macOS 2/2 (round 7); Linux 3/3       | supported; a damaged page opens and fails to render quickly (0.2 s)                                                     |
| LIT                                                                                        | LIT → in-memory EPUB → EngineMupdf                                                      | sample.lit (2 chapters, image, internal link; uncompressed, written by `makeLit()` in fixture-lib.ts)                                                                                                                                           | Linux 1/1                            | supported for uncompressed, DRM-free books; no LZX-compressed or DRM fixture                                            |
| PalmDoc, TXT, Markdown, HTML, SVG                                                          | EngineMupdf                                                                             | sample.pdb/.txt/.md/.html/.svg                                                                                                                                                                                                                  | 5/5                                  | supported                                                                                                               |
| PNG, BMP, TGA, JPEG, WebP, JXL, JPEG 2000, HEIC, AVIF, GIF, multi-page TIFF, image folders | EngineImage / EngineImageDir (MuPDF loaders, own TGA / WebP / JXL / HEIC+AVIF decoders) | generated (sample.gif, animated.gif, sample.jp2/.heic/.avif, image-folder/) + repo images, mixed-images.cbz (one page per format)                                                                                                               | macOS: 15/15 (round 7); Linux: 15/15 | supported; animated GIF: one page per frame (as on Windows)                                                             |
| JPEG XR, ICO                                                                               | none                                                                                    | none                                                                                                                                                                                                                                            | —                                    | not supported on mac (no decoder); not offered in the Open panel. PNM / PSD are no Sumatra file types (Windows neither) |
| TCR                                                                                        | treated as TXT: shows compressed bytes                                                  | none                                                                                                                                                                                                                                            | —                                    | not supported                                                                                                           |
| PostScript                                                                                 | none (Windows uses Ghostscript's DLL)                                                   | sample.ps                                                                                                                                                                                                                                       | fails cleanly                        | not supported, not a target                                                                                             |

"macOS (round 7)": GitHub Actions macOS arm64 run of port mirror 5480cd0. "Linux": `-mac-core` gcc debug build in WSL,
2026-09-27 (fixtures added after round 7: tricky.chm, drm.azw, encrypted.mobi, the 500-chapter MOBI).

### MOBI, CHM and image notes

- MOBI: `<a filepos>` links and the book's ToC page become EPUB links and an NCX outline. Books with more than 512 KB
  of text are split at top-level `<mbp:pagebreak>`s into EPUB chapters that MuPDF lays out on demand (2 MB book,
  debug build: open 1.3 s → 0.1 s). KF8 (AZW3) text isn't reassembled from its skeleton / fragments; only an
  image-only AZW3 fixture exists.
- CHM: every page is converted to UTF-8 (declared charset, else the CHM's code page) and its declaration rewritten;
  `\` and `ms-its:…::` in `href` / `src` are fixed; the ToC becomes an NCX (at most 64 levels); empty or unreadable
  pages stay as empty pages. CHMs made by FPC's chmcmd need the `ext/chmdec` fix noted in `ext/versions.txt`.
  large.chm (1025 topics): opens in 180 ms, peak RSS 145 MB (Linux debug).
- Images: JPEG XR and ICO have no decoder outside Windows (WIC) and aren't offered. Animated GIF: one page per frame,
  composited with the frames' disposal modes.
- Upstream candidate (not changed; shared with Windows): EngineMupdf's chaptered EPUB layout ignores
  `page-break-before:always`. An EPUB chapter `<p>A</p><p style="page-break-before:always">B</p>` lays out as one
  page; `page-break-after:always` on an element works (the MOBI converter uses that).

## Robustness (fuzzing)

`tests/mac/fuzz-engine.ts` corrupts every fixture (truncation, bit flips, random / zeroed ranges, duplicated or cut
chunks, extreme values in length / offset / count fields near zip, RAR, 7z, PDF, PalmDB, LIT, DjVu, PNG, GIF, BMP
signatures, PDF numbers) and runs the driver with `-fuzz` (every page rendered, short waits, a 40 s watchdog that
prints the stuck call's stack). Crashes, sanitizer reports and hangs are saved with a note under
`tests/tmp/mac-fuzz/repro/`.

- 2026-09-27, Linux ASan: 12,400 variants (seed 11) found 3 problems, all fixed and kept as `regress/` fixtures:
  a debug-build abort in MuPDF's `fz_strncasecmp` for any 1-byte vs multi-byte character (EPUB / CHM link targets;
  `ext/patches/0044`), and a damaged DjVu IW44 header making one page render take a minute (`ext/djvudec`, noted in
  `ext/versions.txt`).
- Also hardened: `DocumentLayout` caps page pixel sizes and positions (a CBZ of pages claiming 2130706432 px
  overflowed the canvas), `ReaderModel` caps absurd mediaboxes, the bridge renders on the engine itself when it
  can't be copied (password-protected CBZ).
- CI (non-gating): `bun tests/mac/fuzz-engine.ts --driver out/mac-core-dbg-clang/test_mac_engine --ci` (fixed seed,
  3 variants of every fixture up to 40 KB). Longer runs: `--count 300 --seed <n> --jobs 24`.

## Fixtures and tests

- Fixtures: `tests/mac/fixtures/` (generators `tests/mac/make-fixtures.ts`, `tests/mac/make-extra-fixtures.ts` + `tests/mac/fixture-lib.ts`; tool
  provenance in its header) plus repo fixtures referenced by path; expectations in `tests/mac/fixtures/manifest.json`.
- Driver: `src/tools/test_mac_engine.cpp`, plain C use of `SumatraMacEngine.h` like `SumatraMac.mm`: open (passwords
  via `MacSetPasswordCallback`), page sizes, layout, sync / async render, find, mouse and select-all selection, ToC,
  link hit-testing, properties, stress (reset, cancel, close with pending renders). It first waits for chaptered
  documents' page count to become final (`MacPageCountIsFinal()`), running the posted page-ready tasks the way the
  app's run loop does.
- Run (Linux / macOS, repo root): `bun cmd/build.ts -mac-core -asan` then
  `bun tests/mac/run-engine-tests.ts --driver out/mac-core-asan-gcc/test_mac_engine` (macOS: `bun cmd/build.ts
-mac-core`, driver `out/mac-core-dbg-clang/test_mac_engine`). `--only <id>` restricts, `--json <file>` saves the
  reports.
