# Licenses of the macOS app

`SumatraPDF.app` is a single statically linked executable. The combined work is distributed under the GNU GPL v3,
and it contains AGPL v3 components (MuPDF, extract, jbig2dec). This page lists what is linked in, where each license
text lives in the repository, what the bundle ships, and what a redistributor has to do. It is not legal advice.

## What the build ships

`addBundleResources()` in `cmd/helper/mac-bundle.ts` (called from `buildMacApp()` in `cmd/helper/mac-build.ts`)
writes `SumatraPDF.app/Contents/Resources/Licenses/`:

| File                    | Content                                                                         |
| ----------------------- | ------------------------------------------------------------------------------- |
| `COPYING`               | GPL v3, the license of SumatraPDF                                               |
| `COPYING.BSD`           | BSD 2-clause license of `src/base` and other files marked BSD in their header   |
| `AUTHORS`               | SumatraPDF authors and third-party credits                                      |
| `INDEX.txt`             | table of all components, their licenses and license files, required credits     |
| `SOURCE.txt`            | source offer: repository URL, exact commit, version, whether the tree was dirty |
| `<component>/<file...>` | license texts of each third-party component linked into the app (table below)   |

The component list is derived from the `.a` libraries on the app's link line (`portableEngineLinkArgs()` in
`mac-build.ts`). The build fails if a linked library has no entry in `libComponents` in `mac-bundle.ts`, so a new
dependency cannot ship without its license. The `.tar.gz` / `.zip` / `.dmg` packages also carry `COPYING`,
`SOURCE.txt` and `README.md` (`src/mac/Resources/package-README.md`) next to the app.

`SOURCE.txt` is filled from git: `git remote get-url origin` (credentials stripped; non-http remotes fall back to
`https://github.com/sumatrapdfreader/sumatrapdf`), `git rev-parse HEAD` and `git status --porcelain`. A build from
a dirty tree or outside git says so in `SOURCE.txt` and prints a note during the build.

## Components linked into SumatraPDF.app

Versions are from `ext/versions.txt` (or the `version.txt` of the `ext/a-*` amalgamation).

| Static library       | Component                                   | Version       | License                         | License text in the repo                                       |
| -------------------- | ------------------------------------------- | ------------- | ------------------------------- | -------------------------------------------------------------- |
| app objects          | SumatraPDF (`src/`, `src/mac`, `src/gui`)   | 3.7           | GPL-3.0 (BSD-2-Clause per file) | `COPYING`, `COPYING.BSD`                                       |
| `libbase.a`          | SumatraPDF `src/base`, `src/gui/Layout.cpp` | 3.7           | BSD-2-Clause                    | `COPYING.BSD`                                                  |
| `libmupdf.a`         | MuPDF (+ `ext/patches`, `src/mupdf`)        | 1.28.2        | AGPL-3.0-or-later               | `ext/mupdf/COPYING`                                            |
| `libmupdf.a`         | MuPDF built-in fonts: URW base fonts        | -             | OFL-1.1                         | `ext/mupdf/resources/fonts/urw/OFL.txt`                        |
| `libmupdf.a`         | MuPDF built-in fonts: Charis SIL            | -             | OFL-1.1                         | `ext/mupdf/resources/fonts/sil/OFL.txt`                        |
| `libmupdf.a`         | MuPDF built-in fonts: Noto                  | -             | OFL-1.1                         | `ext/mupdf/resources/fonts/noto/COPYING`                       |
| `libmupdf.a`         | MuPDF built-in fonts: Droid Sans Fallback   | -             | Apache-2.0                      | `ext/mupdf/resources/fonts/droid/NOTICE`                       |
| `liba-extract.a`     | extract                                     | trunk 8750ac3 | AGPL-3.0-or-later               | `ext/mupdf/COPYING` (same text as upstream `COPYING`)          |
| `liba-jbig2dec.a`    | jbig2dec                                    | 0.20          | AGPL-3.0-or-later               | `ext/a-jbig2dec/COPYING`, `ext/a-jbig2dec/LICENSE`             |
| `liba-mujs.a`        | MuJS                                        | 1.3.9         | ISC                             | `ext/a-mujs/COPYING`                                           |
| `libfreetype.a`      | FreeType                                    | 2.14.3        | FTL (or GPL-2.0-or-later)       | `ext/a-freetype/LICENSE.TXT`, `ext/a-freetype/docs/FTL.TXT`    |
| `libharfbuzz.a`      | HarfBuzz                                    | 13.0.1        | MIT-Modern-Variant              | `ext/a-harfbuzz/COPYING`                                       |
| `liblcms2.a`         | Little CMS (lcms2mt)                        | 2.19.1        | MIT                             | `ext/a-lcms2/LICENSE`                                          |
| `liba-openjpeg.a`    | OpenJPEG                                    | 2.5.4         | BSD-2-Clause                    | `ext/a-openjpeg/LICENSE`                                       |
| `liblibjpeg-turbo.a` | libjpeg-turbo                               | 3.1.4.1       | IJG AND BSD-3-Clause AND Zlib   | `ext/libjpeg-turbo/LICENSE.md`, `ext/libjpeg-turbo/README.ijg` |
| `liblibwebp.a`       | libwebp                                     | 1.6.0         | BSD-3-Clause                    | `ext/a-libwebp/COPYING`                                        |
| `libbrotli.a`        | Brotli                                      | 1.2.0         | MIT                             | `ext/a-brotli/LICENSE`                                         |
| `liba-gumbo.a`       | Gumbo HTML parser (Artifex fork)            | 0.10.1        | Apache-2.0                      | `src/mac/Resources/Licenses/gumbo/COPYING`                     |
| `libcmark-gfm.a`     | cmark-gfm                                   | 0.29.0.gfm.13 | BSD-2-Clause AND MIT            | `ext/cmark-gfm/COPYING`                                        |
| `liblibarchive.a`    | libarchive                                  | 3.8.8         | BSD-2-Clause                    | `ext/a-libarchive/COPYING`                                     |
| `liblibarchive.a`    | bzip2                                       | 1.0.8         | bzip2-1.0.6                     | `ext/a-bzip2/LICENSE`                                          |
| `liblibarchive.a`    | liblzma (XZ Utils, decoder subset)          | -             | 0BSD                            | SPDX headers in `ext/liblzma` (0BSD requires no notice)        |
| `liba-zlib.a`        | zlib                                        | 1.3.2         | Zlib                            | `ext/a-zlib/LICENSE`                                           |
| `libchmdec.a`        | chmdec                                      | dbf9c5a       | MIT                             | `ext/chmdec/LICENSE.md`                                        |
| `libdjvudec.a`       | djvudec                                     | c8bf1b3       | MIT                             | `src/mac/Resources/Licenses/djvudec/LICENSE.md`                |
| `libmsdes.a`         | D3DES (msdes)                               | 5.09          | public domain                   | `ext/msdes/README.md`                                          |

