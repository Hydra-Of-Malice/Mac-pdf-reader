/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Document sidebar: outline (table of contents) and page thumbnails.
// Import once (#import); needs <Cocoa/Cocoa.h>. Main thread only.

#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, SumatraSidebarMode) { SumatraSidebarModeOutline = 0, SumatraSidebarModeThumbnails = 1 };

@protocol SumatraSidebarHost <NSObject>
- (void*)sidebarDocumentHandle;      // bridge document handle of the active tab, or NULL
- (int)sidebarCurrentPage;           // 1-based
- (int)sidebarDocumentRotation;      // degrees
- (void)sidebarGoToPage:(int)pageNo; // 1-based
@end

@interface SumatraSidebar : NSObject
- (instancetype)initWithHost:(id<SumatraSidebarHost>)host;
@property (readonly) NSView* view;
@property (nonatomic) SumatraSidebarMode mode;
- (void)documentChanged;             // tab switched / doc opened / closed / reloaded (main thread)
- (void)currentPageChanged:(int)pageNo;
- (void)documentWillClose:(void*)documentHandle; // called BEFORE MacCloseDocument; stop/await in-flight work for it
- (void)shutdown;                    // stops the thumbnail worker; call before MacShutdown()
- (int)visibleThumbnailCount;        // thumbnails on screen that show an image (self-test)
@end
