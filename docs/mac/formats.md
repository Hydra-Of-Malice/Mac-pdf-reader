# macOS port: document formats

"Supported" means a representative fixture opened, rendered and (where it has text) was searched through
`src/mac/SumatraMacEngine.h` in a recorded run of `tests/mac/run-engine-tests.ts`. A registered extension is not
support. The runs so far are on Linux (WSL Ubuntu 24.04, gcc 13, ASan) via `bun cmd/build.ts -mac-core -asan`; the
same portable code is compiled for macOS but has not run on a Mac yet.

The Cocoa app opens files through `MacOpenDocument()` → `ReaderModel::Create()` (`src/ReaderModel.cpp`), not
Windows' `CreateEngineFromFile()`. `ReaderModel` picks the engine by file extension (no content sniffing). The ebook
engines in `src/EngineEbook.cpp` and the CHM viewers (`ChmModel`, `EngineChm`) are Windows-only (GDI+ / IE), so on
macOS MOBI, CHM and LIT are converted to an in-memory EPUB that MuPDF lays out.

## Formats (last run 2026-09-27: 54/58 fixtures pass)

| Format                                                   | Engine on mac                          | Fixtures                                                                                             | Result        | Verdict                                                                       |
| -------------------------------------------------------- | -------------------------------------- | ---------------------------------------------------------------------------------------------------- | ------------- | ----------------------------------------------------------------------------- |
| PDF (incl. AES-128/256 passwords, malformed, 2000 pages) | EngineMupdf                            | text.pdf, encrypted-aes\*.pdf, truncated/garbage/empty/header-only, generated 2000-page, 3 repo PDFs | 14/14         | supported                                                                     |
| XPS / OXPS                                               | EngineMupdf                            | sample.xps (text, outline, internal + URL links), corrupt.xps                                        | 2/2           | supported                                                                     |
| EPUB                                                     | EngineMupdf (chaptered, lazy layout)   | sample.epub, issue-5846/6095.epub, corrupt, empty                                                    | 4/5           | supported; ToC check on issue-6095 fixed in the driver, not re-run            |
| FB2 / FBZ                                                | EngineMupdf (FBZ unzipped first)       | sample.fb2/.fbz, issue-2254/5792.fb2                                                                 | 4/4           | supported                                                                     |
| MOBI / AZW / AZW3                                        | MobiDoc → in-memory EPUB → EngineMupdf | sample.mobi (PalmDOC + image), uncompressed.azw, issue-4315.azw3, corrupt                            | 4/4           | supported (owner now FORMATS-2); no HUFF/CDIC fixture                         |
| AZW4 (Print Replica)                                     | embedded PDF → EngineMupdf             | none                                                                                                 | —             | untested                                                                      |
| CBZ / CB7 / CBT                                          | EngineCbx (libarchive)                 | sample.cbz/.cb7/.cbt, issue-1201.cbz, corrupt, empty                                                 | 6/6           | supported                                                                     |
| CBR                                                      | EngineCbx (UnRAR)                      | RAR4 + RAR5 samples, password.cbr, corrupt                                                           | 4/5           | supported; password-protected RAR fails (UnRAR on POSIX)                      |
| CHM                                                      | ChmFile → in-memory EPUB → EngineMupdf | issue-2737.chm, issue-chm-lzx.chm (malformed)                                                        | 2/2           | supported on the minimal fixture (owner now FORMATS-2)                        |
| DjVu                                                     | EngineDjvuDec                          | sample.djvu (IW44 + JB2 pages, text layer, outline), corrupt                                         | 1/2           | supported; corrupt.djvu opens, its page never renders (driver now fails fast) |
| LIT                                                      | LIT → in-memory EPUB → EngineMupdf     | none                                                                                                 | —             | untested                                                                      |
| PalmDoc, TXT, Markdown, HTML, SVG                        | EngineMupdf                            | sample.pdb/.txt/.md/.html/.svg                                                                       | 5/5           | supported                                                                     |
| PNG, BMP, TGA, JPEG, WebP, JXL, multi-page TIFF          | EngineImage                            | generated + repo images                                                                              | 7/7           | supported                                                                     |
| GIF                                                      | EngineImage                            | issue-6150.gif                                                                                       | 0/1           | renders blank on POSIX (owner FORMATS-2)                                      |
| HEIC / AVIF, JPEG XR                                     | EngineImage                            | none                                                                                                 | —             | untested                                                                      |
| TCR                                                      | treated as TXT: shows compressed bytes | none                                                                                                 | —             | not supported                                                                 |
| PostScript                                               | none (Windows uses Ghostscript's DLL)  | sample.ps                                                                                            | fails cleanly | not supported, not a target                                                   |

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
