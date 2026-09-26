/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"

#include "gui/mac/GuiMacBridge.h"
#include "gui/PlatformWindow.h"

// The macOS app only needs PlatformPostTask() (PageRenderService and the find
// worker post completions with it); its windows are native Cocoa.

struct PostedTask {
    Func0 fn;
};

static void RunPostedTask(void* data) {
    auto* task = (PostedTask*)data;
    task->fn.Call();
    delete task;
}

// Runs fn later on the main thread (the Cocoa main queue).
void PlatformPostTask(const Func0& fn) {
    MacGuiPostTask(RunPostedTask, new PostedTask{fn});
}
