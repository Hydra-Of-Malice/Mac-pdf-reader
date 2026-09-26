/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Unicode normalization with CoreFoundation, for NormalizeString() in
// Base_posix.cpp (Windows uses Win32 NormalizeString()). A C file of its own:
// Apple's headers can't be mixed with base/Base.h.

#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>

// form is a Win32 NORM_FORM value: 1 = NFC, 2 = NFD, 5 = NFKC, 6 = NFKD.
// Returns a malloc()ed NUL-terminated UTF-8 string, NULL on failure.
char* MacNormalizeUtf8(const char* s, int len, int form) {
    CFStringNormalizationForm cfForm;
    switch (form) {
        case 1:
            cfForm = kCFStringNormalizationFormC;
            break;
        case 2:
            cfForm = kCFStringNormalizationFormD;
            break;
        case 5:
            cfForm = kCFStringNormalizationFormKC;
            break;
        case 6:
            cfForm = kCFStringNormalizationFormKD;
            break;
        default:
            return NULL;
    }
    CFStringRef str = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)s, len, kCFStringEncodingUTF8, 0);
    if (!str) {
        return NULL;
    }
    CFMutableStringRef m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, str);
    CFRelease(str);
    if (!m) {
        return NULL;
    }
    CFStringNormalize(m, cfForm);
    CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(m), kCFStringEncodingUTF8) + 1;
    char* res = (char*)malloc((size_t)n);
    if (res && !CFStringGetCString(m, res, n, kCFStringEncodingUTF8)) {
        free(res);
        res = NULL;
    }
    CFRelease(m);
    return res;
}
