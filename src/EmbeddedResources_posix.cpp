/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// POSIX (macOS, Linux) has no IDR_EMBEDDED_PAK: the files it would hold (e.g.
// mupdf's fonts as fonts/<name>) are read from a resources directory instead:
//   <exe dir>/../Resources   macOS app bundle (Contents/MacOS/SumatraPDF)
//   <exe dir>                command-line tools (test_engines, test_util)

#include "base/Base.h"
#include "base/File.h"

#include "EmbeddedResources.h"

bool EnsureEmbeddedArchiveLoaded() {
    return false;
}

lzma::SimpleArchive* GetEmbeddedArchive() {
    return nullptr;
}

u8* GetEmbeddedFileData(Str name, int* outSize) {
    if (outSize) {
        *outSize = 0;
    }
    if (len(name) == 0) {
        return nullptr;
    }
    // archive names use '\' (e.g. "fonts\NimbusSans-Regular.cff")
    TempStr relPath = str::ReplaceTemp(name, StrL("\\"), StrL("/"));
    TempStr exeDir = GetSelfExeDirTemp();
    TempStr dirs[] = {path::JoinTemp(exeDir, StrL("../Resources")), exeDir};
    for (TempStr dir : dirs) {
        Str d = file::ReadFile(path::JoinTemp(dir, relPath));
        if (!d.s) {
            continue;
        }
        if (outSize) {
            *outSize = d.len;
        }
        return (u8*)d.s;
    }
    return nullptr;
}
