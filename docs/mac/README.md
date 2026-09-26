# SumatraPDF for macOS

Native AppKit reader that reuses SumatraPDF's portable engines, layout, rendering, search and settings code.

| Document                                             | Content                                                            |
| ---------------------------------------------------- | ------------------------------------------------------------------ |
| [BUILDING.md](BUILDING.md)                           | requirements, build commands, outputs, running, packaging, signing |
| [architecture.md](architecture.md)                   | how the port is structured                                         |
| [formats.md](formats.md)                             | per-format status, engine tests and fixtures                       |
| [annotations-forms.md](annotations-forms.md)         | annotation and form support                                        |
| [MANUAL-TEST-CHECKLIST.md](MANUAL-TEST-CHECKLIST.md) | manual checks before a release                                     |
| [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md)   | linked libraries, shipped license texts, redistribution duties     |

History of the port: [mac-port-plan.md](../mac-port-plan.md), [mac-port-progress.md](../mac-port-progress.md).

## Where things live

| Path                        | Content                                                                                                                         |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `src/mac/`                  | Cocoa app (`SumatraMac.mm`), plain-C engine bridge, settings                                                                    |
| `src/mac/Resources/`        | `Info.plist` template, `SumatraPDF.icns`, license texts not vendored in `ext/`, `package-README.md` (README.md of the packages) |
| `src/gui/mac/`              | Cocoa implementations of the portable GUI interfaces                                                                            |
| `src/**/*_posix.cpp`        | POSIX implementations shared with other Unix-like targets                                                                       |
| `cmd/helper/mac-build.ts`   | macOS build (`bun cmd/build.ts -mac`)                                                                                           |
| `cmd/helper/mac-bundle.ts`  | Info.plist, icon, licenses, source offer, `.tar.gz` / `.zip` / `.dmg`                                                           |
| `cmd/gen-mac-icon.ts`       | generates `SumatraPDF.icns` from `src/gfx/SumatraPDF-smaller.ico`                                                               |
| `tests/mac/`                | engine test fixtures and runner                                                                                                 |
| `.github/workflows/mac.yml` | macOS CI: arm64 and x64 builds, packages uploaded as artifacts                                                                  |
