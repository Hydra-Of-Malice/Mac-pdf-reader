/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Self-test mode (-self-test <report.json>): drives the running app through its
// menus and controls, checks the results and writes a JSON report plus PNG
// snapshots. Import after <Cocoa/Cocoa.h>. Main thread only, except
// -passwordForAttempt:, which the document loader calls.

#ifndef SumatraPDF_MacSelfTest_h
#define SumatraPDF_MacSelfTest_h

// What the self-test can observe of the app.
struct SumatraTestState {
    int tabCount;
    bool hasTab;
    bool loading; // the active tab's document is still opening
    int pageCount;
    int currentPage;
    double zoom; // factor, or -1 fit page / -2 fit width
    double displayZoom;
    int rotation;
    bool continuous;
    double pageOffsetX; // view origin relative to the current page's top-left, points
    double pageOffsetY;
    bool rendered; // every visible page shows its exact render
    bool findPending;
    int findPage; // page of the current find hit, 0 if none
    bool hasSelection;
    bool selecting; // Select All is extracting text in the background
    bool findBarVisible;
    bool sidebarVisible;
    int thumbnails; // sidebar thumbnails on screen with an image
    int lastOpenError; // MacOpenError of the last failed open
};

@protocol SumatraSelfTestHost <NSObject>
- (NSWindow*)selfTestWindow;
- (NSView*)selfTestDocumentView;
- (NSSearchField*)selfTestSearchField;
- (NSTextField*)selfTestPageField;
- (NSPasteboard*)pasteboard;
- (void*)documentHandle;
- (NSString*)selfTestActivePath;
- (struct SumatraTestState)selfTestState;
- (void)openPaths:(NSArray*)paths;
- (void)selfTestReloadPrefs;
- (BOOL)selfTestPrintToPDF:(NSString*)path firstPage:(int)first lastPage:(int)last;
- (void)selfTestFinished:(int)exitCode;
@end

@interface SumatraSelfTest : NSObject
- (instancetype)initWithHost:(id<SumatraSelfTestHost>)host
                  reportPath:(NSString*)reportPath
                manifestPath:(NSString*)manifestPath
                       paths:(NSArray*)paths
                    findWord:(NSString*)findWord
                     timeout:(double)timeoutSeconds;
- (void)start;
- (void)recordAlert:(NSString*)message info:(NSString*)info;
- (void)recordIgnored:(NSString*)what;
- (const char*)passwordForAttempt:(int)attempt;
@end

#endif
