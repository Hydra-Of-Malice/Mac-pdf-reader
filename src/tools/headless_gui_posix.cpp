/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// The GUI hooks the portable reader code calls, for command-line tools that
// link it without the Cocoa layer (src/gui/mac/*.cpp in the app): posted tasks
// wait in uitask's queue (run them with uitask::DrainQueue() after
// uitask::Initialize()), and there is no password dialog to show.

#include "base/Base.h"
#include "base/UITask.h"

#include "gui/UIModels.h"
#include "EngineBase.h"
#include "gui/PlatformWindow.h"
#include "gui/PasswordDialog.h"

void PlatformPostTask(const Func0& fn) {
    uitask::Post(fn);
}

bool ShowPasswordDialog(const PasswordDialogArgs&, PasswordDialogResult* result) {
    if (result) {
        *result = {};
    }
    return false;
}