Libraries `mac-build.ts` compiles but does not link into the app today. `mac-bundle.ts` has entries for them, so
they are covered if they get linked:

| Static library | Component | License                              | License text / concern                                                             |
| -------------- | --------- | ------------------------------------ | ---------------------------------------------------------------------------------- |
| `liba-unrar.a` | UnRAR     | UnRAR freeware license               | `ext/a-unrar/license.txt`; forbids re-creating RAR compression, not GPL-compatible |
| `libdav1d.a`   | dav1d     | BSD-2-Clause                         | `ext/dav1d/COPYING`                                                                |
| `libheicdec.a` | heicdec   | AGPL-3.0 or commercial (imazen/heic) | `src/mac/Resources/Licenses/heicdec/LICENSE.md`; HEVC may be patent-encumbered     |
| `libjxldec.a`  | jxldec    | none stated upstream                 | resolve with the author before shipping a build that links it                      |

System frameworks and libraries (Cocoa, libc++, libiconv, libSystem) are linked dynamically, are part of macOS, and
are not redistributed; the GPL's System Libraries exception covers them.

License texts not vendored in `ext/` are kept in `src/mac/Resources/Licenses/`, copied from the upstream repository at
the commit recorded in `ext/versions.txt` / `version.txt`:

- `gumbo/COPYING`: `github.com/ArtifexSoftware/thirdparty-gumbo-parser` at `3973c58d759574f2899528d2b3379e17d66dbcad`
- `djvudec/LICENSE.md`: `github.com/kjk/djvudec` at `c8bf1b3e6704fd99d6feaba8d8ebc417e013bdb8`
- `heicdec/LICENSE.md`: `github.com/kjk/heicdec` at `c0aa68eb43de852706069755b0132ac2ec951dc2`

Credits the licenses require in documentation are in `INDEX.txt`: FreeType (FTL), the Independent JPEG Group
(libjpeg-turbo), and the application icon (by Alex, CC BY 3.0, see `AUTHORS`; converted to ICNS by
`cmd/gen-mac-icon.ts`).

## Obligations when redistributing SumatraPDF.app

1. **License.** Distribute under GPL v3. MuPDF, extract and jbig2dec stay under AGPL v3; GPL v3 section 13 allows
   the combination, and AGPL v3 section 13 (source for users interacting over a network) applies to modified
   versions. Do not add terms that restrict recipients (an EULA, no-reverse-engineering clauses). Mac App Store terms
   add such restrictions, so it is not a suitable channel.
2. **Corresponding source.** Everyone who gets the binary must be able to get the complete source of that exact
   build: SumatraPDF, all of `ext/`, and the build scripts (`cmd/`). Ship it alongside, include a written offer
   valid for 3 years, or offer it from the same place as the binary (GPL v3 / AGPL v3 section 6). A link to the
   exact commit in a public repository that you keep available is the usual way; `SOURCE.txt` records it.
3. **Modified or forked builds.** If `SOURCE.txt` says the tree had local changes, or the commit is only in your
   fork, publish that source and make sure `SOURCE.txt` points to it (it uses the `origin` remote). Mark the
   program as modified (GPL v3 section 5a) and use your own `CFBundleIdentifier` in `src/mac/Resources/Info.plist`.
4. **Notices.** Keep `Contents/Resources/Licenses/` complete and unmodified; it carries the license texts and the
   required credits.
5. **Signing.** Code signing and notarization with your Developer ID are compatible with the GPL: users can still
   build, ad-hoc sign and run modified versions on their Macs.

## Adding a library to the app

1. Add it to `libComponents` in `cmd/helper/mac-bundle.ts`, keyed by the static library name without `lib`, `a-`
   and `.a`, with its license texts.
2. If `ext/` has no license text for it, copy the upstream one into `src/mac/Resources/Licenses/<name>/` and record
   the source commit above.
3. Update the tables on this page.
