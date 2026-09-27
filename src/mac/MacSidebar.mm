/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Document sidebar: an outline pane (NSOutlineView over the bridge's flat ToC)
// and a thumbnail pane (NSTableView over MacThumbnails). Manual retain/release,
// like SumatraMac.mm. Everything here runs on the main thread except
// ThumbReady(), which only hops to the main queue.

#import <Cocoa/Cocoa.h>

#include "mac/SumatraMacEngine.h"
#include "mac/MacThumbnails.h"
#import "mac/MacSidebar.h"

static const CGFloat kSidebarPadding = 8.0;
static const CGFloat kModeControlHeight = 24.0;
static const CGFloat kEmptyLabelHeight = 40.0;
static const CGFloat kThumbHMargin = 14.0;
static const CGFloat kThumbTopPad = 8.0;
static const CGFloat kThumbLabelGap = 3.0;
static const CGFloat kThumbLabelHeight = 16.0;
static const CGFloat kThumbBottomPad = 6.0;
static const CGFloat kThumbMinWidth = 32.0;
// thumbnail width changes in steps so a resize doesn't re-render on every pixel
static const CGFloat kThumbWidthStep = 16.0;
// very tall pages are fit into a box at most this many times higher than wide
static const CGFloat kThumbMaxAspect = 2.0;
static const int kThumbPrefetchRows = 3;
static const long long kThumbCacheBytes = 64LL * 1024 * 1024;

static NSString* const kOutlineCellId = @"sumatra.sidebar.outline-cell";
static NSString* const kThumbCellId = @"sumatra.sidebar.thumb-cell";
static NSString* const kNoDocumentText = @"No document open";
static NSString* const kNoOutlineText = @"No outline";

@class SumatraThumbPane;

@interface SumatraSidebar ()
- (void)goToPage:(int)pageNo;
- (void)layoutViews;
- (void)backingChanged;
@end

#pragma mark - Helpers

static NSValue* DocKey(void* document) {
    return [NSValue valueWithPointer:document];
}

static NSString* OneLine(NSString* s) {
    NSArray* parts = [s componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
    return [parts componentsJoinedByString:@" "];
}

static NSString* StringFromBridge(char* s) {
    NSString* res = s ? [NSString stringWithUTF8String:s] : nil;
    MacFreeString(s);
    return res;
}

static void ReleaseThumbPixels(void* info, const void* data, size_t size) {
    (void)data;
    (void)size;
    MacThumbsReleaseImage(info);
}

// Wraps the cached pixels without copying; consumes thumb->ref.
static NSImage* ImageFromThumb(MacThumbImage* thumb, NSSize pointSize) {
    size_t nBytes = (size_t)thumb->stride * (size_t)thumb->height;
    CGDataProviderRef provider = CGDataProviderCreateWithData(thumb->ref, thumb->data, nBytes, ReleaseThumbPixels);
    if (!provider) {
        MacThumbsReleaseImage(thumb->ref);
        return nil;
    }
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo bitmapInfo = (CGBitmapInfo)kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaPremultipliedFirst;
    CGImageRef cgImage = CGImageCreate((size_t)thumb->width, (size_t)thumb->height, 8, 32, (size_t)thumb->stride,
                                       colorSpace, bitmapInfo, provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    if (!cgImage) {
        return nil;
    }
    NSImage* image = [[[NSImage alloc] initWithCGImage:cgImage size:pointSize] autorelease];
    CGImageRelease(cgImage);
    return image;
}

#pragma mark - SumatraOutlineNode

@interface SumatraOutlineNode : NSObject
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* url;
@property(nonatomic) int pageNo;
@property(nonatomic) int tocIndex;
@property(nonatomic) BOOL isOpen;
@property(nonatomic, assign) SumatraOutlineNode* parent;
@property(nonatomic, readonly) NSMutableArray* children;
@end

@implementation SumatraOutlineNode

- (instancetype)init {
    self = [super init];
    if (self) {
        _children = [[NSMutableArray alloc] init];
    }
    return self;
}

- (void)dealloc {
    [_title release];
    [_url release];
    [_children release];
    [super dealloc];
}

@end

#pragma mark - SumatraSidebarRootView

@interface SumatraSidebarRootView : NSView
@property(nonatomic, assign) SumatraSidebar* owner;
@end

@implementation SumatraSidebarRootView

- (BOOL)isFlipped {
    return YES;
}

- (void)resizeSubviewsWithOldSize:(NSSize)oldSize {
    (void)oldSize;
    [_owner layoutViews];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [_owner backingChanged];
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [_owner backingChanged];
}

@end

#pragma mark - SumatraOutlinePane

@interface SumatraOutlinePane : NSObject <NSOutlineViewDataSource, NSOutlineViewDelegate>
- (instancetype)initWithSidebar:(SumatraSidebar*)sidebar;
- (NSScrollView*)scrollView;
- (void)showDocument:(void*)document;
- (void)reloadDocument;
- (void)forgetDocument:(void*)document;
- (BOOL)hasItems;
- (void)syncToPage:(int)pageNo;
@end

@implementation SumatraOutlinePane {
    SumatraSidebar* _sidebar; // owns us
    NSScrollView* _scrollView;
    NSOutlineView* _outlineView;
    SumatraOutlineNode* _root;
    NSMutableArray* _flat;                // nodes in document order
    NSMutableDictionary* _savedExpansion; // DocKey -> NSIndexSet of expanded tocIndex
    void* _document;
    BOOL _syncingSelection;
}

- (instancetype)initWithSidebar:(SumatraSidebar*)sidebar {
    self = [super init];
    if (!self) {
        return nil;
    }
    _sidebar = sidebar;
    _root = [[SumatraOutlineNode alloc] init];
    _flat = [[NSMutableArray alloc] init];
    _savedExpansion = [[NSMutableDictionary alloc] init];

    _outlineView = [[NSOutlineView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    NSTableColumn* column = [[[NSTableColumn alloc] initWithIdentifier:@"title"] autorelease];
    [column setResizingMask:NSTableColumnAutoresizingMask];
    [_outlineView addTableColumn:column];
    [_outlineView setOutlineTableColumn:column];
    [_outlineView setHeaderView:nil];
    [_outlineView setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
    [_outlineView setAutoresizesOutlineColumn:NO];
    [_outlineView setAllowsMultipleSelection:NO];
    [_outlineView setAllowsEmptySelection:YES];
    [_outlineView setAccessibilityLabel:@"Outline"];
    [_outlineView setDataSource:self];
    [_outlineView setDelegate:self];
    [_outlineView setTarget:self];
    [_outlineView setAction:@selector(outlineClicked:)];

    _scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    [_scrollView setHasVerticalScroller:YES];
    [_scrollView setHasHorizontalScroller:NO];
    [_scrollView setAutohidesScrollers:YES];
    [_scrollView setBorderType:NSNoBorder];
    [_scrollView setDocumentView:_outlineView];
    return self;
}

- (void)dealloc {
    // the view may outlive us in the host's window; make it drop our items
    [_outlineView setTarget:nil];
    [_outlineView setDataSource:nil];
    [_outlineView setDelegate:nil];
    [_outlineView reloadData];
    [_outlineView release];
    [_scrollView release];
    [_root release];
    [_flat release];
    [_savedExpansion release];
    [super dealloc];
}

- (NSScrollView*)scrollView {
    return _scrollView;
}

- (BOOL)hasItems {
    return [[_root children] count] > 0;
}

- (void)saveExpansion {
    if (!_document) {
        return;
    }
    NSMutableIndexSet* expanded = [NSMutableIndexSet indexSet];
    for (SumatraOutlineNode* node in _flat) {
        if ([_outlineView isItemExpanded:node]) {
            [expanded addIndex:(NSUInteger)[node tocIndex]];
        }
    }
    [_savedExpansion setObject:expanded forKey:DocKey(_document)];
}

// Builds the tree from the bridge's flat ToC: each item's parent is the
// nearest preceding item one level up.
- (void)buildTree {
    int count = _document ? MacTocItemCount(_document) : 0;
    NSMutableArray* stack = [NSMutableArray arrayWithObject:_root];
    for (int i = 0; i < count; i++) {
        SumatraOutlineNode* node = [[[SumatraOutlineNode alloc] init] autorelease];
        NSString* title = StringFromBridge(MacCopyTocItemTitle(_document, i));
        [node setTitle:title ? OneLine(title) : @""];
        [node setTocIndex:i];
        [node setPageNo:MacTocItemPage(_document, i)];
        [node setIsOpen:MacTocItemIsOpen(_document, i)];
        if ([node pageNo] <= 0) {
            [node setUrl:StringFromBridge(MacCopyTocItemUrl(_document, i))];
        }

        NSUInteger depth = (NSUInteger)MAX(0, MacTocItemDepth(_document, i));
        NSUInteger parentLevel = MIN(depth, [stack count] - 1);
        while ([stack count] > parentLevel + 1) {
            [stack removeLastObject];
        }
        SumatraOutlineNode* parent = [stack lastObject];
        [node setParent:parent == _root ? nil : parent];
        [[parent children] addObject:node];
        [stack addObject:node];
        [_flat addObject:node];
    }
}

// Expands what the user had expanded last time, else the document's defaults.
// Document order visits parents first, so a node is expanded only below expanded parents.
- (void)restoreExpansion {
    NSIndexSet* saved = _document ? [_savedExpansion objectForKey:DocKey(_document)] : nil;
    NSArray* topLevel = [_root children];
    for (SumatraOutlineNode* node in _flat) {
        if ([[node children] count] == 0) {
            continue;
        }
        BOOL open = [node isOpen];
        if (saved) {
            open = [saved containsIndex:(NSUInteger)[node tocIndex]];
        } else if ([topLevel count] == 1 && [node parent] == nil) {
            open = YES;
        }
        SumatraOutlineNode* parent = [node parent];
        if (!open || (parent && ![_outlineView isItemExpanded:parent])) {
            continue;
        }
        [_outlineView expandItem:node];
    }
}

- (void)showDocument:(void*)document {
    if (document == _document) {
        return;
    }
    [self saveExpansion];
    _document = document;

    // NSOutlineView doesn't retain items: keep the old tree alive until it reloaded
    SumatraOutlineNode* oldRoot = _root;
    NSMutableArray* oldFlat = _flat;
    _root = [[SumatraOutlineNode alloc] init];
    _flat = [[NSMutableArray alloc] init];
    [self buildTree];
    _syncingSelection = YES;
    [_outlineView deselectAll:nil];
    [_outlineView reloadData];
    [self restoreExpansion];
    if ([_outlineView numberOfRows] > 0) {
        [_outlineView scrollRowToVisible:0];
    }
    _syncingSelection = NO;
    [oldRoot release];
    [oldFlat release];
}

// Items cache their page numbers: rebuild after the pages were renumbered.
- (void)reloadDocument {
    void* document = _document;
    [self saveExpansion];
    _document = nullptr;
    [self showDocument:document];
}

- (void)forgetDocument:(void*)document {
    [_savedExpansion removeObjectForKey:DocKey(document)];
}

// The item for pageNo: highest page not after it, the last such in document order.
- (SumatraOutlineNode*)nodeForPage:(int)pageNo {
    SumatraOutlineNode* best = nil;
    for (SumatraOutlineNode* node in _flat) {
        int nodePage = [node pageNo];
        if (nodePage > 0 && nodePage <= pageNo && (!best || nodePage >= [best pageNo])) {
            best = node;
        }
    }
    return best;
}

- (void)syncToPage:(int)pageNo {
    if (!_document || ![self hasItems] || [_scrollView isHidden]) {
        return;
    }
    // keep a selection the user made among items starting on the same page
    SumatraOutlineNode* node = [self nodeForPage:pageNo];
    NSInteger selected = [_outlineView selectedRow];
    SumatraOutlineNode* selectedNode = selected >= 0 ? [_outlineView itemAtRow:selected] : nil;
    if (node && selectedNode && [selectedNode pageNo] == [node pageNo]) {
        return;
    }

    // select the item, or its nearest visible ancestor if it's collapsed away
    NSInteger row = node ? [_outlineView rowForItem:node] : -1;
    while (node && row < 0) {
        node = [node parent];
        row = node ? [_outlineView rowForItem:node] : -1;
    }
    _syncingSelection = YES;
    if (row < 0) {
        [_outlineView deselectAll:nil];
    } else {
        [_outlineView selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
        [_outlineView scrollRowToVisible:row];
    }
    _syncingSelection = NO;
}

- (void)openNode:(SumatraOutlineNode*)node allowUrl:(BOOL)allowUrl {
    if (!node) {
        return;
    }
    if ([node pageNo] > 0) {
        [_sidebar goToPage:[node pageNo]];
        return;
    }
    if (!allowUrl || ![node url]) {
        return;
    }
    NSURL* url = [NSURL URLWithString:[node url]];
    if (!url) {
        NSString* escaped = [[node url]
            stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]];
        url = escaped ? [NSURL URLWithString:escaped] : nil;
    }
    if (url) {
        [[NSWorkspace sharedWorkspace] openURL:url];
    }
}

// Clicks also re-open the selected item; only clicks open URLs (not arrow keys).
- (void)outlineClicked:(id)sender {
    (void)sender;
    NSInteger row = [_outlineView clickedRow];
    if (row < 0) {
        return;
    }
    NSEvent* event = [NSApp currentEvent];
    if (event) {
        NSPoint p = [_outlineView convertPoint:[event locationInWindow] fromView:nil];
        if (NSPointInRect(p, [_outlineView frameOfOutlineCellAtRow:row])) {
            return; // the disclosure triangle only expands / collapses
        }
    }
    [self openNode:[_outlineView itemAtRow:row] allowUrl:YES];
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification {
    (void)notification;
    NSInteger row = [_outlineView selectedRow];
    if (_syncingSelection || row < 0) {
        return;
    }
    [self openNode:[_outlineView itemAtRow:row] allowUrl:NO];
}

- (NSInteger)outlineView:(NSOutlineView*)outlineView numberOfChildrenOfItem:(id)item {
    (void)outlineView;
    SumatraOutlineNode* node = item ? (SumatraOutlineNode*)item : _root;
    return (NSInteger)[[node children] count];
}

- (id)outlineView:(NSOutlineView*)outlineView child:(NSInteger)index ofItem:(id)item {
    (void)outlineView;
    SumatraOutlineNode* node = item ? (SumatraOutlineNode*)item : _root;
    return [[node children] objectAtIndex:(NSUInteger)index];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item {
    (void)outlineView;
    return [[(SumatraOutlineNode*)item children] count] > 0;
}

- (NSView*)outlineView:(NSOutlineView*)outlineView viewForTableColumn:(NSTableColumn*)tableColumn item:(id)item {
    (void)tableColumn;
    NSTableCellView* cell = [outlineView makeViewWithIdentifier:kOutlineCellId owner:self];
    if (!cell) {
        cell = [[[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 200, 20)] autorelease];
        [cell setIdentifier:kOutlineCellId];
        NSTextField* text = [NSTextField labelWithString:@""];
        [text setFrame:NSMakeRect(2, 2, 196, 16)];
        [text setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin];
        [text setLineBreakMode:NSLineBreakByTruncatingTail];
        [cell addSubview:text];
        [cell setTextField:text];
    }
    NSString* title = [(SumatraOutlineNode*)item title];
    [[cell textField] setStringValue:title];
    [cell setToolTip:title];
    return cell;
}

@end

#pragma mark - SumatraThumbCellView

@interface SumatraThumbCellView : NSTableCellView
@property(nonatomic) int pageNo;
@property(nonatomic) NSSize thumbSize; // points
@property(nonatomic, retain) NSImage* thumbImage;
@property(nonatomic) BOOL hasExactImage;
@property(nonatomic) CGFloat imageScale; // backing scale the image was rendered for
@property(nonatomic, retain) NSColor* pageColor; // under the thumbnail; nil: white
@end

@implementation SumatraThumbCellView

- (void)dealloc {
    [_thumbImage release];
    [_pageColor release];
    [super dealloc];
}

- (BOOL)isFlipped {
    return YES;
}

- (void)setPageNo:(int)pageNo {
    _pageNo = pageNo;
    NSString* label = [NSString stringWithFormat:@"Page %d", pageNo];
    [self setAccessibilityElement:YES];
    [self setAccessibilityRole:NSAccessibilityImageRole];
    [self setAccessibilityLabel:label];
    [self setToolTip:label];
    [self setNeedsDisplay:YES];
}

- (void)setThumbImage:(NSImage*)image {
    if (_thumbImage == image) {
        return;
    }
    [_thumbImage release];
    _thumbImage = [image retain];
    [self setNeedsDisplay:YES];
}

- (void)setBackgroundStyle:(NSBackgroundStyle)backgroundStyle {
    [super setBackgroundStyle:backgroundStyle];
    [self setNeedsDisplay:YES];
}

- (NSRect)thumbRect {
    NSRect bounds = [self bounds];
    return NSMakeRect(floor((bounds.size.width - _thumbSize.width) / 2.0), kThumbTopPad, _thumbSize.width,
                      _thumbSize.height);
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect thumb = [self thumbRect];
    [(_pageColor ?: [NSColor whiteColor]) setFill];
    NSRectFill(thumb);
    if (_thumbImage) {
        NSDictionary* hints = @{NSImageHintInterpolation : @(NSImageInterpolationHigh)};
        [_thumbImage drawInRect:thumb
                       fromRect:NSZeroRect
                      operation:NSCompositingOperationSourceOver
                       fraction:1.0
                 respectFlipped:YES
                          hints:hints];
    }
    [[NSColor colorWithCalibratedWhite:0.55 alpha:1.0] setStroke];
    [NSBezierPath strokeRect:NSInsetRect(thumb, -0.5, -0.5)];

    BOOL selected = [self backgroundStyle] == NSBackgroundStyleEmphasized;
    NSColor* color = selected ? [NSColor alternateSelectedControlTextColor] : [NSColor secondaryLabelColor];
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:11],
        NSForegroundColorAttributeName : color,
    };
    NSString* text = [NSString stringWithFormat:@"%d", _pageNo];
    NSSize size = [text sizeWithAttributes:attrs];
    NSPoint p = NSMakePoint(floor(NSMidX(thumb) - (size.width / 2.0)), NSMaxY(thumb) + kThumbLabelGap);
    [text drawAtPoint:p withAttributes:attrs];
}

@end

#pragma mark - SumatraThumbPane

@interface SumatraThumbRelay : NSObject
@property(nonatomic, assign) SumatraThumbPane* pane;
@end

@implementation SumatraThumbRelay
@end

@interface SumatraThumbPane : NSObject <NSTableViewDataSource, NSTableViewDelegate>
- (instancetype)initWithSidebar:(SumatraSidebar*)sidebar;
- (NSScrollView*)scrollView;
- (void)showDocument:(void*)document pageCount:(int)pageCount rotation:(int)rotation;
- (void)forgetDocument:(void*)document;
- (void)setActive:(BOOL)active;
- (void)syncToPage:(int)pageNo;
- (void)layoutChanged;
- (void)thumbnailReady:(void*)document page:(int)pageNo;
- (void)shutdown;
- (int)visibleImageCount;
- (double)visibleImageLuminance;
- (void)setPageColor:(NSColor*)color;
- (void)reloadThumbnails;
@end

// Worker thread. The block retains relay; the pane clears relay.pane before it
// goes away, and no callback runs after MacThumbsDestroy() returns.
static void ThumbReady(void* context, void* document, int pageNo) {
    SumatraThumbRelay* relay = (SumatraThumbRelay*)context;
    dispatch_async(dispatch_get_main_queue(), ^{
      [[relay pane] thumbnailReady:document page:pageNo];
    });
}

@implementation SumatraThumbPane {
    SumatraSidebar* _sidebar; // owns us
    NSScrollView* _scrollView;
    NSTableView* _table;
    SumatraThumbRelay* _relay;
    void* _thumbs;
    void* _document;
    int _pageCount;
    int _rotation;
    int _currentPage;
    double* _pageSizes; // unrotated width, height per page
    CGFloat _thumbWidth;
    CGFloat _backingScale;
    NSColor* _pageColor;
    BOOL _active;
    BOOL _syncingSelection;
}

- (instancetype)initWithSidebar:(SumatraSidebar*)sidebar {
    self = [super init];
    if (!self) {
        return nil;
    }
    _sidebar = sidebar;
    _backingScale = MAX(1.0, [[NSScreen mainScreen] backingScaleFactor]);
    _thumbWidth = kThumbMinWidth;

    _table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    NSTableColumn* column = [[[NSTableColumn alloc] initWithIdentifier:@"thumbnail"] autorelease];
    [column setResizingMask:NSTableColumnAutoresizingMask];
    [_table addTableColumn:column];
    [_table setHeaderView:nil];
    [_table setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
    [_table setIntercellSpacing:NSMakeSize(0, 0)];
    [_table setAllowsMultipleSelection:NO];
    [_table setAllowsEmptySelection:YES];
    if (@available(macOS 11.0, *)) {
        [_table setStyle:NSTableViewStyleFullWidth];
    }
    [_table setAccessibilityLabel:@"Page thumbnails"];
    [_table setDataSource:self];
    [_table setDelegate:self];
    [_table setTarget:self];
    [_table setAction:@selector(tableClicked:)];

    _scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 200, 200)];
    [_scrollView setHasVerticalScroller:YES];
    [_scrollView setHasHorizontalScroller:NO];
    [_scrollView setAutohidesScrollers:YES];
    [_scrollView setBorderType:NSNoBorder];
    [_scrollView setDocumentView:_table];

    NSClipView* clip = [_scrollView contentView];
    [clip setPostsBoundsChangedNotifications:YES];
    [_scrollView setPostsFrameChangedNotifications:YES];
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(clipBoundsChanged:) name:NSViewBoundsDidChangeNotification object:clip];
    [center addObserver:self
               selector:@selector(scrollFrameChanged:)
                   name:NSViewFrameDidChangeNotification
                 object:_scrollView];

    _relay = [[SumatraThumbRelay alloc] init];
    [_relay setPane:self];
    _thumbs = MacThumbsCreate(ThumbReady, _relay, kThumbCacheBytes);
    return self;
}

- (void)dealloc {
    [self shutdown];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_table setTarget:nil];
    [_table setDataSource:nil];
    [_table setDelegate:nil];
    [_table reloadData];
    [_table release];
    [_scrollView release];
    [_relay release];
    free(_pageSizes);
    [super dealloc];
}

// Stops the worker and drops the cache; the pane shows nothing afterwards.
- (void)shutdown {
    if (_thumbs) {
        MacThumbsDestroy(_thumbs);
        _thumbs = nullptr;
    }
    [_relay setPane:nil];
}

- (NSScrollView*)scrollView {
    return _scrollView;
}

- (CGFloat)currentBackingScale {
    NSWindow* window = [_scrollView window];
    CGFloat scale = window ? [window backingScaleFactor] : [[NSScreen mainScreen] backingScaleFactor];
    return scale > 0 ? scale : 1.0;
}

- (void)loadPageSizes {
    free(_pageSizes);
    _pageSizes = nullptr;
    if (!_document || _pageCount <= 0) {
        return;
    }
    _pageSizes = (double*)calloc((size_t)_pageCount * 2, sizeof(double));
    if (!_pageSizes) {
        return;
    }
    for (int i = 0; i < _pageCount; i++) {
        MacPageSize(_document, i + 1, &_pageSizes[2 * i], &_pageSizes[(2 * i) + 1]);
    }
}

// Point size of pageNo's thumbnail: the rotated page fit into the thumbnail box.
- (NSSize)thumbSizeForPage:(int)pageNo {
    double w = 0, h = 0;
    if (_pageSizes && pageNo >= 1 && pageNo <= _pageCount) {
        w = _pageSizes[2 * (pageNo - 1)];
        h = _pageSizes[(2 * (pageNo - 1)) + 1];
    }
    if (w <= 0 || h <= 0) {
        w = 210;
        h = 297;
    }
    if (_rotation == 90 || _rotation == 270) {
        double t = w;
        w = h;
        h = t;
    }
    double scale = MIN(_thumbWidth / w, (_thumbWidth * kThumbMaxAspect) / h);
    return NSMakeSize(MAX(1.0, floor(w * scale)), MAX(1.0, floor(h * scale)));
}

- (BOOL)updateThumbWidth {
    CGFloat avail = [_scrollView contentSize].width - (2 * kThumbHMargin);
    CGFloat width = MAX(kThumbMinWidth, floor(avail / kThumbWidthStep) * kThumbWidthStep);
    if (width == _thumbWidth) {
        return NO;
    }
    _thumbWidth = width;
    return YES;
}

- (void)showDocument:(void*)document pageCount:(int)pageCount rotation:(int)rotation {
    BOOL sameDoc = document == _document && pageCount == _pageCount;
    if (sameDoc && rotation == _rotation) {
        return;
    }
    // visible rows re-request what they need after the reload
    MacThumbsPrune(_thumbs, nullptr, 0, 1, 0);
    _document = document;
    _pageCount = document ? pageCount : 0;
    _rotation = rotation;
    _currentPage = 0;
    if (!sameDoc) {
        [self loadPageSizes];
    }
    [self updateThumbWidth];
    _syncingSelection = YES;
    [_table deselectAll:nil];
    [_table reloadData];
    _syncingSelection = NO;
}

- (void)forgetDocument:(void*)document {
    MacThumbsForget(_thumbs, document);
}

- (void)setActive:(BOOL)active {
    if (_active == active) {
        return;
    }
    _active = active;
    if (!_active) {
        MacThumbsPrune(_thumbs, nullptr, 0, 1, 0);
        return;
    }
    [self layoutChanged];
    [self syncToPage:_currentPage];
    [self visibleRowsChanged];
}

- (void)syncToPage:(int)pageNo {
    _currentPage = pageNo;
    if (!_document || pageNo < 1 || pageNo > _pageCount) {
        return;
    }
    NSInteger row = pageNo - 1;
    if ([_table selectedRow] != row) {
        _syncingSelection = YES;
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
        _syncingSelection = NO;
    }
    if (_active) {
        [_table scrollRowToVisible:row];
    }
}

- (void)goToRow:(NSInteger)row {
    if (row >= 0 && row < _pageCount) {
        [_sidebar goToPage:(int)row + 1];
    }
}

- (void)tableClicked:(id)sender {
    (void)sender;
    [self goToRow:[_table clickedRow]];
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    (void)notification;
    if (!_syncingSelection) {
        [self goToRow:[_table selectedRow]];
    }
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    (void)tableView;
    return _document ? _pageCount : 0;
}

- (CGFloat)tableView:(NSTableView*)tableView heightOfRow:(NSInteger)row {
    (void)tableView;
    NSSize size = [self thumbSizeForPage:(int)row + 1];
    return kThumbTopPad + size.height + kThumbLabelGap + kThumbLabelHeight + kThumbBottomPad;
}

- (BOOL)cellIsCurrent:(SumatraThumbCellView*)cell {
    return [cell hasExactImage] && [cell imageScale] == _backingScale;
}

// Shows the best cached image and requests the exact size if it's missing.
- (void)loadCell:(SumatraThumbCellView*)cell priority:(MacThumbPriority)priority {
    if (!_document) {
        return;
    }
    int pageNo = [cell pageNo];
    NSSize size = [cell thumbSize];
    int dx = (int)ceil(size.width * _backingScale);
    int dy = (int)ceil(size.height * _backingScale);
    MacThumbImage thumb = {};
    if (MacThumbsGet(_thumbs, _document, pageNo, _rotation, dx, dy, &thumb)) {
        BOOL exact = thumb.exact;
        NSImage* image = ImageFromThumb(&thumb, size);
        if (image) {
            [cell setThumbImage:image];
            [cell setHasExactImage:exact];
            [cell setImageScale:_backingScale];
            if (exact) {
                return;
            }
        }
    }
    if (_active) {
        MacThumbsRequest(_thumbs, _document, pageNo, _rotation, dx, dy, priority);
    }
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)tableColumn row:(NSInteger)row {
    (void)tableColumn;
    SumatraThumbCellView* cell = [tableView makeViewWithIdentifier:kThumbCellId owner:self];
    if (!cell) {
        cell = [[[SumatraThumbCellView alloc] initWithFrame:NSMakeRect(0, 0, 200, 100)] autorelease];
        [cell setIdentifier:kThumbCellId];
    }
    int pageNo = (int)row + 1;
    [cell setPageNo:pageNo];
    [cell setThumbSize:[self thumbSizeForPage:pageNo]];
    [cell setThumbImage:nil];
    [cell setHasExactImage:NO];
    [cell setPageColor:_pageColor];
    BOOL visible = NSIntersectsRect([tableView rectOfRow:row], [tableView visibleRect]);
    [self loadCell:cell priority:visible ? MacThumbPriority::Visible : MacThumbPriority::Prefetch];
    return cell;
}

// After scrolling or resizing: drop requests far from view, fill visible cells
// that lack an exact image and prefetch a few rows around them.
- (void)visibleRowsChanged {
    if (!_active || !_document || _pageCount <= 0) {
        return;
    }
    NSRange rows = [_table rowsInRect:[_table visibleRect]];
    if (rows.length == 0) {
        return;
    }
    int first = (int)rows.location + 1;
    int last = (int)(rows.location + rows.length);
    MacThumbsPrune(_thumbs, _document, _rotation, first - kThumbPrefetchRows, last + kThumbPrefetchRows);

    for (int pageNo = first; pageNo <= last; pageNo++) {
        SumatraThumbCellView* cell = [_table viewAtColumn:0 row:pageNo - 1 makeIfNecessary:NO];
        if (!cell) {
            continue;
        }
        NSSize size = [self thumbSizeForPage:pageNo];
        if (!NSEqualSizes(size, [cell thumbSize])) {
            [cell setThumbSize:size];
            [cell setHasExactImage:NO];
            [cell setNeedsDisplay:YES];
        }
        if (![self cellIsCurrent:cell]) {
            [self loadCell:cell priority:MacThumbPriority::Visible];
        }
    }

    int prefetch[2 * kThumbPrefetchRows];
    int n = 0;
    for (int i = 1; i <= kThumbPrefetchRows; i++) {
        prefetch[n++] = last + i;
        prefetch[n++] = first - i;
    }
    for (int i = 0; i < n; i++) {
        int pageNo = prefetch[i];
        if (pageNo < 1 || pageNo > _pageCount) {
            continue;
        }
        NSSize size = [self thumbSizeForPage:pageNo];
        int dx = (int)ceil(size.width * _backingScale);
        int dy = (int)ceil(size.height * _backingScale);
        MacThumbsRequest(_thumbs, _document, pageNo, _rotation, dx, dy, MacThumbPriority::Prefetch);
    }
}

- (void)clipBoundsChanged:(NSNotification*)notification {
    (void)notification;
    [self visibleRowsChanged];
}

- (void)scrollFrameChanged:(NSNotification*)notification {
    (void)notification;
    [self layoutChanged];
}

// Width or backing scale changed: resize rows and re-request visible thumbnails.
- (void)layoutChanged {
    [_table sizeLastColumnToFit];
    CGFloat scale = [self currentBackingScale];
    BOOL scaleChanged = scale != _backingScale;
    _backingScale = scale;
    BOOL widthChanged = [self updateThumbWidth];
    if (widthChanged && _pageCount > 0) {
        NSIndexSet* allRows = [NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, (NSUInteger)_pageCount)];
        [_table noteHeightOfRowsWithIndexesChanged:allRows];
    }
    if (widthChanged || scaleChanged) {
        [self visibleRowsChanged];
    }
}

- (void)thumbnailReady:(void*)document page:(int)pageNo {
    if (!_active || !document || document != _document || pageNo < 1 || pageNo > _pageCount) {
        return;
    }
    SumatraThumbCellView* cell = [_table viewAtColumn:0 row:pageNo - 1 makeIfNecessary:NO];
    if (cell && ![self cellIsCurrent:cell]) {
        [self loadCell:cell priority:MacThumbPriority::Visible];
    }
}

- (void)setPageColor:(NSColor*)color {
    if (_pageColor != color) {
        [_pageColor release];
        _pageColor = [color retain];
    }
}

// Document colors changed: the thumbnails are rendered again.
- (void)reloadThumbnails {
    void* document = _document;
    int pageCount = _pageCount;
    int rotation = _rotation;
    [self showDocument:nullptr pageCount:0 rotation:0];
    [self showDocument:document pageCount:pageCount rotation:rotation];
    [self syncToPage:[_sidebar currentPage]];
}

// Mean luminance (0..1) of the first thumbnail on screen, -1 if none (self-test).
- (double)visibleImageLuminance {
    if (!_active || !_document) {
        return -1;
    }
    NSRange rows = [_table rowsInRect:[_table visibleRect]];
    for (NSUInteger row = rows.location; row < rows.location + rows.length; row++) {
        SumatraThumbCellView* cell = [_table viewAtColumn:0 row:(NSInteger)row makeIfNecessary:NO];
        NSImage* image = [cell thumbImage];
        if (!image || ![cell hasExactImage]) {
            continue;
        }
        const NSInteger n = 32;
        NSBitmapImageRep* rep = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                         pixelsWide:n
                                                                         pixelsHigh:n
                                                                      bitsPerSample:8
                                                                    samplesPerPixel:4
                                                                           hasAlpha:YES
                                                                           isPlanar:NO
                                                                     colorSpaceName:NSDeviceRGBColorSpace
                                                                        bytesPerRow:0
                                                                       bitsPerPixel:0] autorelease];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
        [image drawInRect:NSMakeRect(0, 0, n, n)];
        [NSGraphicsContext restoreGraphicsState];
        double sum = 0;
        NSUInteger pixel[4] = {};
        for (NSInteger y = 0; y < n; y++) {
            for (NSInteger x = 0; x < n; x++) {
                [rep getPixel:pixel atX:x y:y];
                sum += (0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]) / 255.0;
            }
        }
        return sum / (double)(n * n);
    }
    return -1;
}

