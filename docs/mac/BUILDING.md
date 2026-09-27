# Building SumatraPDF for macOS

## Requirements

- macOS 11 or later (Apple silicon or Intel) to build and run the app.
- Xcode, or just the Command Line Tools: `xcode-select --install`. The build uses `clang`, `clang++`, `ar`, `ld`,
  `dsymutil`, `lipo`, `codesign`; packaging uses `tar`, `ditto` and `hdiutil`, which ship with macOS.
- [bun](https://bun.sh): `curl -fsSL https://bun.sh/install | bash` or `brew install oven-sh/bun/bun`.

## Build

```sh
bun cmd/build.ts -mac -dbg                    # debug, host architecture
bun cmd/build.ts -mac -rel                    # release (+ .dmg)
bun cmd/build.ts -mac -rel -arch universal    # arm64 + x86_64 in one binary (lipo)
bun cmd/build.ts -mac -asan                   # AddressSanitizer
```

Options: `-arch arm64|x64|universal` (default: the architecture bun runs as), `-dmg` (force a `.dmg` for non-release
builds), `-clean` (delete the output directory first). Every compile and link line gets
`-mmacosx-version-min=11.0`, matching `LSMinimumSystemVersion` in the bundle.

The build compiles the `ext/` dependencies and `src/base` into static libraries, builds and runs `test_util`
(portable unit tests; a failure stops the build), builds `test_engines`, compiles and links `SumatraPDF.app`, writes
its `.dSYM`, adds the bundle resources and fonts, ad-hoc signs the bundle, and creates the packages.

## Output

| Build        | Directory              | Package name                       |
| ------------ | ---------------------- | ---------------------------------- |
| `-mac -dbg`  | `out/mac-dbg-<arch>/`  | `SumatraPDF-<ver>-mac-<arch>-dbg`  |
| `-mac -rel`  | `out/mac-rel-<arch>/`  | `SumatraPDF-<ver>-mac-<arch>`      |
| `-mac -asan` | `out/mac-asan-<arch>/` | `SumatraPDF-<ver>-mac-<arch>-asan` |

`<ver>` is `CURR_VERSION` from `src/Version.h`; `<arch>` is `arm64`, `x64` or `universal`. Each directory contains
`SumatraPDF.app`, `SumatraPDF.app.dSYM`, `test_util`, `test_engines`, `lib/*.a`, `obj/`, and the packages:
`<package>.tar.gz`, `<package>.zip` (`ditto -c -k --keepParent`) and, for release or `-dmg`, `<package>.dmg`
(`hdiutil create -format UDZO`, with an `Applications` link for drag-and-drop install). Each package holds
`SumatraPDF.app`, `README.md` (from `src/mac/Resources/package-README.md`), `COPYING` and `SOURCE.txt`.

## Bundle contents

```
SumatraPDF.app/Contents/
  Info.plist                 src/mac/Resources/Info.plist with @VERSION@, @BUNDLE_VERSION@, @COPYRIGHT@ filled in
  PkgInfo
  MacOS/SumatraPDF
  Resources/SumatraPDF.icns  src/mac/Resources/SumatraPDF.icns
  Resources/fonts/           MuPDF's base fonts (POSIX builds have no embedded font archive)
  Resources/Licenses/        license texts and SOURCE.txt, see THIRD-PARTY-LICENSES.md
  _CodeSignature/            ad-hoc signature (or Developer ID, see below)
```

`cmd/helper/mac-bundle.ts` assembles this right after linking:

- `CFBundleShortVersionString` is `CURR_VERSION`; `CFBundleVersion` is `CURR_VERSION.<build>`, where `<build>` is
  `git rev-list --count HEAD` + 1000. Shallow clones give a small number; without git it is `CURR_VERSION`.
- Document types (`CFBundleDocumentTypes`, `UTImportedTypeDeclarations`) are in the template. Keep them in sync with
  [formats.md](formats.md) and `MacCopySupportedExtensions()` in `src/mac/SumatraMacEngine.cpp`.
- The icon is generated from `src/gfx/SumatraPDF-smaller.ico` by `bun cmd/gen-mac-icon.ts` (no macOS tools needed).

Check the result:

```sh
plutil -lint out/mac-rel-arm64/SumatraPDF.app/Contents/Info.plist
codesign --verify --strict --verbose=2 out/mac-rel-arm64/SumatraPDF.app
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f out/mac-rel-arm64/SumatraPDF.app
```

`lsregister -f` makes Finder pick up changed document types without moving the app.

## Run

```sh
open out/mac-dbg-arm64/SumatraPDF.app --args "$PWD/tests/mac/fixtures/text.pdf"
open -a "$PWD/out/mac-dbg-arm64/SumatraPDF.app" /path/to/file.epub       # also opens in a running instance
out/mac-dbg-arm64/SumatraPDF.app/Contents/MacOS/SumatraPDF /path/to/file.pdf   # logs to the terminal
out/mac-dbg-arm64/SumatraPDF.app/Contents/MacOS/SumatraPDF -for-testing /path/to/file.pdf  # no session, temp settings
lldb -- out/mac-dbg-arm64/SumatraPDF.app/Contents/MacOS/SumatraPDF /path/to/file.pdf
```

Pass absolute paths: apps started by `open` run with `/` as the working directory. `--args` only reaches a newly
launched instance. Settings are in `~/Library/Application Support/SumatraPDF/`. To install, drag `SumatraPDF.app`
from the `.dmg` (or the unpacked `.zip`) to `/Applications`.

## Tests

- `test_util` (portable unit tests) runs during every `-mac` and `-mac-core` build.
- Portable core without a full app build, on macOS or Linux: `bun cmd/build.ts -mac-core [-dbg|-rel] [-asan]
[-cc gcc|clang|zig]` builds the engines, reader model and `src/mac/*.cpp` plus `test_util`, `test_engines`,
  `test_mac_engine` and `test_mac_thumbnails` into `out/mac-core-<cfg>-<cc>/` and runs `test_util` and
  `test_mac_thumbnails`.
- Without a Mac: `bun cmd/build.ts -mac-core -cross [-arch arm64|x64|universal]` compiles the same portable sources
  to macOS objects with zig. Sources that need Apple frameworks are skipped and listed: the Cocoa `.mm` files and
  the C files that call CoreText / CoreFoundation (see Platform code below).
- Per-format engine tests through the app's bridge (`src/mac/SumatraMacEngine.h`):
  `bun tests/mac/run-engine-tests.ts --driver out/mac-core-dbg-clang/test_mac_engine [--only <id>] [--json f.json]`
  over `tests/mac/fixtures/manifest.json`. See [formats.md](formats.md).
- In-app self-test (drives the real Cocoa app through every manifest fixture: open in the background, render,
  navigation, zoom, rotation, sidebar, find and the find bar, links, select all and copy, print to PDF, reopen):
  `out/mac-dbg-arm64/SumatraPDF.app/Contents/MacOS/SumatraPDF -for-testing -prefs-dir /tmp/sp -self-test
/tmp/sp/report.json -self-test-manifest tests/mac/fixtures/manifest.json` (exit code 0 = pass; PNG snapshots next
  to the report).
  - `-for-testing`: no session restore, user defaults (sidebar, window frame, toolbar) neither read nor written,
    settings in a temporary folder removed on quit.
  - `-prefs-dir <dir>`: keep `SumatraPDF-settings.txt` in `<dir>` instead.
  - `-self-test <report.json>`: run the self-test, write the JSON report and PNGs, quit with 0 (pass) / 1 (fail);
    3 means the watchdog fired. Documents on the command line are tested too (expected to open).
  - `-self-test-manifest <manifest.json>`: expectations (pages, passwords, search words); without paths, every
    fixture in it. `-self-test-find <word>`: search word for documents not in the manifest.
    `-self-test-timeout <seconds>`: watchdog, default 1500.
  - `-appearance system|light|dark` and `-doc-colors normal|smart|invert`: start in that appearance / with those
    document colors (not saved), e.g. for dark mode screenshots.
- Remote Mac over ssh: `SUMATRA_MAC_HOST=user@host SUMATRA_MAC_DIR=src/sumatrapdf bun cmd/build.ts -mac-remote
-branch <pushed-branch> -dbg`.
- Manual checks before a release: [MANUAL-TEST-CHECKLIST.md](MANUAL-TEST-CHECKLIST.md).

## Platform code

- macOS-only C files, compiled only with Apple's SDK and linked with `-framework CoreText -framework
CoreFoundation`: `src/mupdf/mupdf_load_system_font_mac.c` (MuPDF's system-font hooks through CoreText, so PDFs
  with non-embedded fonts use the installed font; the Windows build has `mupdf_load_system_font.c`) and
  `src/base/StrNormalize_mac.c` (Unicode normalization for `NormalizeString()`, used by the PDF password retry).
  They don't include `base/Base.h`: Apple headers clash with it.
- UnRAR: `ext/a-unrar/unrar.cpp` is generated for Windows and includes upstream's Windows-only `isnt.cpp` and
  `motw.cpp`. The build compiles a copy without those two into `out/.../generated/unrar/unrar_posix.cpp` (see
  `posixUnrar()` in `cmd/helper/mac-build.ts`); the build fails if the amalgamation changes so they can't be found.
  UnRAR's license is not GPL-compatible: see [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).
- `ext/a-libarchive` and `ext/a-harfbuzz` are generated for Windows too: the build writes a POSIX
  `config_posix.h` and a few stubs for libarchive, and compiles harfbuzz with `-DHB_NO_VISIBILITY`.
- Threads started with `StartThread()` get an 8 MB stack (macOS gives secondary threads 512 KB).
- Large DjVu files are read into memory, not memory-mapped as on Windows (`gMemoryMapLargeFiles` in
  `src/EngineDjvuDec.cpp`): another process truncating a mapped file would crash the app with SIGBUS.
- `CalcMD5Digest()` / `CalcSHA1Digest()` / `CalcSHA2Digest()` use CommonCrypto on macOS and built-in code on Linux
  (`src/base/Crypto_posix.cpp`).

## CI

`.github/workflows/mac.yml` runs on every push:

- `cocoa-syntax` (macos-15): `clang++ -fsyntax-only -mmacosx-version-min=11.0` over `src/mac/*` and
  `src/gui/mac/*` against the real macOS SDK.
- `windows`: `bun cmd/run-unit-tests.ts -dbg` (the port touches shared code).
- `build` (arm64 on macos-15, x64 on macos-15-intel): `-mac-core`, the format test matrix (reported), `-mac -dbg`, a
  launch smoke test (the app must still run 15 s after opening a PDF; screenshot uploaded), `-mac -rel`, and the
  in-app self-test (gating). Artifacts: `SumatraPDF-mac-<arch>` (`.tar.gz`, `.zip`, `.dmg`), `smoke-<arch>`,
  `selftest-<arch>`, kept 14 days. They are ad-hoc signed only; see Gatekeeper below.

`.github/workflows/mac-daily.yml` runs `bun cmd/build.ts -mac -asan` daily.

## Signing and Gatekeeper

The build ad-hoc signs the finished bundle (`codesign --force --deep -s -`) after the resources are written and
before packaging; Apple silicon requires at least an ad-hoc signature. Set `SUMATRA_MAC_SIGN_IDENTITY` to sign with a
real identity instead. Local builds are not quarantined and run as built.

A downloaded, unsigned or ad-hoc signed app is quarantined and blocked on first launch. Either remove the quarantine
attribute (`xattr -dr com.apple.quarantine SumatraPDF.app`), or Control-click it and choose **Open** (macOS 14 and
earlier) / click **Open Anyway** in **System Settings > Privacy & Security** (macOS 15 and later).

### Developer ID signing and notarization (optional)

Only needed to distribute builds that open without Gatekeeper warnings. It requires an Apple Developer account and
a "Developer ID Application" certificate in the keychain. The app is not sandboxed and needs no entitlements (MuJS
is an interpreter, not a JIT).

```sh
APP=out/mac-rel-arm64/SumatraPDF.app
ID="Developer ID Application: <Name> (<TEAMID>)"

codesign --force --options runtime --timestamp --sign "$ID" "$APP"
codesign --verify --strict --verbose=2 "$APP"

xcrun notarytool store-credentials sumatra --apple-id <apple-id> --team-id <TEAMID>   # once; asks for an app-specific password
ditto -c -k --keepParent "$APP" notarize.zip
xcrun notarytool submit notarize.zip --keychain-profile sumatra --wait
xcrun stapler staple "$APP"
spctl --assess --type execute --verbose "$APP"
```

Or build with `SUMATRA_MAC_SIGN_IDENTITY="$ID"` so the build signs before packaging, then notarize. The packages the
build made contain the app as it was before stapling: recreate them from the stapled app, e.g.
`ditto -c -k --keepParent "$APP" SumatraPDF.zip`. For a `.dmg`, create it from the stapled app, then sign, notarize
and staple the `.dmg` itself (`codesign --timestamp --sign "$ID" X.dmg`, `notarytool submit X.dmg --wait`,
`stapler staple X.dmg`).

Before distributing any build, read [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) (source offer, notices).
