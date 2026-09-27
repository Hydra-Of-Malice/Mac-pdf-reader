/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Document colors (dark mode) of the POSIX reader (the macOS app, through
// src/mac/SumatraMacEngine.h): the current scheme, its epoch, and rendering a
// page with it. DocColors.cpp is also the PdfDarkMode.h host there, the part
// Windows gets from its theme and settings (PdfDarkModeProfile.cpp).
//
// What each mode does, per engine (like Windows, see BuildViewDarkModeProfile):
//   SmartDark  MuPDF documents (PDF, XPS, EPUB, FB2, MOBI, CHM, HTML, TXT, ...):
//              the object-level dark device (PdfDarkModeDevice.cpp): text, paths
//              and backgrounds take the scheme's colors, images are classified
//              (photos kept, light artwork blended, full-page scans darkened).
//              DjVu: the bitmap recolor pass (a scan's paper takes the
//              background color, its ink the text color); a color page (photo,
//              illustration) keeps its colors.
//              With preserveImages off, both get the full bitmap recolor
//              (images included), like Windows' CmdTogglePreservePdfImages.
//   Inverted   the bitmap recolor pass with white text on black, images included
//              (Windows' Legacy mode).
//   Normal     nothing.
// Images and comic books (image collections) always keep their colors. Print
// renders (RenderTarget::Print) are always Normal.

enum class DocColorsMode {
    Normal,
    SmartDark,
    Inverted,
};

struct DocColorScheme {
    DocColorsMode mode = DocColorsMode::Normal;
    Color text = MkRgb(0xe6, 0xe6, 0xe6);       // SmartDark
    Color background = MkRgb(0x1e, 0x1e, 0x1e); // SmartDark
    bool preserveImages = true;                 // SmartDark
};

// Any thread. Starts a new epoch if the scheme changed; the caller drops renders
// made with the old one.
void SetDocColorScheme(const DocColorScheme& scheme);
DocColorScheme GetDocColorScheme();
// changes with every scheme change
u32 GetDocColorsEpoch();

// engine->RenderPage(args) with the current scheme applied to View renders.
// *epochOut (if non-null): the epoch of the scheme it was rendered with.
Pixmap* RenderPageWithDocColors(EngineBase* engine, RenderPageArgs& args, u32* epochOut = nullptr);