- (int)visibleImageCount {
    if (!_active || !_document) {
        return 0;
    }
    NSRange rows = [_table rowsInRect:[_table visibleRect]];
    int count = 0;
    for (NSUInteger row = rows.location; row < rows.location + rows.length; row++) {
        SumatraThumbCellView* cell = [_table viewAtColumn:0 row:(NSInteger)row makeIfNecessary:NO];
        if ([cell thumbImage]) {
            count++;
        }
    }
    return count;
}

@end

#pragma mark - SumatraSidebar

@implementation SumatraSidebar {
    id<SumatraSidebarHost> _host; // owns us
    SumatraSidebarRootView* _rootView;
    NSSegmentedControl* _modeControl;
    NSTextField* _emptyLabel;
    SumatraOutlinePane* _outlinePane;
    SumatraThumbPane* _thumbPane;
    SumatraSidebarMode _mode;
    void* _document;
    int _currentPage;
}

- (instancetype)initWithHost:(id<SumatraSidebarHost>)host {
    self = [super init];
    if (!self) {
        return nil;
    }
    _host = host;
    _mode = SumatraSidebarModeOutline;

    _rootView = [[SumatraSidebarRootView alloc] initWithFrame:NSMakeRect(0, 0, 220, 480)];
    [_rootView setOwner:self];

    _modeControl = [[NSSegmentedControl alloc] initWithFrame:NSMakeRect(0, 0, 200, kModeControlHeight)];
    [_modeControl setSegmentCount:2];
    [_modeControl setLabel:@"Outline" forSegment:SumatraSidebarModeOutline];
    [_modeControl setLabel:@"Thumbnails" forSegment:SumatraSidebarModeThumbnails];
    [_modeControl setToolTip:@"Show the table of contents" forSegment:SumatraSidebarModeOutline];
    [_modeControl setToolTip:@"Show page thumbnails" forSegment:SumatraSidebarModeThumbnails];
    [_modeControl setTrackingMode:NSSegmentSwitchTrackingSelectOne];
    [_modeControl setSegmentDistribution:NSSegmentDistributionFillEqually];
    [_modeControl setSelectedSegment:_mode];
    [_modeControl setAccessibilityLabel:@"Sidebar view"];
    [_modeControl setTarget:self];
    [_modeControl setAction:@selector(modeControlChanged:)];
    [_rootView addSubview:_modeControl];

    _outlinePane = [[SumatraOutlinePane alloc] initWithSidebar:self];
    _thumbPane = [[SumatraThumbPane alloc] initWithSidebar:self];
    [_rootView addSubview:[_outlinePane scrollView]];
    [_rootView addSubview:[_thumbPane scrollView]];

    _emptyLabel = [[NSTextField labelWithString:kNoDocumentText] retain];
    [_emptyLabel setAlignment:NSTextAlignmentCenter];
    [_emptyLabel setTextColor:[NSColor secondaryLabelColor]];
    [_emptyLabel setLineBreakMode:NSLineBreakByWordWrapping];
    [_rootView addSubview:_emptyLabel];

    [self layoutViews];
    [self updateVisibility];
    return self;
}

