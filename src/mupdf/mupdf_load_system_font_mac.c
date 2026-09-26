// Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
// License: Simplified BSD (see COPYING.BSD)
//
// macOS counterpart of mupdf_load_system_font.c: mupdf's system-font hooks
// backed by CoreText, so a PDF that names a font without embedding it (and an
// ebook stylesheet naming a family) gets the installed font rather than a
// built-in substitute. Compiled into mupdf for the macOS build only: it needs
// the CoreText framework.

#include "mupdf/fitz.h"
#include "mupdf/ucdn.h"
#include "mupdf/pdf.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kMaxFontName = 256,
    kMaxPath = 4096,
};

// A font file's bytes, read once and kept for the life of the process: fonts
// share them through fz_new_buffer_from_shared_data()
typedef struct font_file {
    struct font_file* next;
    unsigned char* data;
    size_t size;
    char path[1];
} font_file;

static pthread_mutex_t g_files_mutex = PTHREAD_MUTEX_INITIALIZER;
static font_file* g_files = NULL;

static font_file* read_font_file(const char* path) {
    pthread_mutex_lock(&g_files_mutex);
    font_file* ff = g_files;
    while (ff && strcmp(ff->path, path) != 0) {
        ff = ff->next;
    }
    if (ff) {
        pthread_mutex_unlock(&g_files_mutex);
        return ff;
    }
    FILE* f = fopen(path, "rb");
    long size = -1;
    if (f && fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    unsigned char* data = size > 0 ? (unsigned char*)malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    if (f) {
        fclose(f);
    }
    if (data) {
        ff = (font_file*)malloc(sizeof(font_file) + strlen(path));
    }
    if (ff) {
        strcpy(ff->path, path);
        ff->data = data;
        ff->size = (size_t)size;
        ff->next = g_files;
        g_files = ff;
    } else {
        free(data);
    }
    pthread_mutex_unlock(&g_files_mutex);
    return ff;
}

static int cfstr_to_utf8(CFStringRef s, char* buf, int cb) {
    buf[0] = 0;
    return s && CFStringGetCString(s, buf, cb, kCFStringEncodingUTF8);
}

// lower case, without spaces, '-' and '_': "Times New Roman" == "TimesNewRoman"
static void normalize_name(const char* s, char* out, int cb) {
    int n = 0;
    for (; *s && n < cb - 1; s++) {
        if (*s == ' ' || *s == '-' || *s == '_') {
            continue;
        }
        out[n++] = (char)tolower((unsigned char)*s);
    }
    out[n] = 0;
}

static int names_equal(const char* a, const char* b) {
    char na[kMaxFontName];
    char nb[kMaxFontName];
    normalize_name(a, na, sizeof(na));
    normalize_name(b, nb, sizeof(nb));
    return na[0] && strcmp(na, nb) == 0;
}

// CTFontCreateWithName() falls back to some other font for an unknown name, so
// only accept a font whose PostScript, full or family name is the one asked for
static int font_has_name(CTFontRef font, const char* name) {
    CFStringRef names[3] = {CTFontCopyPostScriptName(font), CTFontCopyFullName(font), CTFontCopyFamilyName(font)};
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        char buf[kMaxFontName];
        if (!ok && cfstr_to_utf8(names[i], buf, sizeof(buf))) {
            ok = names_equal(buf, name);
        }
        if (names[i]) {
            CFRelease(names[i]);
        }
    }
    return ok;
}

// The installed font called name (PostScript, full or family name), or NULL.
// traits asks for a bold / italic face of it; NULL if there's none.
static CTFontRef find_ct_font(const char* name, CTFontSymbolicTraits traits) {
    CFStringRef cfName = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    if (!cfName) {
        return NULL;
    }
    CTFontRef font = CTFontCreateWithName(cfName, 12.0, NULL);
    CFRelease(cfName);
    if (!font) {
        return NULL;
    }
    if (!font_has_name(font, name)) {
        CFRelease(font);
        return NULL;
    }
    if (traits == 0) {
        return font;
    }
    CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(font, 0.0, NULL, traits, traits);
    CFRelease(font);
    return styled;
}

