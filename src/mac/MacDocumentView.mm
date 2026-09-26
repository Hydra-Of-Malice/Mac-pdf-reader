/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#import <Cocoa/Cocoa.h>

#include <math.h>

#include "mac/SumatraMacEngine.h"
#import "mac/MacDocumentView.h"

static const CGFloat kDragThreshold = 4.0;
static const CGFloat kSpinnerSize = 32.0;
static const double kPrintDpi = 300.0;
static const double kMaxPrintPixels = 64.0 * 1024 * 1024;

static void ReleasePagePixels(void* info, const void* data, size_t size) {
    (void)data;
    (void)size;
    free(info);
}

// Wraps a rendered page into a CGImage without copying the pixels. On success
// the image owns page->data (set to nullptr); on failure the caller still does.
CGImageRef SumatraCreateImage(MacRenderedPage* page) {
    if (!page || !page->data || page->width <= 0 || page->height <= 0 || page->stride < page->width * 4) {
        return nullptr;
    }
    size_t nBytes = (size_t)page->stride * (size_t)page->height;
    CGDataProviderRef provider = CGDataProviderCreateWithData(page->data, page->data, nBytes, ReleasePagePixels);
    if (!provider) {
        return nullptr;
    }
    page->data = nullptr;

    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!colorSpace) {
        colorSpace = CGColorSpaceCreateDeviceRGB();
    }
    CGImageAlphaInfo alpha = page->premultiplied ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaFirst;
    CGBitmapInfo bitmapInfo = (CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)alpha;
    CGImageRef image = CGImageCreate((size_t)page->width, (size_t)page->height, 8, 32, (size_t)page->stride, colorSpace,
                                     bitmapInfo, provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    // the image retains the provider; without an image this frees the pixels
    CGDataProviderRelease(provider);
    return image;
}

// Draws image into r of a flipped view (CGImages are drawn bottom-up).
static void DrawImageInRect(CGContextRef ctx, CGImageRef image, NSRect r) {
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, r.origin.x, r.origin.y + r.size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, r.size.width, r.size.height), image);
    CGContextRestoreGState(ctx);
}

static void FillRects(NSArray* rects, NSColor* color) {
    if ([rects count] == 0) {
        return;
    }
    [color setFill];
    for (NSValue* value in rects) {
        NSRectFillUsingOperation([value rectValue], NSCompositingOperationSourceOver);
    }
}

@implementation SumatraPageImage

- (void)dealloc {
    if (_image) {
        CGImageRelease(_image);
    }
    [_findRects release];
    [_selectionRects release];
    [super dealloc];
}

- (void)setImage:(CGImageRef)image {
    if (_image == image) {
        return;
    }
    if (image) {
        CGImageRetain(image);
    }
    if (_image) {
        CGImageRelease(_image);
    }
    _image = image;
}

@end

enum class DragMode {
    None,
    Select,
    Pan,
    Link,
};

@implementation SumatraDocumentView {
    NSProgressIndicator* _spinner;
    NSTrackingArea* _trackingArea;
    DragMode _dragMode;
    NSPoint _lastDragWindowPoint;
    NSPoint _mouseDownPoint;
    int _linkPage;
    double _linkX;
    double _linkY;
    double _linkZoom;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)dealloc {
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
        [_trackingArea release];
    }
    [_pages release];
    [_message release];
    [_spinner release];
    [super dealloc];
}

- (void)placeSpinner {
    NSRect r = [self visibleRect];
    [_spinner setFrameOrigin:NSMakePoint(floor(NSMidX(r) - kSpinnerSize / 2.0), floor(NSMidY(r) - kSpinnerSize - 24))];
}

- (void)setBusy:(BOOL)busy {
    if (_busy == busy) {
        return;
    }
    _busy = busy;
    if (!busy) {
        [_spinner stopAnimation:nil];
        [_spinner setHidden:YES];
        return;
    }
    if (!_spinner) {
        _spinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, kSpinnerSize, kSpinnerSize)];
        [_spinner setStyle:NSProgressIndicatorStyleSpinning];
        [_spinner setDisplayedWhenStopped:NO];
        [_spinner setAccessibilityLabel:@"Opening document"];
        [self addSubview:_spinner];
    }
    [self placeSpinner];
    [_spinner setHidden:NO];
    [_spinner startAnimation:nil];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (_busy) {
        [self placeSpinner];
    }
}