- (void)dealloc {
    [self shutdown];
    [_rootView setOwner:nil];
    [_modeControl setTarget:nil];
    [_rootView release];
    [_modeControl release];
    [_emptyLabel release];
    [_outlinePane release];
    [_thumbPane release];
    [super dealloc];
}

- (NSView*)view {
    return _rootView;
}

- (SumatraSidebarMode)mode {
    return _mode;
}

- (void)setMode:(SumatraSidebarMode)mode {
    if (mode != SumatraSidebarModeThumbnails) {
        mode = SumatraSidebarModeOutline;
    }
    _mode = mode;
    [self updateVisibility];
    [_outlinePane syncToPage:_currentPage];
    [_thumbPane syncToPage:_currentPage];
}

- (void)modeControlChanged:(id)sender {
    (void)sender;
    [self setMode:(SumatraSidebarMode)[_modeControl selectedSegment]];
}

- (void)layoutViews {
    NSRect bounds = [_rootView bounds];
    CGFloat width = bounds.size.width;
    [_modeControl setFrame:NSMakeRect(kSidebarPadding, kSidebarPadding, MAX(0.0, width - (2 * kSidebarPadding)),
                                      kModeControlHeight)];

    CGFloat top = (2 * kSidebarPadding) + kModeControlHeight;
    NSRect content = NSMakeRect(0, top, width, MAX(0.0, bounds.size.height - top));
    [[_outlinePane scrollView] setFrame:content];
    [[_thumbPane scrollView] setFrame:content];

    CGFloat labelY = top + MAX(0.0, floor((content.size.height - kEmptyLabelHeight) / 3.0));
    [_emptyLabel setFrame:NSMakeRect(12, labelY, MAX(0.0, width - 24), kEmptyLabelHeight)];
}

