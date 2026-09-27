/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "gui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "Theme.h"
#include "Translations.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "PdfDarkMode.h"

// The Windows host of PdfDarkMode.h: page colors come from the theme and the
// DocumentColorsFollowTheme setting (DocColors.cpp is the host on other platforms).

// dark page rendering is active when the effective page background is dark
// (DocumentColorsFollowTheme or custom dark FixedPageUI colors); master's
// themes never touch page colors, unlike the fork's
bool PdfDarkModePagesDark() {
    Color bg;
    ThemePageRenderColors(bg);
    return !IsLightColor(bg);
}

static const char* DocumentColorsFollowThemeToString(DocumentColorsFollowTheme mode) {
    if (mode == DocumentColorsFollowTheme::Smart) {
        return "smart";
    }
    if (mode == DocumentColorsFollowTheme::Legacy) {
        return "legacy";
    }
    return "off";
}

static TempStr ColorToCssHexTemp(Color c) {
    u8 r, g, b;
    UnpackColor(c, r, g, b);
    return fmt("#%02x%02x%02x", r, g, b);
}

// User CSS overlay for MuPDF reflowable documents (EPUB, HTML, FB2, MOBI, TXT).
// Empty when the effective page colors are black-on-white (nothing to override).
TempStr ReflowDocumentThemeCssTemp() {
    Color bgCol;
    Color txtCol = ThemePageRenderColors(bgCol);
    if (bgCol == kColWhite && txtCol == kColBlack) {
        return {};
    }
    TempStr bg = ColorToCssHexTemp(bgCol);
    TempStr fg = ColorToCssHexTemp(txtCol);
    TempStr link = ColorToCssHexTemp(ThemeWindowLinkColor());
    // * first so html/body's background wins if MuPDF treats later rules as
    // stronger (a trailing * { background: transparent } would leave the
    // pixmap's white clear color showing through). Images are unaffected.
    return fmt(
        "* { color: %s !important; background-color: transparent !important; }\n"
        "html, body { background-color: %s !important; color: %s !important; }\n"
        "a, a * { color: %s !important; }\n",
        fg, bg, fg, link);
}

// an unsaved value the advanced settings dialog is previewing; -1 when there is
// none and the saved setting applies
static int gDocumentColorsFollowThemePreview = -1;

DocumentColorsFollowTheme GetDocumentColorsFollowTheme() {
    if (gDocumentColorsFollowThemePreview >= 0) {
        return (DocumentColorsFollowTheme)gDocumentColorsFollowThemePreview;
    }
    if (!gSettings || len(gSettings->documentColorsFollowTheme) == 0) {
        return DocumentColorsFollowTheme::Off;
    }
    return DocumentColorsFollowThemeFromString(gSettings->documentColorsFollowTheme);
}

// Render pages as if the setting had this value, without touching gSettings,
// so the advanced settings dialog can show what a value does before it's saved
// (and go back to the saved one when it's cancelled). The caller re-renders.
void SetDocumentColorsFollowThemePreview(DocumentColorsFollowTheme mode) {
    if (mode < DocumentColorsFollowTheme::Off || mode > DocumentColorsFollowTheme::Legacy) {
        mode = DocumentColorsFollowTheme::Off;
    }
    gDocumentColorsFollowThemePreview = (int)mode;
}

void ClearDocumentColorsFollowThemePreview() {
    gDocumentColorsFollowThemePreview = -1;
}

void SetDocumentColorsFollowTheme(DocumentColorsFollowTheme mode) {
    if (mode < DocumentColorsFollowTheme::Off || mode > DocumentColorsFollowTheme::Legacy) {
        mode = DocumentColorsFollowTheme::Off;
    }
    if (!gSettings) {
        return;
    }
    Str name(DocumentColorsFollowThemeToString(mode));
    if (!str::EqI(gSettings->documentColorsFollowTheme, name)) {
        str::ReplaceWithCopy(&gSettings->documentColorsFollowTheme, name);
    }
}