- (void)setPages:(NSArray*)pages {
    if (_pages != pages) {
        [pages retain];
        [_pages release];
        _pages = pages;
    }
    [self setNeedsDisplay:YES];
}

- (void)setMessage:(NSString*)message {
    if (_message != message) {
        [_message release];
        _message = [message copy];
    }
    [self setNeedsDisplay:YES];
}

#pragma mark - Drawing

- (void)drawMessage {
    NSString* text = _message ?: @"Open a document with File › Open (⌘O), or drop it here.";
    NSMutableParagraphStyle* paragraph = [[[NSMutableParagraphStyle alloc] init] autorelease];
    [paragraph setAlignment:NSTextAlignmentCenter];
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:15],
        NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.85 alpha:1.0],
        NSParagraphStyleAttributeName : paragraph,
    };
    NSRect box = NSInsetRect([self visibleRect], 24, 24);
    if (box.size.width < 10 || box.size.height < 10) {
        return;
    }
    NSRect measured = [text boundingRectWithSize:NSMakeSize(box.size.width, CGFLOAT_MAX)
                                         options:NSStringDrawingUsesLineFragmentOrigin
                                      attributes:attrs];
    CGFloat h = ceil(measured.size.height);
    NSRect r = NSMakeRect(box.origin.x, NSMidY(box) - (h / 2.0), box.size.width, h + 1);
    [text drawWithRect:r options:NSStringDrawingUsesLineFragmentOrigin attributes:attrs];
}

- (void)drawPagePlaceholder:(SumatraPageImage*)page {
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.55 alpha:1.0],
    };
    NSString* text = [NSString stringWithFormat:@"Page %d", [page pageNo]];
    NSSize size = [text sizeWithAttributes:attrs];
    NSRect frame = [page frame];
    NSPoint p = NSMakePoint(NSMidX(frame) - (size.width / 2.0), NSMidY(frame) - (size.height / 2.0));
    [text drawAtPoint:p withAttributes:attrs];
}

- (void)drawRect:(NSRect)dirtyRect {
    [[NSColor colorWithCalibratedWhite:0.18 alpha:1.0] setFill];
    NSRectFill(dirtyRect);
    if ([_pages count] == 0) {
        [self drawMessage];
        return;
    }

    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    NSColor* border = [NSColor colorWithCalibratedWhite:0.05 alpha:1.0];
    NSColor* findColor = [NSColor colorWithCalibratedRed:1.0 green:0.8 blue:0.0 alpha:0.45];
    NSColor* selectionColor = [[NSColor selectedTextBackgroundColor] colorWithAlphaComponent:0.5];
    for (SumatraPageImage* page in _pages) {
        NSRect frame = [page frame];
        if (!NSIntersectsRect(NSInsetRect(frame, -2, -2), dirtyRect)) {
            continue;
        }
        [border setFill];
        NSFrameRectWithWidth(NSInsetRect(frame, -1, -1), 1.0);
        [[NSColor whiteColor] setFill];
        NSRectFill(frame);
        CGImageRef image = [page image];
        if (image && ctx) {
            DrawImageInRect(ctx, image, frame);
        } else {
            [self drawPagePlaceholder:page];
        }
        FillRects([page findRects], findColor);
        FillRects([page selectionRects], selectionColor);
    }
}

#pragma mark - Hit testing

- (SumatraPageImage*)pageAtPoint:(NSPoint)point {
    for (SumatraPageImage* page in _pages) {
        if (NSPointInRect(point, [page frame])) {
            return page;
        }
    }
    return nil;
}

// The page under point, or the vertically closest one (drag-selecting past a page edge).
- (SumatraPageImage*)pageNearestPoint:(NSPoint)point {
    SumatraPageImage* best = nil;
    CGFloat bestDistance = CGFLOAT_MAX;
    for (SumatraPageImage* page in _pages) {
        NSRect f = [page frame];
        if (NSPointInRect(point, f)) {
            return page;
        }
        CGFloat d = 0;
        if (point.y < NSMinY(f)) {
            d = NSMinY(f) - point.y;
        } else if (point.y > NSMaxY(f)) {
            d = point.y - NSMaxY(f);
        }
        if (d < bestDistance) {
            bestDistance = d;
            best = page;
        }
    }
    return best;
}

