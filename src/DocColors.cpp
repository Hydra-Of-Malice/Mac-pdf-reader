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
    return a.mode == b.mode && a.text == b.text && a.background == b.background &&
           a.preserveImages == b.preserveImages;
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
    if (profile.mode == PageColorMode::PreserveImages && profile.preservePdfImages) {
        RectF pageRect = args.pageRect ? *args.pageRect : engine->PageMediabox(args.pageNo);
        engine->GetBitmapRecolorSkipRects(args.pageNo, args.zoom, args.rotation, pageRect,
                                          Size(bmp->width, bmp->height), skipRects);
        if (len(skipRects) > 0) {
            skip = &skipRects;
        }
    }
    RecolorPixmap(bmp, profile.foreground, profile.pageBackground, profile.linkColor, skip);
    return bmp;
}
