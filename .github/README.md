<div align="center">

# 📖 SumatraPDF for macOS

### Read PDFs, ebooks and comics natively on your Mac.

**20+ document formats in one app · Native on Apple silicon and Intel · Free and open source**

[![Download](https://img.shields.io/github/v/release/Hydra-Of-Malice/Mac-pdf-reader?style=for-the-badge&label=download&color=2f6fde)](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/releases/latest)
![Platform](https://img.shields.io/badge/macOS-11%2B-555?style=for-the-badge&logo=apple)
![Chips](https://img.shields.io/badge/Apple%20silicon%20%7C%20Intel-supported-1a7f37?style=for-the-badge)
![License](https://img.shields.io/badge/license-GPLv3-9a6700?style=for-the-badge)

<img src="https://raw.githubusercontent.com/Hydra-Of-Malice/Mac-pdf-reader/main/docs/mac/screenshot-main.png" width="720" alt="SumatraPDF for macOS showing a PDF with the page toolbar, search field and macOS menu bar">

</div>

SumatraPDF for macOS is a native Mac build of the [SumatraPDF](https://www.sumatrapdfreader.org) reader. Open papers and manuals as PDF, novels as EPUB or MOBI, comics as CBZ or CBR, scanned books as DjVu, and old Windows help files as CHM, all in one app with tabs. It is an unofficial port and is not supported by the SumatraPDF project.

## 💡 Why you'll like it

|                                    |                                                                                                     |
| ---------------------------------- | --------------------------------------------------------------------------------------------------- |
| 📚 **One app, many formats**       | PDF, EPUB, MOBI, AZW, CBZ, CBR, DjVu, CHM, XPS, FB2, LIT and common images open in the same window. |
| 🗂️ **Tabs and a sidebar**          | Keep several documents open and jump around with the outline or page thumbnails.                    |
| 🔍 **Search that keeps up**        | Find text with ⌘F, ⌘G and ⇧⌘G; long documents are searched in the background.                       |
| 🔖 **Picks up where you left off** | Page, zoom, rotation and position within the page come back when you reopen a file.                 |
| 🔐 **Opens protected files**       | You get a password prompt for encrypted PDFs and password-protected CBZ and CBR comics.             |
| 💻 **Feels like a Mac app**        | Standard menus, Preview-style shortcuts, trackpad pinch zoom and printing through the macOS dialog. |
| 🆓 **Free and open source**        | GPLv3. The full source of every release is in this repository.                                      |

## 🚀 Three steps

<img src="https://raw.githubusercontent.com/Hydra-Of-Malice/Mac-pdf-reader/main/docs/mac/screenshot-epub.png" width="720" alt="An EPUB chapter with an image and links rendered in SumatraPDF for macOS">

1. **Download the app.** Get the `.dmg` for your Mac from the Releases page.
2. **Drag it to Applications.** Open the `.dmg` and drag SumatraPDF into the Applications folder.
3. **Open your files.** Double-click a document, drop it on the window, or use File ▸ Open.

## 📥 Download

1. Open the [latest release](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/releases/latest).
2. Download `SumatraPDF-3.7-mac-arm64.dmg` for an Apple silicon Mac (M1, M2, M3, M4) or `SumatraPDF-3.7-mac-x64.dmg` for an Intel Mac. To check, choose Apple menu ▸ About This Mac and look at "Chip" or "Processor".
3. Open the `.dmg` and drag SumatraPDF to Applications.
4. Start SumatraPDF. The app is not notarized by Apple, so the first launch is blocked with a message that macOS cannot verify it. Open System Settings ▸ Privacy & Security, scroll down and click **Open Anyway**. Or run this once in Terminal:

   ```bash
   xattr -dr com.apple.quarantine /Applications/SumatraPDF.app
   ```

5. Optional: to make it the default reader, select a file in Finder, press ⌘I, choose SumatraPDF under "Open with" and click **Change All…**

The release page lists SHA-256 checksums for both downloads.

| Requirement   | Details                                                                           |
| ------------- | --------------------------------------------------------------------------------- |
| macOS         | 11 Big Sur or later. Tested on macOS 15.7.                                        |
| Mac           | Apple silicon (arm64) or Intel (x86_64). Each has its own download.               |
| Disk          | About 12 MB to download, about 20 MB installed.                                   |
| Internet      | Not needed.                                                                       |
| Not supported | JPEG XR, ICO, TCR and PostScript files. Editing annotations and filling in forms. |

## 🔍 What it does

| Stage         | What happens                                                                                                                                                     |
| ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Open          | The reader is chosen from the file extension. MOBI, CHM and LIT books are converted to EPUB in memory. Large files open in the background while a spinner shows. |
| Render        | Visible pages render on background threads. Pages you scroll past are cancelled, and a size-limited cache keeps recent pages.                                    |
| Read          | Continuous or single-page view, zoom, fit page, fit width, rotation, full screen, clickable links, back and forward.                                             |
| Find and copy | Search results are highlighted; ⌘G and ⇧⌘G move between them. Select text with the mouse or ⌘A and copy it.                                                      |
| Print         | Pages go through the standard macOS print dialog, which also offers Save as PDF.                                                                                 |
| Remember      | Recent files, reading position, zoom and rotation are saved per document.                                                                                        |

## ⚙️ How it works

```text
 Finder, File ▸ Open, drag & drop, command line
                       │
                       ▼
 AppKit app (src/mac/*.mm) ──► C bridge (src/mac/SumatraMacEngine.h)
                                          │
              ┌───────────────────────────┼──────────────────────────┐
              ▼                           ▼                          ▼
   MuPDF engine: PDF, EPUB,     DjVu, image and comic      MOBI, CHM, LIT ──► EPUB
   XPS, FB2, HTML, Markdown     archive engines            in memory ──► MuPDF
              └───────────────────────────┼──────────────────────────┘
                                          ▼
                     background render service + page cache
```

| Component                        | Purpose                                                 | License                                           |
| -------------------------------- | ------------------------------------------------------- | ------------------------------------------------- |
| SumatraPDF (`src/`, `src/mac/`)  | Reader model, engines, macOS app                        | GPL-3.0 (parts BSD-2-Clause)                      |
| MuPDF, extract, jbig2dec         | PDF, EPUB, XPS, FB2, HTML rendering                     | AGPL-3.0-or-later                                 |
| djvudec, chmdec                  | DjVu and CHM decoding                                   | MIT                                               |
| libarchive, bzip2, liblzma, zlib | CBZ, CB7, CBT and other archives                        | BSD-2-Clause, bzip2, 0BSD, Zlib                   |
| UnRAR                            | CBR and RAR archives, including password-protected ones | UnRAR freeware license                            |
| FreeType, HarfBuzz               | Font rendering and text shaping                         | FTL, MIT                                          |
| libjpeg-turbo, OpenJPEG, libwebp | JPEG, JPEG 2000 and WebP images                         | IJG/BSD-3-Clause, BSD-2-Clause, BSD-3-Clause      |
| heicdec, dav1d, jxldec           | HEIC, AVIF and JPEG XL images                           | AGPL-3.0 or commercial, BSD-2-Clause, none stated |

The full list, with the license file for each part, is in [THIRD-PARTY-LICENSES.md](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/blob/main/docs/mac/THIRD-PARTY-LICENSES.md). The same license texts ship inside the app under `Contents/Resources/Licenses`.

## ⚠️ Known limits

- The app is not notarized by Apple, so the first launch needs **Open Anyway** (see Download).
- Automated tests run on macOS 15.7 only. macOS 11 to 14 have not been tested.
- It has not yet been tested by hand on a physical Mac. Drag and drop from Finder, VoiceOver, real printers and trackpad pinch are not verified.
- Newer Kindle books in KF8 format (most `.azw3` text books) are not reassembled. Only the older MOBI part of such a book is shown.
- Annotations and form fields are displayed but cannot be edited, filled in or saved.
- JPEG XR, ICO, TCR and PostScript files are not supported.
- A page that fails to render stays blank. There is no error message on the page.
- Dark mode is not in this release. It is being worked on for the next one.
- Some damaged files use a lot of memory, for example a DjVu file that claims a page of almost two billion pixels.
- Reloading a document after it changes on disk briefly pauses the window.
- UnRAR's license is not compatible with the GPL, and jxldec has no stated license. Check both before you redistribute the app.

## 🛠️ Development

Prerequisites: a Mac with macOS 11 or later, the Xcode Command Line Tools (`xcode-select --install`) and [bun](https://bun.sh).

```bash
git clone https://github.com/Hydra-Of-Malice/Mac-pdf-reader.git
cd Mac-pdf-reader
bun cmd/build.ts -mac -rel
open out/mac-rel-arm64/SumatraPDF.app
```

On an Intel Mac the output folder is `out/mac-rel-x64`. Other options: `-dbg` for a debug build, `-arch universal` for one binary that runs on both chip types, and `-dmg` for a disk image.

Run the tests:

```bash
bun cmd/build.ts -mac-core
bun tests/mac/run-engine-tests.ts --driver out/mac-core-dbg-clang/test_mac_engine
```

The first command builds the document engines and runs the unit tests; it also works on Linux. The second opens every test document through the same bridge the app uses. The app has its own end-to-end test, `-self-test`, described in [BUILDING.md](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/blob/main/docs/mac/BUILDING.md).

| Folder / file               | Contents                                                                  |
| --------------------------- | ------------------------------------------------------------------------- |
| `src/mac/`                  | The Cocoa app, the C bridge to the engines, settings, sidebar, thumbnails |
| `src/gui/mac/`              | Password dialog and main-thread helpers                                   |
| `src/**/*_posix.cpp`        | macOS and Linux versions of Windows-only base code                        |
| `cmd/helper/mac-build.ts`   | The macOS build, bundling, signing and packaging                          |
| `tests/mac/`                | Test documents, the format test runner and the fuzzer                     |
| `docs/mac/`                 | Build guide, formats, architecture, keyboard shortcuts, licenses          |
| `.github/workflows/mac.yml` | CI: Apple silicon and Intel builds, app self-test, Windows regression     |

The Windows app still builds from the same source with `bun cmd/build.ts -dbg`. See [BUILDING.md](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/blob/main/docs/mac/BUILDING.md) for signing, notarization and packaging.

## 📄 License

SumatraPDF is licensed under the [GPLv3](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/blob/main/COPYING), with some code under the [BSD license](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/blob/main/COPYING.BSD). The app includes AGPLv3 components such as MuPDF. See the [third-party notices](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/blob/main/docs/mac/THIRD-PARTY-LICENSES.md). SumatraPDF is written by Krzysztof Kowalczyk and [contributors](https://github.com/Hydra-Of-Malice/Mac-pdf-reader/blob/main/AUTHORS).