- (void)updateCursorAtPoint:(NSPoint)point {
    NSCursor* cursor = [NSCursor arrowCursor];
    id<SumatraDocumentViewOwner> owner = _owner;
    void* doc = [owner documentHandle];
    SumatraPageImage* page = doc ? [self pageAtPoint:point] : nil;
    if (page) {
        NSRect f = [page frame];
        double x = point.x - f.origin.x;
        double y = point.y - f.origin.y;
        int rotation = [owner documentRotation];
        MacLink link = {};
        if (MacLinkAtPoint(doc, [page pageNo], x, y, [page layoutZoom], rotation, &link)) {
            MacFreeLink(&link);
            cursor = [NSCursor pointingHandCursor];
        } else if (MacTextAtPoint(doc, [page pageNo], x, y, [page layoutZoom], rotation)) {
            cursor = [NSCursor IBeamCursor];
        }
    }
    [cursor set];
}

- (void)updateTrackingAreas {
    if (_trackingArea) {
        [self removeTrackingArea:_trackingArea];
        [_trackingArea release];
        _trackingArea = nil;
    }
    NSTrackingAreaOptions options = NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingCursorUpdate |
                                    NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil];
    [self addTrackingArea:_trackingArea];
    [super updateTrackingAreas];
}

- (void)mouseMoved:(NSEvent*)event {
    [self updateCursorAtPoint:[self convertPoint:[event locationInWindow] fromView:nil]];
}

- (void)mouseEntered:(NSEvent*)event {
    [self updateCursorAtPoint:[self convertPoint:[event locationInWindow] fromView:nil]];
}

- (void)cursorUpdate:(NSEvent*)event {
    [self updateCursorAtPoint:[self convertPoint:[event locationInWindow] fromView:nil]];
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (_dragMode != DragMode::Pan) {
        [[NSCursor arrowCursor] set];
    }
}

#pragma mark - Mouse

// Click on a link follows it (on mouse up), on text starts a selection
// (double: word, triple: line, shift: extend), elsewhere pans the document.
- (void)mouseDown:(NSEvent*)event {
    id<SumatraDocumentViewOwner> owner = _owner;
    void* doc = [owner documentHandle];
    _dragMode = DragMode::None;
    if (!doc) {
        [super mouseDown:event];
        return;
    }
    [[self window] makeFirstResponder:self];
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    SumatraPageImage* page = [self pageAtPoint:p];
    int rotation = [owner documentRotation];
    if (page) {
        int pageNo = [page pageNo];
        double zoom = [page layoutZoom];
        double x = p.x - [page frame].origin.x;
        double y = p.y - [page frame].origin.y;
        NSInteger clicks = [event clickCount];
        BOOL shift = ([event modifierFlags] & NSEventModifierFlagShift) != 0;
        if (clicks >= 2) {
            MacSelectUnit unit = clicks == 2 ? MacSelectUnit::Word : MacSelectUnit::Line;
            if (MacSelectAt(doc, pageNo, x, y, zoom, rotation, unit)) {
                _dragMode = DragMode::Select;
                [owner documentSelectionChanged];
            }
            return;
        }
        if (shift && MacHasSelection(doc) && MacUpdateSelection(doc, pageNo, x, y, zoom, rotation)) {
            _dragMode = DragMode::Select;
            [owner documentSelectionChanged];
            return;
        }
        MacLink link = {};
        if (MacLinkAtPoint(doc, pageNo, x, y, zoom, rotation, &link)) {
            MacFreeLink(&link);
            _dragMode = DragMode::Link;
            _linkPage = pageNo;
            _linkX = x;
            _linkY = y;
            _linkZoom = zoom;
            _mouseDownPoint = p;
            return;
        }
        if (MacStartSelection(doc, pageNo, x, y, zoom, rotation)) {
            _dragMode = DragMode::Select;
            [owner documentSelectionChanged];
            return;
        }
    }
    if (MacHasSelection(doc)) {
        MacClearSelection(doc);
        [owner documentSelectionChanged];
    }
    _dragMode = DragMode::Pan;
    _lastDragWindowPoint = [event locationInWindow];
    [[NSCursor closedHandCursor] set];
}

