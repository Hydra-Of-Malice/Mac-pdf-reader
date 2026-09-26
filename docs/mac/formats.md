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

| Format                                                   | Engine on mac                                 | Fixtures                                                                                             | Result        | Verdict                                                                |
| -------------------------------------------------------- | --------------------------------------------- | ---------------------------------------------------------------------------------------------------- | ------------- | ---------------------------------------------------------------------- |
| PDF (incl. AES-128/256 passwords, malformed, 2000 pages) | EngineMupdf                                   | text.pdf, encrypted-aes\*.pdf, truncated/garbage/empty/header-only, generated 2000-page, 3 repo PDFs | 14/14         | supported                                                              |
| XPS / OXPS                                               | EngineMupdf                                   | sample.xps (text, outline, internal + URL links), corrupt.xps                                        | 2/2           | supported                                                              |
| EPUB                                                     | EngineMupdf (chaptered, lazy layout)          | sample.epub, issue-5846/6095.epub, corrupt, empty                                                    | 5/5           | supported                                                              |
| FB2 / FBZ                                                | EngineMupdf (FBZ unzipped first)              | sample.fb2/.fbz, issue-2254/5792.fb2                                                                 | 4/4           | supported                                                              |
| MOBI / AZW / AZW3                                        | MobiDoc → in-memory EPUB → EngineMupdf        | sample.mobi (PalmDOC + image), uncompressed.azw, issue-4315.azw3, corrupt                            | 4/4           | supported; no HUFF/CDIC-compressed fixture yet                         |
| AZW4 (Print Replica)                                     | embedded PDF → EngineMupdf                    | none                                                                                                 | —             | untested                                                               |
| CBZ / CB7 / CBT                                          | EngineCbx (libarchive)                        | sample.cbz/.cb7/.cbt, issue-1201.cbz, corrupt, empty                                                 | 6/6           | supported                                                              |
| CBR                                                      | EngineCbx (libarchive; UnRAR is Windows-only) | RAR4 + RAR5 samples, password.cbr, corrupt                                                           | 4/5           | supported; password-protected RAR fails (libarchive can't decrypt RAR) |
| CHM                                                      | ChmFile → in-memory EPUB → EngineMupdf        | issue-2737.chm, issue-chm-lzx.chm (malformed)                                                        | 2/2           | supported (tested on a small fixture; no large-CHM fixture yet)        |
| DjVu                                                     | EngineDjvuDec                                 | sample.djvu (IW44 + JB2 pages, text layer, outline), corrupt                                         | 2/2           | supported; corrupt.djvu opens but its page doesn't render (no hang)    |
| LIT                                                      | LIT → in-memory EPUB → EngineMupdf            | none                                                                                                 | —             | untested                                                               |
| PalmDoc, TXT, Markdown, HTML, SVG                        | EngineMupdf                                   | sample.pdb/.txt/.md/.html/.svg                                                                       | 5/5           | supported                                                              |
| PNG, BMP, TGA, JPEG, WebP, JXL, multi-page TIFF          | EngineImage                                   | generated + repo images                                                                              | 7/7           | supported                                                              |
| GIF                                                      | EngineImage                                   | issue-6150.gif                                                                                       | 0/1           | unverified: the fixture itself is a broken GIF; needs a valid one      |
| HEIC / AVIF, JPEG XR                                     | EngineImage                                   | none                                                                                                 | —             | untested                                                               |
| TCR                                                      | treated as TXT: shows compressed bytes        | none                                                                                                 | —             | not supported                                                          |
| PostScript                                               | none (Windows uses Ghostscript's DLL)         | sample.ps                                                                                            | fails cleanly | not supported, not a target                                            |

## Fixtures and tests

- Fixtures: `tests/mac/fixtures/` (generator `tests/mac/make-fixtures.ts` + `tests/mac/fixture-lib.ts`; tool
  provenance in its header) plus repo fixtures referenced by path; expectations in `tests/mac/fixtures/manifest.json`.
- Driver: `src/tools/test_mac_engine.cpp`, plain C use of `SumatraMacEngine.h` like `SumatraMac.mm`: open (passwords
  via `MacSetPasswordCallback`), page sizes, layout, sync / async render, find, mouse and select-all selection, ToC,
  link hit-testing, properties, stress (reset, cancel, close with pending renders).
- Run (Linux / macOS, repo root): `bun cmd/build.ts -mac-core -asan` then
  `bun tests/mac/run-engine-tests.ts --driver out/mac-core-asan-gcc/test_mac_engine` (macOS: `bun cmd/build.ts
-mac-core`, driver `out/mac-core-dbg-clang/test_mac_engine`). `--only <id>` restricts, `--json <file>` saves the
  reports.
