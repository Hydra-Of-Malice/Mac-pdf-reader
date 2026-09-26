# Building SumatraPDF for macOS

## Requirements

- A Mac with macOS 11 or later (Apple silicon or Intel). `cmd/helper/mac-build.ts` exits on other systems.
- Xcode, or just the Command Line Tools: `xcode-select --install`. The build uses `clang`, `clang++`, `ar`, `ld`,
  `dsymutil`, `lipo`; packaging uses `tar`, `ditto` and `hdiutil`, which ship with macOS.
- [bun](https://bun.sh): `curl -fsSL https://bun.sh/install | bash` or `brew install oven-sh/bun/bun`.

## Build

```sh
bun cmd/build.ts -mac -dbg     # debug
bun cmd/build.ts -mac -rel     # release
bun cmd/build.ts -mac -asan    # AddressSanitizer
```

Add `-clean` to delete the output directory first. The build targets the architecture bun runs as (`process.arch`:
`arm64` or `x64`). If `bun cmd/build.ts -help` does not list `-mac` yet, call the helper directly, as CI does:

```sh
bun -e 'import { buildMac } from "./cmd/helper/mac-build.ts"; await buildMac({ outDir: "out/mac-rel64", isRelease: true });'
```

`buildMac()` in `cmd/helper/mac-build.ts` runs these steps:

1. compiles the `ext/` dependencies and `src/base` into static libraries in `<out>/lib/`
2. builds `test_util` and runs it with `-for-ai` (portable unit tests); a failure stops the build
3. compile-checks the portable sources in `PORTABLE_COMPILE_SOURCES`
4. builds `test_engines`
5. compiles and links `SumatraPDF.app` (`MAC_APP_SOURCES`), writes its `.dSYM`, and adds the bundle resources
6. creates the distribution packages

## Output

| Config         | Directory             | Package name                               |
| -------------- | --------------------- | ------------------------------------------ |
| debug          | `out/mac-dbg64/`      | `SumatraPDF-<ver>-mac-<arch>-debug`        |
| release        | `out/mac-rel64/`      | `SumatraPDF-<ver>-mac-<arch>`              |
| asan           | `out/mac-asan64/`     | `SumatraPDF-<ver>-mac-<arch>-asan`         |
| release + asan | `out/mac-rel64_asan/` | `SumatraPDF-<ver>-mac-<arch>-release-asan` |

`<ver>` is `CURR_VERSION` from `src/Version.h`; `<arch>` is `arm64` or `x64`. Each directory contains:

- `SumatraPDF.app` and `SumatraPDF.app.dSYM`
- `test_util`, `test_engines` (with `.dSYM`), `lib/*.a`, `obj/`
- `<package>.tar.gz`; on macOS also `<package>.zip` (`ditto -c -k --keepParent`); for release builds also
  `<package>.dmg` (`hdiutil create -volname SumatraPDF -srcfolder <staging> -ov -format UDZO`, with an
  `Applications` link for drag-and-drop install). `MacBuildOptions.dmg` in `mac-build.ts` overrides the default.

Each package holds `<package>/SumatraPDF.app`, `README.md` (from `src/mac/Resources/package-README.md`), `COPYING` and
`SOURCE.txt`.

## Bundle contents

```
SumatraPDF.app/Contents/
  Info.plist                 src/mac/Resources/Info.plist with @VERSION@, @BUNDLE_VERSION@, @COPYRIGHT@ filled in
  PkgInfo
  MacOS/SumatraPDF
  Resources/SumatraPDF.icns  src/mac/Resources/SumatraPDF.icns
  Resources/Licenses/        license texts and SOURCE.txt, see THIRD-PARTY-LICENSES.md
```

`cmd/helper/mac-bundle.ts` assembles this right after linking:

- `CFBundleShortVersionString` is `CURR_VERSION`; `CFBundleVersion` is `CURR_VERSION.<build>`, where `<build>` is
  `git rev-list --count HEAD` + 1000 (the numbering of `bun cmd/build.ts -build-no`). Shallow clones give a small
  number; without git it is `CURR_VERSION`.
- `NSHumanReadableCopyright` is `kCopyrightStr` from `src/Version.h`.
- Document types (`CFBundleDocumentTypes`, `UTImportedTypeDeclarations`) are in the template. Keep them in sync with
  [formats.md](formats.md) and the Open panel's file types in `src/mac/SumatraMac.mm`.
- The icon is generated from `src/gfx/SumatraPDF-smaller.ico` by `bun cmd/gen-mac-icon.ts` (no macOS tools needed);
  rerun it when the artwork changes and commit the `.icns`.

Check the result:

```sh
plutil -lint out/mac-rel64/SumatraPDF.app/Contents/Info.plist
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f out/mac-rel64/SumatraPDF.app
```

`lsregister -f` makes Finder pick up changed document types without moving the app.

`LSMinimumSystemVersion` is 11.0. The compiler targets the SDK's macOS version unless told otherwise, so build with
`MACOSX_DEPLOYMENT_TARGET=11.0` (clang and ld read it) for a binary that runs on older macOS versions.

## Run

```sh
open out/mac-dbg64/SumatraPDF.app --args "$PWD/ext/a-zlib/zlib.3.pdf"
open -a "$PWD/out/mac-dbg64/SumatraPDF.app" /path/to/file.epub     # also opens in an already running instance
out/mac-dbg64/SumatraPDF.app/Contents/MacOS/SumatraPDF /path/to/file.pdf   # logs to the terminal
lldb -- out/mac-dbg64/SumatraPDF.app/Contents/MacOS/SumatraPDF /path/to/file.pdf
```

Pass absolute paths: apps started by `open` run with `/` as the working directory. `--args` only reaches a newly
launched instance. Settings are in `~/Library/Application Support/SumatraPDF/SumatraPDF-settings.txt`.

## Tests

- `test_util` runs during every build.
- `out/mac-dbg64/test_engines <file>` opens a document with the portable engines and prints what it finds; it also
  takes `-find-text <text>`, `-list-toc`, `-list-properties`.
- Per-format engine tests and fixtures: [formats.md](formats.md).
- Manual checks before a release: [MANUAL-TEST-CHECKLIST.md](MANUAL-TEST-CHECKLIST.md).
- Without a Mac: `bun cmd/build.ts -mac-remote -branch <temporary-branch> -dbg` builds a pushed branch on the remote
  Mac configured in `cmd/helper/mac-remote-build.ts`, and `bun cmd/build.ts -mac-core` (when `-help` lists it)
  compile-checks the portable core.

## CI

`.github/workflows/mac.yml` runs on every push:

- `cocoa-syntax` (macos-15): `clang++ -fsyntax-only -mmacosx-version-min=11.0` over `src/mac/*.mm`, `src/mac/*.cpp`
  and `src/gui/mac/*` against the real macOS SDK.
- `build` (arm64 on macos-15, x64 on macos-15-intel): `-mac-core` if available, then debug and release builds. The
  `.tar.gz`, `.zip` and `.dmg` packages are uploaded as artifacts `SumatraPDF-mac-arm64` / `SumatraPDF-mac-x64`
  (kept 14 days). They are not Developer ID signed; see Gatekeeper below.

`.github/workflows/mac-daily.yml` runs `bun cmd/build.ts -mac -asan` daily.

## Signing and Gatekeeper

Local builds are not quarantined and run as built. On Apple silicon every executable must carry at least an ad-hoc
signature; the linker ad-hoc signs `Contents/MacOS/SumatraPDF`. To seal the whole bundle (Info.plist and
resources), sign it after the bundle resources are written and before packaging:

```sh
codesign --force --sign - out/mac-rel64/SumatraPDF.app
codesign --verify --strict --verbose=2 out/mac-rel64/SumatraPDF.app
```

A downloaded, unsigned or ad-hoc signed app is quarantined and blocked on first launch. Either remove the quarantine
attribute (`xattr -dr com.apple.quarantine SumatraPDF.app`), or Control-click it and choose **Open** (macOS 14 and
earlier) / click **Open Anyway** in **System Settings > Privacy & Security** (macOS 15 and later).

### Developer ID signing and notarization (optional)

Only needed to distribute builds that open without Gatekeeper warnings. It requires an Apple Developer account and
a "Developer ID Application" certificate in the keychain. The app is not sandboxed and needs no entitlements (MuJS
is an interpreter, not a JIT).

```sh
APP=out/mac-rel64/SumatraPDF.app
ID="Developer ID Application: <Name> (<TEAMID>)"

codesign --force --options runtime --timestamp --sign "$ID" "$APP"
codesign --verify --strict --verbose=2 "$APP"

xcrun notarytool store-credentials sumatra --apple-id <apple-id> --team-id <TEAMID>   # once; asks for an app-specific password
ditto -c -k --keepParent "$APP" notarize.zip
xcrun notarytool submit notarize.zip --keychain-profile sumatra --wait
xcrun stapler staple "$APP"
spctl --assess --type execute --verbose "$APP"
```

The packages the build made contain the app as it was before signing: recreate them from the stapled app, e.g.
`ditto -c -k --keepParent "$APP" SumatraPDF.zip`. For a `.dmg`, create it from the stapled app, then sign, notarize
and staple the `.dmg` itself (`codesign --timestamp --sign "$ID" X.dmg`, `notarytool submit X.dmg --wait`,
`stapler staple X.dmg`).

Before distributing any build, read [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) (source offer, notices).