- (void)mouseDragged:(NSEvent*)event {
    id<SumatraDocumentViewOwner> owner = _owner;
    void* doc = [owner documentHandle];
    if (!doc) {
        _dragMode = DragMode::None;
        return;
    }
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    if (_dragMode == DragMode::Select) {
        SumatraPageImage* page = [self pageNearestPoint:p];
        if (page) {
            NSRect f = [page frame];
            double x = MAX(0.0, MIN(p.x - f.origin.x, f.size.width));
            double y = MAX(0.0, MIN(p.y - f.origin.y, f.size.height));
            if (MacUpdateSelection(doc, [page pageNo], x, y, [page layoutZoom], [owner documentRotation])) {
                [owner documentSelectionChanged];
            }
        }
        [self autoscroll:event];
        return;
    }
    if (_dragMode == DragMode::Pan) {
        // window coordinates are stable while the view scrolls; y points up
        NSPoint w = [event locationInWindow];
        NSPoint delta = NSMakePoint(_lastDragWindowPoint.x - w.x, w.y - _lastDragWindowPoint.y);
        _lastDragWindowPoint = w;
        [owner documentPanBy:delta];
        return;
    }
    if (_dragMode == DragMode::Link) {
        if (hypot(p.x - _mouseDownPoint.x, p.y - _mouseDownPoint.y) > kDragThreshold) {
            _dragMode = DragMode::None;
        }
    }
}

- (void)mouseUp:(NSEvent*)event {
    DragMode mode = _dragMode;
    _dragMode = DragMode::None;
    if (mode == DragMode::Link) {
        [_owner documentLinkClickedOnPage:_linkPage x:_linkX y:_linkY zoom:_linkZoom];
        return;
    }
    if (mode == DragMode::Pan) {
        [self updateCursorAtPoint:[self convertPoint:[event locationInWindow] fromView:nil]];
        return;
    }
    if (mode == DragMode::None) {
        [super mouseUp:event];
    }
}

- (void)scrollWheel:(NSEvent*)event {
    if ([_owner documentWheelFlip:event]) {
        return;
    }
    [super scrollWheel:event];
}

- (void)magnifyWithEvent:(NSEvent*)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    NSEventPhase phase = [event phase];
    BOOL ending = phase == NSEventPhaseNone || (phase & (NSEventPhaseEnded | NSEventPhaseCancelled)) != 0;
    [_owner documentMagnify:[event magnification] atPoint:p ending:ending];
}

