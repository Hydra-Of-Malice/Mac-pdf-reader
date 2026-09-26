/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Plain C entry points into Cocoa for the portable GUI implementations in this
// directory (PlatformWindowMac.cpp, PasswordDialogMac.cpp).

#ifndef SumatraPDF_GuiMacBridge_h
#define SumatraPDF_GuiMacBridge_h

void MacGuiPostTask(void (*fn)(void*), void* data);

bool MacGuiShowPasswordDialog(void* parent, const char* fileName, int fileNameLen, bool isRetry, bool canRemember,
                              bool rememberPassword, bool showPassword, bool* rememberPasswordOut,
                              bool* showPasswordOut, char** passwordOut, int* passwordLenOut);

#endif
