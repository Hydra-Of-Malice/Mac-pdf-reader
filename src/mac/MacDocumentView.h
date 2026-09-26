/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Page canvas (inside the main NSScrollView) and print view of the macOS app.
// Import after <Cocoa/Cocoa.h> and mac/SumatraMacEngine.h. Main thread only.

#ifndef SumatraPDF_MacDocumentView_h
#define SumatraPDF_MacDocumentView_h

CGImageRef SumatraCreateImage(MacRenderedPage* page);

// Implemented by the app delegate. Points are in document view coordinates
// (flipped, points); page-local x/y are relative to a page's frame origin.
@protocol SumatraDocumentViewOwner <NSObject>
- (void*)documentHandle;
- (int)documentRotation;
- (NSString*)documentAccessibilityLabel;
- (void)documentLinkClickedOnPage:(int)pageNo x:(double)x y:(double)y zoom:(double)zoom;
- (void)documentSelectionChanged;
- (void)documentScrollLines:(int)lines;
- (void)documentScrollScreens:(int)screens;
- (void)documentHorizontalArrow:(int)direction;
- (void)documentPanBy:(NSPoint)delta;
- (void)documentMagnify:(CGFloat)magnification atPoint:(NSPoint)point ending:(BOOL)ending;
- (void)documentSmartMagnifyAtPoint:(NSPoint)point;
- (BOOL)documentWheelFlip:(NSEvent*)event;
- (void)documentCancel;
- (IBAction)goToNextPage:(id)sender;
- (IBAction)goToPrevPage:(id)sender;
- (IBAction)goToFirstPage:(id)sender;
- (IBAction)goToLastPage:(id)sender;
- (IBAction)zoomIn:(id)sender;
- (IBAction)zoomOut:(id)sender;
- (IBAction)showKeyboardShortcuts:(id)sender;
@end

// One visible page of the current layout.
@interface SumatraPageImage : NSObject
@property(nonatomic) int pageNo;
@property(nonatomic) NSRect frame;
@property(nonatomic) double layoutZoom;
@property(nonatomic) CGImageRef image;                // retained; may be a lower-resolution placeholder
@property(nonatomic, retain) NSArray* findRects;      // NSValue rects, document view coordinates
@property(nonatomic, retain) NSArray* selectionRects; // NSValue rects, document view coordinates
@end

@interface SumatraDocumentView : NSView
@property(nonatomic, retain) NSArray* pages;  // SumatraPageImage
@property(nonatomic, copy) NSString* message; // shown when there are no pages
@property(nonatomic) BOOL busy;               // spinner above the message (document opening)
@property(nonatomic, assign) id<SumatraDocumentViewOwner> owner;
- (SumatraPageImage*)pageAtPoint:(NSPoint)point;
@end

@interface SumatraPrintView : NSView
- (instancetype)initWithDocument:(void*)document pageCount:(int)pageCount rotation:(int)rotation;
@end

#endif
