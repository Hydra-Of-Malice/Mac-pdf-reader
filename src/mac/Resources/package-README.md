# SumatraPDF for macOS

`SumatraPDF.app` is a native macOS build of SumatraPDF. It needs macOS 11 or later.

## Install

Move `SumatraPDF.app` to `/Applications`, or run it in place. From the `.dmg`, drag `SumatraPDF.app` onto
`Applications`.

## First launch

Builds that are not signed with a Developer ID and notarized are blocked by Gatekeeper the first time. To allow it:

- macOS 14 and earlier: Control-click `SumatraPDF.app`, choose **Open**, then **Open** again.
- macOS 15 and later: open it once, then go to **System Settings > Privacy & Security** and click **Open Anyway**.
- Or remove the quarantine attribute: `xattr -dr com.apple.quarantine /Applications/SumatraPDF.app`

## Opening documents

Use **File > Open**, drag files onto the Dock icon, use **Open With** in Finder, or the command line:

```sh
open -a /Applications/SumatraPDF.app /path/to/document.pdf
```

Settings are stored in `~/Library/Application Support/SumatraPDF/SumatraPDF-settings.txt`.

## License and source code

SumatraPDF is free software licensed under the GNU GPL v3 (`COPYING`). It includes MuPDF and other components under
the GNU AGPL v3 and other licenses. All license texts are in `SumatraPDF.app/Contents/Resources/Licenses/`;
`INDEX.txt` there lists them. `SOURCE.txt` says where to get the source code of this exact build.
