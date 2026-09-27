/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Pixmap.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "gui/UIModels.h"
#include "EngineBase.h"
#include "PdfDarkMode.h"
#include "DocColors.h"

// See DocColors.h. The scheme is set on the main thread and read by render
// threads (every render takes a snapshot), so it's behind a mutex.

// SmartDark: links (blue-ish text) are mapped to this, like the theme's link
// color on Windows
static constexpr Color kSmartDarkLinkColor = MkRgb(0x8a, 0xb4, 0xf8);

static Mutex gDocColorsMutex;
static DocColorScheme gDocColors;
static u32 gDocColorsEpoch = 1;

static bool SameScheme(const DocColorScheme& a, const DocColorScheme& b) {
    return a.mode == b.mode && a.text == b.text && a.background == b.background && a.preserveImages == b.preserveImages;
}

void SetDocColorScheme(const DocColorScheme& scheme) {
    AutoUnlockMutex lock(&gDocColorsMutex);
    if (SameScheme(scheme, gDocColors)) {
        return;
    }
    gDocColors = scheme;
    gDocColorsEpoch++;
    // the global the Windows toggle sets; part of the analyses' cache keys
    SetPreservePdfImagesInDarkMode(scheme.preserveImages);
}

DocColorScheme GetDocColorScheme() {
    AutoUnlockMutex lock(&gDocColorsMutex);
    return gDocColors;
}

u32 GetDocColorsEpoch() {
    AutoUnlockMutex lock(&gDocColorsMutex);
    return gDocColorsEpoch;
}

// page text, background and link colors a scheme renders with
static void SchemePageColors(const DocColorScheme& s, Color* text, Color* bg, Color* link) {
    switch (s.mode) {
        case DocColorsMode::SmartDark:
            *text = s.text;
            *bg = s.background;
            *link = kSmartDarkLinkColor;
            return;
        case DocColorsMode::Inverted:
            *text = kColWhite;
            *bg = kColBlack;
            *link = 0;
            return;
        default:
            *text = kColBlack;
            *bg = kColWhite;
            *link = 0;
            return;
    }
}

static bool SchemePagesDark(const DocColorScheme& s) {
    Color text, bg, link;
    SchemePageColors(s, &text, &bg, &link);
    return !IsLightColor(bg);
}

//--- PdfDarkMode.h host functions (PdfDarkModeProfile.cpp on Windows)

bool PdfDarkModePagesDark() {
    return SchemePagesDark(GetDocColorScheme());
}

DocumentColorsFollowTheme GetDocumentColorsFollowTheme() {
    DocColorScheme s = GetDocColorScheme();
    if (s.mode == DocColorsMode::SmartDark) {
        return DocumentColorsFollowTheme::Smart;
    }
    if (s.mode == DocColorsMode::Inverted) {
        return DocumentColorsFollowTheme::Legacy;
    }
    return DocumentColorsFollowTheme::Off;
}

// always the object-level device for SmartDark (Windows keeps it behind a
// compile-time switch, GetPdfDarkModeRenderer())
bool PdfDarkModeUsesObjectLevel() {
    DocColorScheme s = GetDocColorScheme();
    return s.mode == DocColorsMode::SmartDark && SchemePagesDark(s);
}

// Windows restyles reflowable documents with this CSS; here they get the
// object-level device like every MuPDF document (no relayout, images classified)
TempStr ReflowDocumentThemeCssTemp() {
    return {};
}

bool EngineUsesReflowThemeCss(EngineBase*) {
    return false;
}

// engines whose pages are recolored (image collections keep their colors)
bool EngineUsesDocumentColorsFollowTheme(EngineBase* engine) {
    if (!engine || engine->IsImageCollection()) {
        return false;
    }
    return engine->kind == kindEngineMupdf || engine->kind == kindEngineDjVu;
}