const char* DocumentColorsFollowThemeDescription(DocumentColorsFollowTheme mode) {
    if (mode == DocumentColorsFollowTheme::Smart) {
        return TrN("Document colors follow theme: Smart (recolor text and background, not images)").s;
    }
    if (mode == DocumentColorsFollowTheme::Legacy) {
        return TrN("Document colors follow theme: Legacy (recolor text, background and images)").s;
    }
    return TrN("Document colors follow theme: Off").s;
}

bool PdfDarkModeUsesObjectLevel() {
    if (!PdfDarkModePagesDark()) {
        return false;
    }
    if (GetDocumentColorsFollowTheme() != DocumentColorsFollowTheme::Smart) {
        return false;
    }
    return GetPdfDarkModeRenderer() == PdfDarkModeRenderer::ObjectLevelDevice;
}

void BuildViewDarkModeProfile(EngineBase* engine, DarkModeProfile* profile) {
    ReportIf(!profile);
    if (!profile) {
        return;
    }
    *profile = DarkModeProfile{};

    // unlike the fork's themes, master's themes never touch page colors:
    // dark pages come from DocumentColorsFollowTheme or custom dark
    // FixedPageUI colors, so key the dark modes off the effective page
    // background rather than the window chrome
    Color bgCol;
    Color textCol = ThemePageRenderColors(bgCol);
    bool pagesDark = !IsLightColor(bgCol);
    profile->foreground = textCol;
    profile->pageBackground = bgCol;
    profile->linkColor = pagesDark ? ThemeWindowLinkColor() : 0;
    profile->strength = 1.f;
    profile->preservePdfImages = GetPreservePdfImagesInDarkMode();
    profile->preservePdfImagesMinSize = GetPreservePdfImagesMinSize();
    profile->options = PdfDarkModeCurrentOptions();
    profile->palette = PdfDarkModePaletteFromColors(textCol, bgCol, profile->linkColor);

    if (!pagesDark) {
        // mode stays Normal: the render cache's default recolor pass still
        // applies custom (light) page colors from the cache colors
        profile->hash = PdfDarkModeComputeProfileHash(profile);
        return;
    }

    if (EngineUsesReflowThemeCss(engine)) {
        // EPUB/HTML/FB2/MOBI/TXT go through MuPDF's HTML engine: page colors
        // are applied as user CSS (images stay as in the file). Bitmap recolor
        // inverted some of those images (#6050).
        profile->mode = PageColorMode::Normal;
    } else if (EngineUsesDocumentColorsFollowTheme(engine)) {
        if (GetDocumentColorsFollowTheme() == DocumentColorsFollowTheme::Legacy) {
            profile->mode = PageColorMode::LegacyInvert;
        } else {
            // Smart (or Off with pagesDark already handled above): prefer object-level
            if (EngineSupportsSmartDarkMode(engine) && PdfDarkModeUsesObjectLevel()) {
                profile->mode = PageColorMode::SmartDark;
            } else if (profile->preservePdfImages) {
                profile->mode = PageColorMode::PreserveImages;
            } else {
                profile->mode = PageColorMode::LegacyInvert;
            }
        }
    }

    profile->hash = PdfDarkModeComputeProfileHash(profile);
}

bool EngineUsesDocumentColorsFollowTheme(EngineBase* engine) {
    if (!engine || engine->IsImageCollection()) {
        return false;
    }
    if (engine->kind == kindEngineMupdf || engine->kind == kindEngineDjVu) {
        return true;
    }
    // Native HTML-layout engines paint black-on-white pages. Recolor them with
    // FixedPageUI colors the same way as PDF (issue #6030: CHM went white when
    // recolor was narrowed to MuPDF+DjVu).
    return engine->kind == kindEngineChm || engine->kind == kindEngineEpub || engine->kind == kindEngineFb2 ||
           engine->kind == kindEngineMobi || engine->kind == kindEnginePdb || engine->kind == kindEngineHtml ||
           engine->kind == kindEngineTxt;
}

bool EngineUsesReflowThemeCss(EngineBase* engine) {
    return engine && engine->kind == kindEngineMupdf && engine->isReflowable;
}
