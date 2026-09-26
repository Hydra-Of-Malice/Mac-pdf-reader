/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// POSIX (macOS, Linux) image decoding: MuPDF's loaders plus our own decoders for
// the formats it doesn't handle. Windows (GDI+ / WIC) is in ImageReader.cpp.

#include "base/Base.h"
#include "base/GuessFileType.h"
#include "base/Pixmap.h"
#include "base/TgaReader.h"

extern "C" {
#include <mupdf/fitz.h>
}

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

// PNG, GIF, BMP, TIFF, PNM, PSD, JBIG2: MuPDF's loaders (first image only)
static Pixmap* PixmapFromMupdfLoaders(Str d) {
    fz_context* ctx = fz_new_context_windows();
    if (!ctx) {
        return nullptr;
    }
    Pixmap* px = PixmapFromImageData(ctx, (const u8*)d.s, (size_t)len(d));
    fz_drop_context_windows(ctx);
    return px;
}

// Decode image bytes to a single (first-frame) Pixmap. Caller owns it (FreePixmap).
Pixmap* PixmapFromData(Str bmpData) {
    if (ImageDecodedPixmapWouldBeHuge(bmpData)) {
        return nullptr;
    }
    FileType kind = GuessFileTypeFromData(bmpData);
    Pixmap* px = PixmapFromDataFz(bmpData);
    if (px) {
        // mupdf doesn't apply a WebP's EXIF orientation
        if (kind == FileType::Webp) {
            px = PixmapApplyExifOrientation(px, WebpExifOrientation(bmpData));
        }
        return px;
    }
    px = PixmapFromOwnDecoders(bmpData, kind);
    if (px) {
        return px;
    }
    return PixmapFromMupdfLoaders(bmpData);
}

// Every page of a multi-page TIFF; empty if it has fewer than 2.
static Vec<Pixmap*> TiffPages(Str d) {
    Vec<Pixmap*> res;
    fz_context* ctx = fz_new_context_windows();
    if (!ctx) {
        return res;
    }
    const u8* data = (const u8*)d.s;
    size_t n = (size_t)len(d);
    int count = 0;
    fz_try(ctx) {
        count = fz_load_tiff_subimage_count(ctx, data, n);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        count = 0;
    }
    for (int i = 0; count > 1 && i < count; i++) {
        fz_pixmap* pix = nullptr;
        fz_try(ctx) {
            pix = fz_load_tiff_subimage(ctx, data, n, i);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            pix = nullptr;
        }
        Pixmap* px = pix ? PixmapFromFzPixmap(ctx, pix) : nullptr;
        if (!px) {
            break;
        }
        VecAppend(res, px);
    }
    fz_drop_context_windows(ctx);
    return res;
}

// One Pixmap per frame: every page of a multi-page TIFF; the first (MuPDF:
// composited) frame of other multi-frame formats, e.g. animated GIF.
Vec<Pixmap*> PixmapsFromData(Str bmpData) {
    Vec<Pixmap*> res;
    if (ImageDecodedPixmapWouldBeHuge(bmpData)) {
        return res;
    }
    if (GuessFileTypeFromData(bmpData) == FileType::Tiff) {
        res = TiffPages(bmpData);
        if (len(res) > 0) {
            return res;
        }
    }
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