static void BuildProfile(EngineBase* engine, const DocColorScheme& s, DarkModeProfile* profile) {
    *profile = DarkModeProfile{};
    Color text, bg, link;
    SchemePageColors(s, &text, &bg, &link);
    profile->foreground = text;
    profile->pageBackground = bg;
    profile->linkColor = link;
    profile->strength = 1.f;
    profile->preservePdfImages = s.preserveImages;
    profile->preservePdfImagesMinSize = GetPreservePdfImagesMinSize();
    profile->options = PdfDarkModeCurrentOptions();
    profile->palette = PdfDarkModePaletteFromColors(text, bg, link);

    if (SchemePagesDark(s) && EngineUsesDocumentColorsFollowTheme(engine)) {
        if (s.mode == DocColorsMode::Inverted || !s.preserveImages) {
            profile->mode = PageColorMode::LegacyInvert;
        } else if (engine->kind == kindEngineMupdf) {
            profile->mode = PageColorMode::SmartDark;
        } else {
            // DjVu: no image regions to keep, so the whole page is recolored
            profile->mode = PageColorMode::PreserveImages;
        }
    }
    profile->hash = PdfDarkModeComputeProfileHash(profile);
}

void BuildViewDarkModeProfile(EngineBase* engine, DarkModeProfile* profile) {
    ReportIf(!profile);
    if (profile) {
        BuildProfile(engine, GetDocColorScheme(), profile);
    }
}

//--- rendering

// A DjVu page is recolored as a whole (the engine has no image regions to keep).
// That suits scans (ink and paper), not photos and color illustrations: those
// keep their colors when images are preserved. Colorful = a good share of
// clearly chromatic pixels (a scan's colored highlights or stamps are few).
static bool PixmapLooksLikeColorImage(const Pixmap* bmp) {
    int bpp = PixmapBytesPerPixel(bmp->format);
    if (!bmp->data || (bmp->format != PixmapFormat::BGRA8 && bmp->format != PixmapFormat::BGR8)) {
        return false;
    }
    constexpr int kStep = 3;
    constexpr int kMinChroma = 40;
    i64 n = 0, colorful = 0;
    for (int y = 0; y < bmp->height; y += kStep) {
        const u8* row = bmp->data + (size_t)y * (size_t)bmp->stride;
        for (int x = 0; x < bmp->width; x += kStep) {
            const u8* p = row + (size_t)x * bpp;
            int maxC = std::max({p[0], p[1], p[2]});
            int minC = std::min({p[0], p[1], p[2]});
            colorful += maxC - minC >= kMinChroma ? 1 : 0;
            n++;
        }
    }
    return n > 0 && colorful * 10 >= n * 3;
}

Pixmap* RenderPageWithDocColors(EngineBase* engine, RenderPageArgs& args, u32* epochOut) {
    DocColorScheme scheme;
    u32 epoch = 0;
    {
        AutoUnlockMutex lock(&gDocColorsMutex);
        scheme = gDocColors;
        epoch = gDocColorsEpoch;
    }
    if (epochOut) {
        *epochOut = epoch;
    }
    DarkModeProfile profile;
    bool view = args.target == RenderTarget::View;
    if (view) {
        BuildProfile(engine, scheme, &profile);
    }
    const DarkModeProfile* prevProfile = args.darkProfile;
    if (view && profile.mode != PageColorMode::Normal) {
        args.darkProfile = &profile;
    }
    Pixmap* bmp = engine->RenderPage(args);
    args.darkProfile = prevProfile;

    // the bitmap pass, as the Windows RenderCache does after rendering
    if (!bmp || !view || !DarkModeProfileUsesLegacyPostProcess(&profile) || bmp->hasAlpha) {
        return bmp;
    }
    Vec<Rect> skipRects;
    Vec<Rect>* skip = nullptr;
    Color linkColor = profile.linkColor;
    if (engine->kind == kindEngineDjVu) {
        if (profile.mode == PageColorMode::PreserveImages && PixmapLooksLikeColorImage(bmp)) {
            return bmp;
        }
        // blue-ish areas of a scan aren't links
        linkColor = 0;
    } else if (profile.mode == PageColorMode::PreserveImages && profile.preservePdfImages) {
        RectF pageRect = args.pageRect ? *args.pageRect : engine->PageMediabox(args.pageNo);
        engine->GetBitmapRecolorSkipRects(args.pageNo, args.zoom, args.rotation, pageRect,
                                          Size(bmp->width, bmp->height), skipRects);
        if (len(skipRects) > 0) {
            skip = &skipRects;
        }
    }
    RecolorPixmap(bmp, profile.foreground, profile.pageBackground, linkColor, skip);
    return bmp;
}
