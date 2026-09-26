/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// POSIX (macOS, Linux) image decoding: MuPDF first, then our own decoders for
// the formats it doesn't handle. Windows (GDI+ / WIC) is in ImageReader.cpp.

#include "base/Base.h"
#include "base/GuessFileType.h"
#include "base/Pixmap.h"
#include "base/TgaReader.h"

#include "AvifReader.h"
#include "JxlReader.h"
#include "WebpReader.h"

#include "ImageReader.h"

static Pixmap* PixmapFromOwnDecoders(Str d, FileType kind) {
    switch (kind) {
        case FileType::Tga:
            return tga::PixmapFromData(d);
        case FileType::Webp:
            return webp::PixmapFromData(d);
        case FileType::Jxl:
            return jxl::PixmapFromData(d);
        case FileType::Heic:
        case FileType::Avif:
            return PixmapFromAvifData(d);
        default:
            return nullptr;
    }
}

// Decode image bytes to a single (first-frame) Pixmap. Caller owns it (FreePixmap).
Pixmap* PixmapFromData(Str bmpData) {
    if (ImageDecodedPixmapWouldBeHuge(bmpData)) {
        return nullptr;
    }
    FileType kind = GuessFileTypeFromData(bmpData);
    Pixmap* px = PixmapFromDataFz(bmpData);
    if (!px) {
        return PixmapFromOwnDecoders(bmpData, kind);
    }
    // mupdf doesn't apply a WebP's EXIF orientation
    if (kind == FileType::Webp) {
        px = PixmapApplyExifOrientation(px, WebpExifOrientation(bmpData));
    }
    return px;
}

// One Pixmap per frame; only the first frame of multi-frame TIFF / GIF for now.
Vec<Pixmap*> PixmapsFromData(Str bmpData) {
    Vec<Pixmap*> res;
    Pixmap* px = PixmapFromData(bmpData);
    if (px) {
        VecAppend(res, px);
    }
    return res;
}

// RenderedBitmap wraps a GDI HBITMAP, which POSIX doesn't have
RenderedBitmap* LoadRenderedBitmap(Str) {
    return nullptr;
}
