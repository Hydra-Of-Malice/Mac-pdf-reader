/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Command palette and keyboard shortcuts panels. Import after <Cocoa/Cocoa.h>.
// Main thread only.

#ifndef SumatraPDF_MacPanels_h
#define SumatraPDF_MacPanels_h

NSMenuItem* SumatraRunCommandPalette(NSWindow* parent);
void SumatraShowKeyboardShortcuts(void);
NSString* SumatraShortcutText(NSMenuItem* item);

#endif
