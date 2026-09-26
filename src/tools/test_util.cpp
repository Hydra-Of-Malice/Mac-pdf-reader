/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests of the code the macOS app shares with Windows, as a command-line
// tool for macOS and Linux (cmd/build.ts -mac-core / -mac). The Windows app
// runs the full set with -unit-tests (src/tests/Sumatra_ut.cpp).
//   test_util [-for-ai]

#include "base/Base.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

// in src/base/tests/
void BaseUtilTest();
void ByteOrderTests();
void CryptoUtilTest();
void CssParser_UnitTests();
void DictTest();
void GuessFileTypeTest();
void JsonTest();
void SettingsUtilTest();
void SquareTreeTest();
void StrFormatTest();
void StrTest();
void StrVecTest();
void VecTest();

// in src/tests/
void CachedObjects_UnitTests();
void ChapterTable_UnitTests();
void CommandPaletteModel_UnitTests();
void EngineDjvuDec_UnitTests();
void LitDoc_UnitTests();
void MobiDoc_UnitTests();
void PageRenderPolicy_UnitTests();
void PdfDarkModeImageClassifier_UnitTests();
void PdfDarkModeOklab_UnitTests();
void SimpleLogTest();
void TextSelection_UnitTests();

// hooks the Windows app implements (AppSettings.cpp); engines ask them for ebook settings
struct EBookUI;
EBookUI* GetEBookUI() {
    return nullptr;
}

struct FileEBookUI;
FileEBookUI* GetFileEBookUI(Str) {
    return nullptr;
}

int main(int argc, char** argv) {
    bool forAi = false;
    for (int i = 1; i < argc; i++) {
        if (str::Eq(Str(argv[i]), StrL("-for-ai"))) {
            forAi = true;
        }
    }
    if (forAi) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        utassert_set_for_ai(true);
    }
    printf("Running unit tests\n");

    BaseUtilTest();
    ByteOrderTests();
    CryptoUtilTest();
    CssParser_UnitTests();
    DictTest();
    GuessFileTypeTest();
    JsonTest();
    SettingsUtilTest();
    SquareTreeTest();
    StrFormatTest();
    StrTest();
    StrVecTest();
    VecTest();

    CachedObjects_UnitTests();
    ChapterTable_UnitTests();
    CommandPaletteModel_UnitTests();
    EngineDjvuDec_UnitTests();
    LitDoc_UnitTests();
    MobiDoc_UnitTests();
    PageRenderPolicy_UnitTests();
    PdfDarkModeImageClassifier_UnitTests();
    PdfDarkModeOklab_UnitTests();
    SimpleLogTest();
    TextSelection_UnitTests();

    int res = utassert_print_results();
    DestroyTempArena();
    return res == 0 ? 0 : 1;
}
