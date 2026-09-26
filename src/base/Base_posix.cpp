/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// POSIX (macOS, Linux) versions of the OS-specific parts of Base.cpp

#include "base/Base.h"

#include <errno.h>
#include <iconv.h>
#include <time.h>
#include <unistd.h>
#if OS_LINUX
#include <sys/syscall.h>
#endif

//--- atomics ------------------------------------------------------------------

bool AtomicBoolGet(AtomicBool* p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST) != 0;
}

void AtomicBoolSet(AtomicBool* p, bool v) {
    __atomic_store_n(p, v ? 1 : 0, __ATOMIC_SEQ_CST);
}

bool AtomicBoolSwap(AtomicBool* p, bool v) {
    return __atomic_exchange_n(p, v ? 1 : 0, __ATOMIC_SEQ_CST) != 0;
}

int AtomicIntGet(AtomicInt* p) {
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

void AtomicIntSet(AtomicInt* p, int v) {
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

int AtomicIntAdd(AtomicInt* p, int v) {
    return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST);
}

int AtomicIntInc(AtomicInt* p) {
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}

int AtomicIntDec(AtomicInt* p) {
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}

// stores v and returns what was there before
void* AtomicPtrExchange(AtomicPtr* p, void* v) {
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

// monotonic milliseconds, like Win32 GetTickCount64()
u64 GetTickCount64() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((u64)ts.tv_sec * 1000) + ((u64)ts.tv_nsec / 1000000);
}

//--- threads ------------------------------------------------------------------

ThreadId GetCurrentThreadId() {
#if OS_MAC
    u64 tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return tid;
#else
    return (ThreadId)syscall(SYS_gettid);
#endif
}

// POSIX can only name the calling thread
void SetThreadName(Str threadName, ThreadId threadId) {
    if (len(threadName) == 0) {
        return;
    }
    if (threadId != 0 && threadId != GetCurrentThreadId()) {
        return;
    }
    // Linux limits names to 15 chars + NUL
    char buf[64];
    int maxLen = OS_LINUX ? 15 : sizeofi(buf) - 1;
    int n = std::min(threadName.len, maxLen);
    memcpy(buf, threadName.s, (size_t)n);
    buf[n] = 0;
#if OS_MAC
    pthread_setname_np(buf);
#else
    pthread_setname_np(pthread_self(), buf);
#endif
}

struct ThreadHandlePosix {
    pthread_t thread;
};

struct ThreadFuncData {
    Func0 fn;
    Str threadName;

    ThreadFuncData(const Func0& fn, Str threadName) : fn(fn) { this->threadName = str::Dup(threadName); }
    ~ThreadFuncData() { str::Free(threadName); }
};

static void* ThreadFunc0(void* data) {
    auto* threadData = (ThreadFuncData*)data;
    SetThreadName(threadData->threadName);
    threadData->fn.Call();
    delete threadData;
    DestroyTempArena();
    return nullptr;
}

ThreadHandle StartThread(const Func0& fn, Str threadName) {
    auto* threadData = new ThreadFuncData(fn, threadName);
    auto* hThread = new ThreadHandlePosix();
    int err = pthread_create(&hThread->thread, nullptr, ThreadFunc0, threadData);
    if (err != 0) {
        delete hThread;
        delete threadData;
        return nullptr;
    }
    return hThread;
}

bool SafeCloseThreadHandle(ThreadHandle* hPtr) {
    ThreadHandle h = *hPtr;
    if (!h) {
        return false;
    }
    int err = pthread_detach(h->thread);
    delete h;
    *hPtr = nullptr;
    return err == 0;
}

void RunAsync(const Func0& fn, Str threadName) {
    ThreadHandle hThread = StartThread(fn, threadName);
    SafeCloseThreadHandle(&hThread);
}

void SleepInMs(int ms) {
    if (ms <= 0) {
        return;
    }
    usleep((useconds_t)ms * 1000);
}

//--- code page conversions ----------------------------------------------------

// UTF-8 is converted directly; other Windows code pages through iconv.
// CP_ACP (the Windows "ANSI" code page) is taken to be Windows-1252.

namespace strconv {

constexpr uint kCodePageUsAscii = 20127;

static bool IsUtf8CodePage(uint codePage) {
    return codePage == CP_UTF8 || codePage == kCodePageUsAscii;
}

static const char* IconvName(uint codePage, char* buf, int cbBuf) {
    switch (codePage) {
        case CP_ACP:
            return "CP1252";
        case 1200:
            return "UTF-16LE";
        case 1201:
            return "UTF-16BE";
        case 10000:
            return "MACINTOSH";
        case 20866:
            return "KOI8-R";
        case 21866:
            return "KOI8-U";
        case 20932:
        case 51932:
            return "EUC-JP";
        case 50220:
        case 50221:
        case 50222:
            return "ISO-2022-JP";
        case 51949:
            return "EUC-KR";
        case 54936:
            return "GB18030";
    }
    if (codePage >= 28591 && codePage <= 28606) {
        snprintf(buf, (size_t)cbBuf, "ISO-8859-%u", codePage - 28590);
        return buf;
    }
    snprintf(buf, (size_t)cbBuf, "CP%u", codePage);
    return buf;
}

// Converts src from one iconv encoding to another. An input unit (cbUnit
// bytes) that can't be converted becomes `bad`. Returns a malloc()ed buffer,
// nullptr if iconv doesn't know one of the encodings.
static char* IconvConvert(const char* to, const char* from, Str src, int cbUnit, Str bad, size_t* cbOut) {
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        return nullptr;
    }
    size_t cap = (size_t)src.len * 4 + 16;
    char* res = (char*)malloc(cap);
    char* in = src.s;
    size_t inLeft = (size_t)src.len;
    char* out = res;
    size_t outLeft = cap;
    while (res && inLeft > 0) {
        if (iconv(cd, &in, &inLeft, &out, &outLeft) != (size_t)-1) {
            break;
        }
        if (errno == E2BIG || outLeft < (size_t)bad.len) {
            size_t used = (size_t)(out - res);
            cap *= 2;
            char* bigger = (char*)realloc(res, cap);
            if (!bigger) {
                free(res);
                res = nullptr;
                break;
            }
            res = bigger;
            out = res + used;
            outLeft = cap - used;
            continue;
        }
        // invalid, truncated or unmappable input
        memcpy(out, bad.s, (size_t)bad.len);
        out += bad.len;
        outLeft -= (size_t)bad.len;
        size_t skip = std::min(inLeft, (size_t)cbUnit);
        in += skip;
        inLeft -= skip;
    }
    iconv_close(cd);
    *cbOut = res ? (size_t)(out - res) : 0;
    return res;
}

// null in => null out; empty in => allocated empty out
WStr CodePageToWStr(uint codePage, Str s, Arena* a) {
    if (str::IsNull(s)) {
        return {};
    }
    static_assert(sizeof(WCHAR) == 4, "UTF-32 wchar_t expected");
    if (IsUtf8CodePage(codePage)) {
        int cch = Utf8CodepointCount(s);
        WCHAR* res = AllocArray<WCHAR>(a, cch + 1);
        if (!res) {
            return {};
        }
        int n = 0;
        for (int i = 0; i < s.len && n < cch;) {
            res[n++] = (WCHAR)Utf8CodepointNext(s, i);
        }
        return WStr(res, n);
    }

    char name[32];
    const char* from = IconvName(codePage, name, sizeofi(name));
    const char kReplacementUtf32[] = {(char)0xfd, (char)0xff, 0, 0};
    size_t cb = 0;
    char* conv = IconvConvert("UTF-32LE", from, s, 1, Str(kReplacementUtf32, 4), &cb);
    if (!conv) {
        return {};
    }
    int cch = (int)(cb / sizeof(WCHAR));
    WCHAR* res = AllocArray<WCHAR>(a, cch + 1);
    if (res) {
        memcpy(res, conv, (size_t)cch * sizeof(WCHAR));
    }
    free(conv);
    return res ? WStr(res, cch) : WStr();
}

Str WStrToCodePage(uint codePage, WStr s, Arena* a) {
    // subtle: if s.s is nullptr, we return empty. if empty string => we return empty string
    if (wstr::IsNull(s)) {
        return {};
    }
    if (IsUtf8CodePage(codePage)) {
        int cb = 0;
        char tmp[8];
        for (int i = 0; i < s.len; i++) {
            int off = 0;
            str::Utf8Encode(tmp, off, (int)s.s[i]);
            cb += off;
        }
        char* res = AllocArray<char>(a, cb + 1);
        if (!res) {
            return {};
        }
        int off = 0;
        for (int i = 0; i < s.len; i++) {
            str::Utf8Encode(res, off, (int)s.s[i]);
        }
        return Str(res, off);
    }

    char name[32];
    const char* to = IconvName(codePage, name, sizeofi(name));
    size_t cb = 0;
    Str src((char*)s.s, s.len * sizeofi(WCHAR));
    char* conv = IconvConvert(to, "UTF-32LE", src, sizeofi(WCHAR), StrL("?"), &cb);
    if (!conv) {
        return {};
    }
    char* res = AllocArray<char>(a, (int)cb + 1);
    if (res) {
        memcpy(res, conv, cb);
    }
    free(conv);
    return res ? Str(res, (int)cb) : Str();
}

} // namespace strconv
