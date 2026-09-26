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

//--- animated GIF: one page per frame, like Windows (MuPDF alone draws all frames onto one image)

constexpr int kMaxGifFrames = 1000;
constexpr i64 kMaxGifPagesBytes = 512LL * 1024 * 1024;

// the blocks that draw one frame: its graphic control extension (may be empty)
// and the image descriptor through the end of the image data
struct GifFrame {
    Str gce;
    Str image;
    int x = 0;
    int y = 0;
    int disposal = 0; // 2: restore to background, 3: restore to previous
};

static int GifColorTableSize(u8 flags) {
    return (flags & 0x80) ? 3 * (1 << ((flags & 7) + 1)) : 0;
}

// index after the data sub-blocks starting at i, -1 if truncated
static int GifSkipSubBlocks(Str d, int i) {
    while (i < len(d)) {
        int n = (u8)d.s[i];
        i += 1 + n;
        if (n == 0) {
            return i <= len(d) ? i : -1;
        }
    }
    return -1;
}

// Splits a GIF into its frames (a truncated last frame is dropped).
static void GifParseFrames(Str d, Vec<GifFrame>& frames) {
    const u8* p = (const u8*)d.s;
    int i = 13 + GifColorTableSize(p[10]);
    Str gce;
    while (i + 1 < len(d) && len(frames) < kMaxGifFrames) {
        if (p[i] == 0x21) {
            int next = GifSkipSubBlocks(d, i + 2);
            if (next < 0) {
                return;
            }
            if (p[i + 1] == 0xf9 && next - i >= 8) {
                gce = Str(d.s + i, next - i);
            }
            i = next;
            continue;
        }
        if (p[i] != 0x2c || i + 11 > len(d)) {
            return;
        }
        // +1: the LZW minimum code size before the sub-blocks
        int next = GifSkipSubBlocks(d, i + 10 + GifColorTableSize(p[i + 9]) + 1);
        if (next < 0) {
            return;
        }
        GifFrame f;
        f.gce = gce;
        f.image = Str(d.s + i, next - i);
        f.x = p[i + 1] | (p[i + 2] << 8);
        f.y = p[i + 3] | (p[i + 4] << 8);
        f.disposal = len(gce) > 0 ? (((u8)gce.s[3] >> 2) & 7) : 0;
        VecAppend(frames, f);
        gce = {};
        i = next;
    }
}

// Decodes a frame on its own: a GIF of the frame's size, at 0,0, with the global colors.
static Pixmap* GifDecodeFrame(fz_context* ctx, Str d, const GifFrame& f) {
    const u8* img = (const u8*)f.image.s;
    int ctSize = GifColorTableSize((u8)d.s[10]);
    str::Builder gif;
    gif.Append(StrL("GIF89a"));
    u8 lsd[7] = {img[5], img[6], img[7], img[8], (u8)d.s[10], (u8)d.s[11], 0};
    gif.Append(Str((char*)lsd, sizeof(lsd)));
    gif.Append(Str(d.s + 13, ctSize));
    gif.Append(f.gce);
    int imgStart = len(gif);
    gif.Append(f.image);
    gif.AppendChar(0x3b);
    Str s = ToStr(gif);
    memset(s.s + imgStart + 1, 0, 4);
    return PixmapFromImageData(ctx, (const u8*)s.s, (size_t)len(s));
}