// FT_Get_Postscript_Name() can allocate, and mupdf's FreeType allocator takes
// its fz_context from the FreeType lock: calling it unlocked crashed (fz_lock on
// a NULL context) the first time a .ttc was searched, e.g. Helvetica.ttc as the
// fallback for U+FFFD. Never called with the lock held: fz_new_font_from_buffer()
// in load_ct_font() takes it too.
static int ps_name_is(fz_context* ctx, FT_Face face, const char* psName) {
    if (!face) {
        return 0;
    }
    fz_ft_lock(ctx);
    const char* name = FT_Get_Postscript_Name(face);
    int ok = name && strcmp(name, psName) == 0;
    fz_ft_unlock(ctx);
    return ok;
}

// Loads the file behind ct as an fz_font; a collection (.ttc) is searched for
// the face with ct's PostScript name
static fz_font* load_ct_font(fz_context* ctx, CTFontRef ct, const char* requested) {
    char path[kMaxPath];
    char psName[kMaxFontName];
    CFURLRef url = (CFURLRef)CTFontCopyAttribute(ct, kCTFontURLAttribute);
    int havePath = url && CFURLGetFileSystemRepresentation(url, 1, (UInt8*)path, sizeof(path));
    if (url) {
        CFRelease(url);
    }
    CFStringRef cfPsName = CTFontCopyPostScriptName(ct);
    cfstr_to_utf8(cfPsName, psName, sizeof(psName));
    if (cfPsName) {
        CFRelease(cfPsName);
    }
    font_file* ff = havePath ? read_font_file(path) : NULL;
    if (!ff) {
        return NULL;
    }

    fz_font* font = NULL;
    fz_buffer* buf = NULL;
    fz_var(font);
    fz_var(buf);
    fz_try(ctx) {
        buf = fz_new_buffer_from_shared_data(ctx, ff->data, ff->size);
        font = fz_new_font_from_buffer(ctx, requested, buf, 0, 1);
        FT_Face face = (FT_Face)fz_font_ft_face(ctx, font);
        int nFaces = face ? (int)face->num_faces : 1;
        for (int i = 1; i < nFaces && psName[0] && !ps_name_is(ctx, face, psName); i++) {
            fz_drop_font(ctx, font);
            font = NULL;
            font = fz_new_font_from_buffer(ctx, requested, buf, i, 1);
            face = (FT_Face)fz_font_ft_face(ctx, font);
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_drop_font(ctx, font);
        fz_report_error(ctx);
        font = NULL;
    }
    return font;
}

static fz_font* load_font_named(fz_context* ctx, const char* name, CTFontSymbolicTraits traits, const char* requested) {
    CTFontRef ct = find_ct_font(name, traits);
    if (!ct) {
        return NULL;
    }
    fz_font* font = load_ct_font(ctx, ct, requested);
    CFRelease(ct);
    return font;
}

static CTFontSymbolicTraits style_traits(const char* style, int bold, int italic) {
    CTFontSymbolicTraits traits = 0;
    if (bold || (style && strstr(style, "Bold"))) {
        traits |= kCTFontBoldTrait;
    }
    if (italic || (style && (strstr(style, "Italic") || strstr(style, "Oblique")))) {
        traits |= kCTFontItalicTrait;
    }
    return traits;
}

// "Arial,Bold", "ArialMT", "Arial-BoldMT", "ArialBold". bold / italic also ask
// for a styled face, e.g. a stylesheet's "Georgia" in bold (a faked bold can't
// be written into a PDF appearance stream, see #6198 in the Windows loader)
static fz_font* load_mac_font_by_name(fz_context* ctx, const char* orig_name, int bold, int italic) {
    char base[kMaxFontName];
    fz_strlcpy(base, orig_name, sizeof(base));
    const char* style = NULL;
    char* comma = strchr(base, ',');
    if (comma) {
        *comma = 0;
        style = comma + 1;
    } else {
        // prestyled: "ArialBold", "Arial-BoldItalic"
        static const char* styles[] = {"BoldItalic", "BoldOblique", "Bold", "Italic", "Oblique"};
        size_t n = strlen(base);
        for (size_t i = 0; i < nelem(styles) && !style; i++) {
            size_t k = strlen(styles[i]);
            if (n > k && strcmp(base + n - k, styles[i]) == 0) {
                style = styles[i];
                base[n - k] = 0;
                if (base[n - k - 1] == '-' || base[n - k - 1] == ' ') {
                    base[n - k - 1] = 0;
                }
            }
        }
    }

    fz_font* font = NULL;
    CTFontSymbolicTraits traits = style_traits(style, bold, italic);
    if (traits) {
        font = load_font_named(ctx, base, traits, orig_name);
    }
    if (!font) {
        // the name as given, e.g. a PostScript name like "Arial-BoldMT"
        font = load_font_named(ctx, orig_name, 0, orig_name);
    }
    if (!font && strcmp(base, orig_name) != 0) {
        // no such face: the regular one, which mupdf emboldens / slants
        font = load_font_named(ctx, base, 0, orig_name);
    }
    return font;
}

static fz_font* load_mac_font(fz_context* ctx, const char* fontname, int bold, int italic, int needs_exact_metrics) {
    const char* clean_name = pdf_clean_font_name(fontname);
    int is_base_14 = clean_name != fontname;
    // same as the Windows loader: a PDF naming Times / Helvetica / Courier gets
    // mupdf's metric-compatible built-in fonts, not the system ones
    int pdf_base_14 = is_base_14 && (needs_exact_metrics || !strchr(fontname, ' '));
    if (pdf_base_14 && (!strncmp(clean_name, "Times", 5) || !strncmp(clean_name, "Helvetica", 9) ||
                        !strncmp(clean_name, "Courier", 7))) {
        return NULL;
    }
    if (needs_exact_metrics) {
        int len;
        if (fz_lookup_base14_font(ctx, fontname, &len)) {
            return NULL;
        }
    }

    fz_font* font = load_mac_font_by_name(ctx, fontname, bold, italic);
    if (font && is_base_14) {
        // use the font's own metrics for base 14 fonts
        font->flags.ft_substitute = 0;
    }
    return font;
}

static fz_font* load_mac_cjk_font(fz_context* ctx, const char* fontname, int ros, int serif) {
    fz_font* font = load_mac_font_by_name(ctx, fontname, 0, 0);
    if (font) {
        return font;
    }
    const char* fallback = NULL;
    switch (ros) {
        case FZ_ADOBE_CNS:
            fallback = serif ? "STSongti-TC-Regular" : "PingFangTC-Regular";
            break;
        case FZ_ADOBE_GB:
            fallback = serif ? "STSongti-SC-Regular" : "PingFangSC-Regular";
            break;
        case FZ_ADOBE_JAPAN:
            fallback = serif ? "HiraMinProN-W3" : "HiraginoSans-W3";
            break;
        case FZ_ADOBE_KOREA:
            fallback = serif ? "AppleMyungjo" : "AppleSDGothicNeo-Regular";
            break;
    }
    return fallback ? load_font_named(ctx, fallback, 0, fallback) : NULL;
}

// fonts that ship with macOS 11+, per script
static fz_font* load_mac_fallback_font(fz_context* ctx, int script, int language, int serif, int bold, int italic) {
    const char* name = NULL;
    (void)language;
    switch (script) {
        case UCDN_SCRIPT_DEVANAGARI:
            name = "KohinoorDevanagari-Regular";
            break;
        case UCDN_SCRIPT_BENGALI:
            name = "KohinoorBangla-Regular";
            break;
        case UCDN_SCRIPT_GURMUKHI:
            name = "GurmukhiMN";
            break;
        case UCDN_SCRIPT_GUJARATI:
            name = "KohinoorGujarati-Regular";
            break;
        case UCDN_SCRIPT_KANNADA:
            name = "KannadaSangamMN";
            break;
        case UCDN_SCRIPT_MALAYALAM:
            name = "MalayalamSangamMN";
            break;
        case UCDN_SCRIPT_SINHALA:
            name = "SinhalaSangamMN";
            break;
        case UCDN_SCRIPT_ORIYA:
            name = "OriyaSangamMN";
            break;
        case UCDN_SCRIPT_TAMIL:
            name = "TamilSangamMN";
            break;
        case UCDN_SCRIPT_TELUGU:
            name = "KohinoorTelugu-Regular";
            break;
        case UCDN_SCRIPT_HEBREW:
            name = bold ? "ArialHebrew-Bold" : "ArialHebrew";
            break;
        case UCDN_SCRIPT_ARABIC:
        case UCDN_SCRIPT_SYRIAC:
        case UCDN_SCRIPT_THAANA:
            name = bold ? "GeezaPro-Bold" : "GeezaPro";
            break;
        case UCDN_SCRIPT_THAI:
            name = bold ? "Thonburi-Bold" : "Thonburi";
            break;
        case UCDN_SCRIPT_LAO:
            name = "LaoSangamMN";
            break;
        case UCDN_SCRIPT_KHMER:
            name = "KhmerSangamMN";
            break;
        case UCDN_SCRIPT_MYANMAR:
            name = "MyanmarSangamMN";
            break;
        case UCDN_SCRIPT_TIBETAN:
            name = "Kailasa";
            break;
        case UCDN_SCRIPT_HAN:
        case UCDN_SCRIPT_BOPOMOFO:
            name = bold ? "PingFangSC-Semibold" : "PingFangSC-Regular";
            break;
        case UCDN_SCRIPT_HIRAGANA:
        case UCDN_SCRIPT_KATAKANA:
            name = bold ? "HiraginoSans-W6" : "HiraginoSans-W3";
            break;
        case UCDN_SCRIPT_HANGUL:
            name = bold ? "AppleSDGothicNeo-Bold" : "AppleSDGothicNeo-Regular";
            break;
        case UCDN_SCRIPT_ETHIOPIC:
            name = "KefaIII";
            break;
        case UCDN_SCRIPT_CANADIAN_ABORIGINAL:
            name = "EuphemiaUCAS";
            break;
        case UCDN_SCRIPT_MONGOLIAN:
            name = "NotoSansMongolian";
            break;
        case UCDN_SCRIPT_ARMENIAN:
            name = "Mshtakan";
            break;
        case UCDN_SCRIPT_GEORGIAN:
            name = "Georgian";
            break;
        case UCDN_SCRIPT_CYRILLIC:
        case UCDN_SCRIPT_GREEK:
        case UCDN_SCRIPT_LATIN:
        case UCDN_SCRIPT_COMMON:
        case UCDN_SCRIPT_INHERITED:
        case UCDN_SCRIPT_UNKNOWN:
            name = serif ? "TimesNewRomanPSMT" : "Helvetica";
            break;
    }
    if (!name) {
        return NULL;
    }
    CTFontSymbolicTraits traits = style_traits(NULL, bold, italic);
    fz_font* font = load_font_named(ctx, name, traits, name);
    if (!font && traits) {
        font = load_font_named(ctx, name, 0, name);
    }
    return font;
}

void install_load_mac_font_funcs(fz_context* ctx) {
    fz_install_load_system_font_funcs(ctx, load_mac_font, load_mac_cjk_font, load_mac_fallback_font);
}