- (void)smartMagnifyWithEvent:(NSEvent*)event {
    [_owner documentSmartMagnifyAtPoint:[self convertPoint:[event locationInWindow] fromView:nil]];
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    (void)event;
    if (![_owner documentHandle]) {
        return nil;
    }
    NSMenu* menu = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    [menu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Use Selection for Find" action:@selector(useSelectionForFind:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Zoom In" action:@selector(zoomIn:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Zoom Out" action:@selector(zoomOut:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Actual Size" action:@selector(zoomActualSize:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Zoom to Fit" action:@selector(zoomFitPage:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Zoom to Width" action:@selector(zoomFitWidth:) keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"Rotate Left" action:@selector(rotateLeft:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Rotate Right" action:@selector(rotateRight:) keyEquivalent:@""];
    return menu;
}

#pragma mark - Keyboard

// Bare keys; the ⌘/⌥/⌃ shortcuts are menu key equivalents.
- (void)keyDown:(NSEvent*)event {
    id<SumatraDocumentViewOwner> owner = _owner;
    NSEventModifierFlags flags = [event modifierFlags];
    BOOL shift = (flags & NSEventModifierFlagShift) != 0;
    BOOL chord = (flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) != 0;
    NSString* chars = [event charactersIgnoringModifiers];
    unichar c = [chars length] ? [chars characterAtIndex:0] : 0;
    if (!owner || chord) {
        [super keyDown:event];
        return;
    }
    switch (c) {
        case NSUpArrowFunctionKey:
        case 'k':
            [owner documentScrollLines:-1];
            return;
        case NSDownArrowFunctionKey:
        case 'j':
            [owner documentScrollLines:1];
            return;
        case NSLeftArrowFunctionKey:
            [owner documentHorizontalArrow:-1];
            return;
        case NSRightArrowFunctionKey:
            [owner documentHorizontalArrow:1];
            return;
        case NSPageUpFunctionKey:
            [owner documentScrollScreens:-1];
            return;
        case NSPageDownFunctionKey:
            [owner documentScrollScreens:1];
            return;
        case ' ':
            [owner documentScrollScreens:shift ? -1 : 1];
            return;
        case NSHomeFunctionKey:
            [owner goToFirstPage:nil];
            return;
        case NSEndFunctionKey:
            [owner goToLastPage:nil];
            return;
        case 'n':
            [owner goToNextPage:nil];
            return;
        case 'p':
            [owner goToPrevPage:nil];
            return;
        case '+':
        case '=':
            [owner zoomIn:nil];
            return;
        case '-':
            [owner zoomOut:nil];
            return;
        case '?':
            [owner showKeyboardShortcuts:nil];
            return;
        case 27: // Escape
            [owner documentCancel];
            return;
        default:
            break;
    }
    [super keyDown:event];
}

#pragma mark - Accessibility

- (BOOL)isAccessibilityElement {
    return YES;
}

- (NSString*)accessibilityRole {
    return NSAccessibilityGroupRole;
}

- (NSString*)accessibilityRoleDescription {
    return @"document";
}

- (NSString*)accessibilityLabel {
    NSString* label = [_owner documentAccessibilityLabel];
    return label ?: @"No document";
}

@end

@implementation SumatraPrintView {
    void* _document;
    int _pageCount;
    int _rotation;
    NSSize _slot;
}

- (instancetype)initWithDocument:(void*)document pageCount:(int)pageCount rotation:(int)rotation {
    self = [super initWithFrame:NSMakeRect(0, 0, 612, 792)];
    if (self) {
        _document = document;
        _pageCount = pageCount;
        _rotation = rotation;
        _slot = NSMakeSize(612, 792);
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

// Pages are stacked vertically, one printable area (paper minus margins) each.
- (NSSize)printableSize {
    NSPrintInfo* info = [[NSPrintOperation currentOperation] printInfo];
    if (!info) {
        info = [NSPrintInfo sharedPrintInfo];
    }
    NSSize paper = [info paperSize];
    CGFloat w = paper.width - [info leftMargin] - [info rightMargin];
    CGFloat h = paper.height - [info topMargin] - [info bottomMargin];
    return NSMakeSize(MAX(w, 72.0), MAX(h, 72.0));
}

- (BOOL)knowsPageRange:(NSRangePointer)range {
    _slot = [self printableSize];
    [self setFrameSize:NSMakeSize(_slot.width, _slot.height * MAX(_pageCount, 1))];
    range->location = 1;
    range->length = (NSUInteger)MAX(_pageCount, 0);
    return YES;
}

- (NSRect)rectForPage:(NSInteger)page {
    return NSMakeRect(0, (CGFloat)(page - 1) * _slot.height, _slot.width, _slot.height);
}

// Each page is shrunk (never enlarged) to fit its slot and centered.
- (void)drawRect:(NSRect)dirtyRect {
    if (_slot.height <= 0 || !_document) {
        return;
    }
    int pageNo = (int)floor(NSMidY(dirtyRect) / _slot.height) + 1;
    if (pageNo < 1 || pageNo > _pageCount) {
        return;
    }
    double w = 0;
    double h = 0;
    if (!MacPageSize(_document, pageNo, &w, &h) || w <= 0 || h <= 0) {
        return;
    }
    double dpi = MacFileDPI(_document);
    if (dpi <= 0) {
        dpi = 96.0;
    }
    double wPt = w * 72.0 / dpi;
    double hPt = h * 72.0 / dpi;
    if (_rotation == 90 || _rotation == 270) {
        double t = wPt;
        wPt = hPt;
        hPt = t;
    }
    NSRect slot = [self rectForPage:pageNo];
    double scale = MIN(1.0, MIN(slot.size.width / wPt, slot.size.height / hPt));
    NSRect target =
        NSMakeRect(floor(slot.origin.x + ((slot.size.width - wPt * scale) / 2.0)),
                   floor(slot.origin.y + ((slot.size.height - hPt * scale) / 2.0)), wPt * scale, hPt * scale);

    double zoom = scale * kPrintDpi / dpi;
    double pixels = (w * zoom) * (h * zoom);
    if (pixels > kMaxPrintPixels) {
        zoom *= sqrt(kMaxPrintPixels / pixels);
    }
    MacRenderedPage page = {};
    if (!MacRenderPageForPrint(_document, pageNo, (float)zoom, _rotation, &page)) {
        return;
    }
    CGImageRef image = SumatraCreateImage(&page);
    MacFreeRenderedPage(&page);
    if (!image) {
        return;
    }
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (ctx) {
        DrawImageInRect(ctx, image, target);
    }
    CGImageRelease(image);
}

@end
