/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// POSIX (macOS, Linux) version of UITask.cpp: run a function on the main (UI)
// thread. macOS hands the task to the main dispatch queue, which AppKit's run
// loop drains. Elsewhere tasks wait in a queue until the main thread calls
// DrainQueue().

#include "base/Base.h"

#if OS_MAC
#include <dispatch/dispatch.h>
#endif

#include "base/UITask.h"

namespace uitask {

static ThreadId gMainUIThreadId = 0;
static bool gIsInitialized = false;

struct Task {
    Task* next = nullptr;
    Func0 fn;
};

static Mutex gQueueLock;
static Task* gQueueFirst = nullptr;
static Task* gQueueLast = nullptr;

void Initialize() {
    gMainUIThreadId = GetCurrentThreadId();
    gIsInitialized = true;
}

bool IsMainUIThread() {
    return GetCurrentThreadId() == gMainUIThreadId;
}

static Task* TakeQueue() {
    AutoUnlockMutex lock(&gQueueLock);
    Task* first = gQueueFirst;
    gQueueFirst = nullptr;
    gQueueLast = nullptr;
    return first;
}

// call only from the thread that called Initialize()
void DrainQueue() {
    Task* t = TakeQueue();
    while (t) {
        Task* next = t->next;
        t->fn.Call();
        delete t;
        t = next;
    }
}

// runs what's still queued, then drops tasks posted afterwards
void Destroy() {
    DrainQueue();
    gIsInitialized = false;
}

#if OS_MAC
static void RunTask(void* data) {
    auto* t = (Task*)data;
    t->fn.Call();
    delete t;
}
#endif

void Post(const Func0& fn, Kind) {
    if (!gIsInitialized) {
        return;
    }
    auto* t = new Task();
    t->fn = fn;
#if OS_MAC
    dispatch_async_f(dispatch_get_main_queue(), t, RunTask);
#else
    AutoUnlockMutex lock(&gQueueLock);
    if (gQueueLast) {
        gQueueLast->next = t;
    } else {
        gQueueFirst = t;
    }
    gQueueLast = t;
#endif
}

void PostOptimized(const Func0& fn, Kind kind) {
    if (IsMainUIThread()) {
        // already on the ui thread: run it now
        fn.Call();
        return;
    }
    Post(fn, kind);
}

} // namespace uitask
