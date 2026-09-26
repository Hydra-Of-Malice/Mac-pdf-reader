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
void EngineDjvuDec_UnitTests();
void LitDoc_UnitTests();
void MobiDoc_UnitTests();
void PageRenderPolicy_UnitTests();
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

//--- threads get enough stack (Base_posix.cpp StartThread)

constexpr size_t kMinThreadStack = 4 * 1024 * 1024;
constexpr int kRecursionDepth = 2048; // * ~1 KB frame = 2 MB, over macOS's 512 KB default

struct StackCheck {
    AtomicInt done = 0;
    size_t stackSize = 0;
    int recursionSum = 0;
};

static size_t CurrentThreadStackSize() {
#if OS_MAC
    return pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t attr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstacksize(&attr, &size);
        pthread_attr_destroy(&attr);
    }
    return size;
#endif
}

// uses ~1 KB of stack per level; volatile so the frames aren't optimized away
static int Recurse(int depth) {
    volatile char frame[1000];
    frame[0] = (char)depth;
    frame[sizeof(frame) - 1] = 1;
    if (depth == 0) {
        return frame[sizeof(frame) - 1];
    }
    return frame[sizeof(frame) - 1] + Recurse(depth - 1);
}

static void StackCheckThread(StackCheck* check) {
    check->stackSize = CurrentThreadStackSize();
    check->recursionSum = Recurse(kRecursionDepth);
    AtomicIntSet(&check->done, 1);
}

static void ThreadStackTest() {
    StackCheck check;
    ThreadHandle h = StartThread(MkFunc0(StackCheckThread, &check), StrL("stack-check"));
    utassert(h != nullptr);
    for (int waited = 0; waited < 10000 && !AtomicIntGet(&check.done); waited++) {
        SleepInMs(1);
    }
    SafeCloseThreadHandle(&h);
    utassert(AtomicIntGet(&check.done) == 1);
    utassert(check.stackSize >= kMinThreadStack);
    utassert(check.recursionSum == kRecursionDepth + 1);
}

//--- Base_posix.cpp: code pages, normalization

static void CodePageTest() {
    const char* cp1251 =
        "\xcf\xf0\xe8\xe2\xe5\xf2\x2c\x20\xea\xe0\xea\x20\xe4\xe5\xeb\xe0\x3f\x20\xdd\xf2\xee\x20\xf2"
        "\xe5\xf1\xf2\xee\xe2\xfb\xe9\x20\xf2\xe5\xea\xf1\xf2\x2e";
    const char* koi8r =
        "\xf0\xd2\xc9\xd7\xc5\xd4\x2c\x20\xcb\xc1\xcb\x20\xc4\xc5\xcc\xc1\x3f\x20\xfc\xd4\xcf\x20\xd4"
        "\xc5\xd3\xd4\xcf\xd7\xd9\xca\x20\xd4\xc5\xcb\xd3\xd4\x2e";
    const char* sjis =
        "\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd\x81\x41\x82\xb1\x82\xea\x82\xcd\x93\xfa\x96\x7b\x8c"
        "\xea\x82\xcc\x83\x65\x83\x4c\x83\x58\x83\x67\x82\xc5\x82\xb7\x81\x42";
    const char* gbk =
        "\xd5\xe2\xca\xc7\xd2\xbb\xb8\xf6\xd6\xd0\xce\xc4\xb5\xc4\xb2\xe2\xca\xd4\xce\xc4\xb1\xbe\xa3\xac"
        "\xce\xd2\xc3\xc7\xd4\xda\xd5\xe2\xc0\xef\xcb\xb5\xbb\xb0\xa1\xa3";
    const char* korean =
        "\xbe\xc8\xb3\xe7\xc7\xcf\xbc\xbc\xbf\xe4\x20\xc7\xd1\xb1\xb9\xbe\xee\x20\xc5\xd8\xbd\xba\xc6"
        "\xae\xc0\xd4\xb4\xcf\xb4\xd9";
    utassert(GuessTextCodepage(StrL("plain ascii"), CP_ACP) == CP_UTF8);
    utassert(GuessTextCodepage(StrL("caf\xc3\xa9 au lait"), CP_ACP) == CP_UTF8);
    utassert(GuessTextCodepage(StrL("caf\xe9 au lait"), CP_ACP) == CP_ACP);
    utassert(GuessTextCodepage(Str(cp1251), CP_ACP) == 1251);
    utassert(GuessTextCodepage(Str(koi8r), CP_ACP) == 20866);
    utassert(GuessTextCodepage(Str(sjis), CP_ACP) == 932);
    utassert(GuessTextCodepage(Str(gbk), CP_ACP) == 936);
    utassert(GuessTextCodepage(Str(korean), CP_ACP) == 949);

    // "Привет" through iconv both ways
    TempStr utf8 = strconv::ToMultiByteTemp(Str(cp1251, 6), 1251, CP_UTF8);
    utassert(str::Eq(utf8, StrL("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82")));
    TempStr back = strconv::ToMultiByteTemp(utf8, CP_UTF8, 1251);
    utassert(str::Eq(back, Str(cp1251, 6)));

    // U+FB01 (fi ligature) -> "fi"; Linux has no normalization tables
    TempStr nfkc = NormalizeString(StrL("\xef\xac\x81"), 5);
#if OS_MAC
    utassert(str::Eq(nfkc, StrL("fi")));
#else
    utassert(len(nfkc) == 0 || str::Eq(nfkc, StrL("fi")));
#endif
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
    EngineDjvuDec_UnitTests();
    LitDoc_UnitTests();
    MobiDoc_UnitTests();
    PageRenderPolicy_UnitTests();
    PdfDarkModeOklab_UnitTests();
    SimpleLogTest();
    TextSelection_UnitTests();
    ThreadStackTest();
    CodePageTest();

    int res = utassert_print_results();
    DestroyTempArena();
    return res == 0 ? 0 : 1;
}
