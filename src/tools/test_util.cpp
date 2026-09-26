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

    int res = utassert_print_results();
    DestroyTempArena();
    return res == 0 ? 0 : 1;
}