- (void)backingChanged {
    [_thumbPane layoutChanged];
}

- (void)updateVisibility {
    BOOL outline = _mode == SumatraSidebarModeOutline;
    NSString* empty = nil;
    if (!_document) {
        empty = kNoDocumentText;
    } else if (outline && ![_outlinePane hasItems]) {
        empty = kNoOutlineText;
    }
    [[_outlinePane scrollView] setHidden:!outline || empty != nil];
    [[_thumbPane scrollView] setHidden:outline || empty != nil];
    [_thumbPane setActive:!outline && empty == nil];
    [_emptyLabel setStringValue:empty ?: @""];
    [_emptyLabel setHidden:empty == nil];
    [_modeControl setSelectedSegment:_mode];
}

- (void)goToPage:(int)pageNo {
    if (_document && pageNo >= 1) {
        [_host sidebarGoToPage:pageNo];
    }
}

// Safe to call often: the same document only refreshes page, page count and rotation.
- (void)documentChanged {
    void* document = [_host sidebarDocumentHandle];
    int pageCount = document ? MacPageCount(document) : 0;
    if (pageCount <= 0) {
        document = nullptr;
    }
    _document = document;
    _currentPage = document ? [_host sidebarCurrentPage] : 0;
    int rotation = document ? [_host sidebarDocumentRotation] : 0;

    [_outlinePane showDocument:document];
    [_thumbPane showDocument:document pageCount:pageCount rotation:rotation];
    [self updateVisibility];
    [_outlinePane syncToPage:_currentPage];
    [_thumbPane syncToPage:_currentPage];
}