// Frames composited as the animation shows them. Pixel states as in MuPDF's GIF
// decoder: 0 transparent, 1 background, 2 drawn by a frame.
static Vec<Pixmap*> GifPages(Str d) {
    Vec<Pixmap*> res;
    if (len(d) < 13) {
        return res;
    }
    Vec<GifFrame> frames;
    GifParseFrames(d, frames);
    const u8* p = (const u8*)d.s;
    int w = p[6] | (p[7] << 8);
    int h = p[8] | (p[9] << 8);
    if (len(frames) < 2 || w <= 0 || h <= 0) {
        return res;
    }
    i64 pageBytes = (i64)w * h * 4;
    int nFrames = (int)std::min((i64)len(frames), kMaxGifPagesBytes / pageBytes);
    if (nFrames < 2) {
        return res;
    }

    bool hasGct = (p[10] & 0x80) != 0;
    u8 bg[4] = {0, 0, 0, 0};
    if (hasGct) {
        const u8* c = p + 13 + 3 * std::min((int)p[11], GifColorTableSize(p[10]) / 3 - 1);
        u8 bgra[4] = {c[2], c[1], c[0], 0xff};
        memcpy(bg, bgra, 4);
    }
    u8 bgState = hasGct ? 1 : 0;
    int n = w * h;
    u8* canvas = AllocArray<u8>(n * 4);
    u8* state = AllocArray<u8>(n);
    u8* savedCanvas = AllocArray<u8>(n * 4);
    u8* savedState = AllocArray<u8>(n);
    fz_context* ctx = (canvas && state && savedCanvas && savedState) ? fz_new_context_windows() : nullptr;
    if (!ctx) {
        nFrames = 0;
    }
    for (int i = 0; i < n && ctx; i++) {
        memcpy(canvas + i * 4, bg, 4);
        state[i] = bgState;
    }

    for (int fi = 0; fi < nFrames; fi++) {
        const GifFrame& f = frames[fi];
        Pixmap* fp = GifDecodeFrame(ctx, d, f);
        if (!fp) {
            break;
        }
        if (f.disposal == 3) {
            memcpy(savedCanvas, canvas, (size_t)n * 4);
            memcpy(savedState, state, (size_t)n);
        }
        int fx1 = std::min(w, f.x + fp->width);
        int fy1 = std::min(h, f.y + fp->height);
        for (int y = f.y; y < fy1; y++) {
            const u8* src = fp->data + (size_t)(y - f.y) * fp->stride;
            for (int x = f.x; x < fx1; x++) {
                const u8* s = src + (x - f.x) * 4;
                int idx = y * w + x;
                if (s[3] != 0) {
                    memcpy(canvas + idx * 4, s, 3);
                    canvas[idx * 4 + 3] = 0xff;
                    state[idx] = 2;
                } else if (state[idx] == 1) {
                    state[idx] = 0;
                }
            }
        }
        FreePixmap(fp);

        Pixmap* page = AllocPixmap(w, h, PixmapFormat::BGRA8, true);
        if (!page) {
            break;
        }
        for (int y = 0; y < h; y++) {
            u8* dst = page->data + (size_t)y * page->stride;
            for (int x = 0; x < w; x++) {
                int idx = y * w + x;
                bool transparent = state[idx] == 0;
                page->hasAlpha |= transparent;
                u8 clear[4] = {0, 0, 0, 0};
                memcpy(dst + x * 4, transparent ? clear : canvas + idx * 4, 4);
            }
        }
        VecAppend(res, page);

        if (f.disposal == 2) {
            for (int y = f.y; y < fy1; y++) {
                for (int x = f.x; x < fx1; x++) {
                    memcpy(canvas + (y * w + x) * 4, bg, 4);
                    state[y * w + x] = bgState;
                }
            }
        } else if (f.disposal == 3) {
            memcpy(canvas, savedCanvas, (size_t)n * 4);
            memcpy(state, savedState, (size_t)n);
        }
    }
    if (ctx) {
        fz_drop_context_windows(ctx);
    }
    free(canvas);
    free(state);
    free(savedCanvas);
    free(savedState);
    if (len(res) < 2) {
        for (Pixmap* px : res) {
            FreePixmap(px);
        }
        VecReset(res);
    }
    return res;
}

// One Pixmap per frame: every page of a multi-page TIFF, every frame of an
// animated GIF; the first image of other formats.
Vec<Pixmap*> PixmapsFromData(Str bmpData) {
    Vec<Pixmap*> res;
    if (ImageDecodedPixmapWouldBeHuge(bmpData)) {
        return res;
    }
    FileType kind = GuessFileTypeFromData(bmpData);
    if (kind == FileType::Tiff) {
        res = TiffPages(bmpData);
    } else if (kind == FileType::Gif) {
        res = GifPages(bmpData);
    }
    if (len(res) > 0) {
        return res;
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