// Same document, pages renumbered (chapters laid out): drop the cached
// thumbnails and outline page numbers.
- (void)documentPagesChanged {
    void* document = _document;
    if (document) {
        [_thumbPane showDocument:nullptr pageCount:0 rotation:0];
        [_thumbPane forgetDocument:document];
        [_outlinePane reloadDocument];
    }
    [self documentChanged];
}

- (void)currentPageChanged:(int)pageNo {
    if (!_document) {
        return;
    }
    _currentPage = pageNo;
    [_outlinePane syncToPage:pageNo];
    [_thumbPane syncToPage:pageNo];
}

- (void)documentWillClose:(void*)documentHandle {
    if (!documentHandle) {
        return;
    }
    if (documentHandle == _document) {
        _document = nullptr;
        _currentPage = 0;
        [_outlinePane showDocument:nullptr];
        [_thumbPane showDocument:nullptr pageCount:0 rotation:0];
        [self updateVisibility];
    }
    [_thumbPane forgetDocument:documentHandle];
    [_outlinePane forgetDocument:documentHandle];
}

- (void)shutdown {
    [_thumbPane shutdown];
}

- (int)visibleThumbnailCount {
    return _mode == SumatraSidebarModeThumbnails ? [_thumbPane visibleImageCount] : 0;
}

- (double)visibleThumbnailLuminance {
    return _mode == SumatraSidebarModeThumbnails ? [_thumbPane visibleImageLuminance] : -1;
}

- (int)currentPage {
    return _currentPage;
}

- (void)setPageColor:(NSColor*)color {
    [_thumbPane setPageColor:color];
}

- (void)documentColorsChanged {
    if (_document) {
        [_thumbPane reloadThumbnails];
    }
}

@end
