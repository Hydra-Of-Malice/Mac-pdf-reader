/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// The Cocoa application shell: window, tabs, toolbar, menus, sidebar, find,
// printing and document lifecycle. Manual retain/release (no ARC); AppKit is
// only touched on the main thread. Engine access goes through the plain C
// bridge (SumatraMacEngine.h); see docs/mac/architecture.md.

#import <Cocoa/Cocoa.h>

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "mac/SumatraMacEngine.h"
#include "mac/MacPrefs.h"
#include "gui/mac/GuiMacBridge.h"
#import "mac/MacDocumentView.h"
#import "mac/MacPanels.h"
#import "mac/MacSelfTest.h"
#import "mac/MacSidebar.h"

static NSString* const kWebsiteURL = @"https://www.sumatrapdfreader.org";
static NSString* const kManualURL = @"https://www.sumatrapdfreader.org/manual";

// zoom is a factor (1 = actual size) or one of the fit modes below; the fit
// values match DocumentLayoutParams::zoomVirtual (kZoomFitPage, kZoomFitWidth)
static const CGFloat kMacZoomFitPage = -1.0;
static const CGFloat kMacZoomFitWidth = -2.0;
static const CGFloat kZoomMin = 0.1;
static const CGFloat kZoomMax = 8.0;
static const CGFloat kZoomLevels[] = {0.1,  0.125, 0.25, 0.3333, 0.5, 0.6667, 0.75, 1.0,
                                      1.25, 1.5,   2.0,  3.0,    4.0, 6.0,    8.0};

static const CGFloat kLineScroll = 40.0;
static const CGFloat kPageTopGap = 8.0;
static const CGFloat kPrintMargin = 18.0;
static const double kReloadDelay = 0.5;
static const double kWheelFlipInterval = 0.3;
static const NSUInteger kMaxClosedTabs = 10;
static const NSUInteger kMaxHistory = 50;
static const NSInteger kTabMenuSeparatorTag = 7001;
static const NSUInteger kMaxTabLabelChars = 24;
static const CGFloat kFindBarHeight = 30;
static const int kMaxConcurrentOpens = 4;
static const int kSelectAllSyncPages = 50;
static const double kLoadingIndicatorDelay = 0.3;

static const CGFloat kSidebarDefaultWidth = 220;
static const CGFloat kSidebarMinWidth = 140;
static const CGFloat kSidebarMaxWidth = 480;
static const CGFloat kDocumentMinWidth = 240;

static NSString* const kDefSidebarVisible = @"SidebarVisible";
static NSString* const kDefSidebarWidth = @"SidebarWidth";
static NSString* const kDefSidebarMode = @"SidebarMode";

static NSString* const kSettingsFileName = @"SumatraPDF-settings.txt";
static NSString* const kSelfTestPasteboard = @"org.sumatrapdfreader.self-test";
static const double kSelfTestTimeout = 1500.0;
static const double kSelfTestStartDelay = 0.5;

// command-line flags; see ParseCommandLine()
static NSString* const kArgForTesting = @"-for-testing";
static NSString* const kArgPrefsDir = @"-prefs-dir";
static NSString* const kArgSelfTest = @"-self-test";
static NSString* const kArgSelfTestManifest = @"-self-test-manifest";
static NSString* const kArgSelfTestFind = @"-self-test-find";
static NSString* const kArgSelfTestTimeout = @"-self-test-timeout";
static NSString* const kArgPaths = @"paths";

static NSString* const kToolbarIdentifier = @"sumatra.toolbar.v2";
static NSString* const kToolbarSidebar = @"sumatra.toolbar.sidebar";
static NSString* const kToolbarOpen = @"sumatra.toolbar.open";
static NSString* const kToolbarTabs = @"sumatra.toolbar.tabs";
static NSString* const kToolbarPrevPage = @"sumatra.toolbar.prev-page";
static NSString* const kToolbarNextPage = @"sumatra.toolbar.next-page";
static NSString* const kToolbarPage = @"sumatra.toolbar.page";
static NSString* const kToolbarZoomOut = @"sumatra.toolbar.zoom-out";
static NSString* const kToolbarZoomActual = @"sumatra.toolbar.zoom-actual";
static NSString* const kToolbarZoomIn = @"sumatra.toolbar.zoom-in";
static NSString* const kToolbarFitPage = @"sumatra.toolbar.fit-page";
static NSString* const kToolbarFitWidth = @"sumatra.toolbar.fit-width";
static NSString* const kToolbarRotateLeft = @"sumatra.toolbar.rotate-left";
static NSString* const kToolbarRotateRight = @"sumatra.toolbar.rotate-right";
static NSString* const kToolbarSearch = @"sumatra.toolbar.search";

enum class PagePosition {
    Top,
    Bottom,
};

enum class Highlight {
    Find,
    Selection,
};

// A point of the view expressed relative to a page, so it can be kept in
// place across zoom, rotation and resize relayouts.
struct ViewAnchor {
    int pageNo;
    double fx;
    double fy;
    NSPoint clipPoint;
};

#pragma mark - Helpers

static const char* FsPath(NSString* path) {
    if ([path length] == 0) {
        return nullptr;
    }
    return [path fileSystemRepresentation];
}

static NSString* StringFromFs(const char* s) {
    if (!s || !s[0]) {
        return nil;
    }
    return [[NSFileManager defaultManager] stringWithFileSystemRepresentation:s length:strlen(s)];
}

static NSString* StringFromUtf8(const char* s) {
    if (!s) {
        return nil;
    }
    return [NSString stringWithUTF8String:s];
}

static NSString* CanonicalPath(NSString* path) {
    return [[path stringByStandardizingPath] stringByResolvingSymlinksInPath];
}

static NSString* ExistingPath(NSArray* candidates) {
    NSFileManager* fileManager = [NSFileManager defaultManager];
    for (NSString* candidate in candidates) {
        if ([fileManager fileExistsAtPath:candidate]) {
            return candidate;
        }
    }
    return [candidates count] ? [candidates objectAtIndex:0] : @"";
}

// Relative command-line paths: the shell's directory, then the repository
// root for a development build (out/<config>/SumatraPDF.app).
static NSString* ResolveDocumentPath(NSString* path) {
    path = [path stringByStandardizingPath];
    if ([path isAbsolutePath]) {
        return path;
    }
    NSMutableArray* candidates = [NSMutableArray arrayWithObject:path];
    NSString* pwd = [[[NSProcessInfo processInfo] environment] objectForKey:@"PWD"];
    if ([pwd length] > 0) {
        [candidates addObject:[[pwd stringByAppendingPathComponent:path] stringByStandardizingPath]];
    }
    NSString* cwd = [[NSFileManager defaultManager] currentDirectoryPath];
    if ([cwd length] > 0) {
        [candidates addObject:[[cwd stringByAppendingPathComponent:path] stringByStandardizingPath]];
    }
    NSString* bundlePath = [[NSBundle mainBundle] bundlePath];
    NSString* repoRoot = [[[bundlePath stringByDeletingLastPathComponent] stringByDeletingLastPathComponent]
        stringByDeletingLastPathComponent];
    [candidates addObject:[[repoRoot stringByAppendingPathComponent:path] stringByStandardizingPath]];
    return ExistingPath(candidates);
}

// argv: flags (kArg*; some take the next argument as value) and document paths
// (under kArgPaths). Launch Services may add -psn_*; AppKit treats -NS*/-Apple*
// as user-default overrides that take a value.
static NSDictionary* ParseCommandLine() {
    NSArray* args = [[NSProcessInfo processInfo] arguments];
    NSArray* valueFlags = @[ kArgPrefsDir, kArgSelfTest, kArgSelfTestManifest, kArgSelfTestFind, kArgSelfTestTimeout ];
    NSMutableDictionary* opts = [NSMutableDictionary dictionary];
    NSMutableArray* paths = [NSMutableArray array];
    for (NSUInteger i = 1; i < [args count]; i++) {
        NSString* arg = [args objectAtIndex:i];
        if ([arg hasPrefix:@"-psn_"]) {
            continue;
        }
        if ([arg hasPrefix:@"-NS"] || [arg hasPrefix:@"-Apple"]) {
            i++;
            continue;
        }
        if ([valueFlags containsObject:arg]) {
            if (i + 1 < [args count]) {
                [opts setObject:[args objectAtIndex:++i] forKey:arg];
            }
            continue;
        }
        if ([arg length] == 0) {
            continue;
        }
        if ([arg hasPrefix:@"-"]) {
            [opts setObject:[NSNumber numberWithBool:YES] forKey:arg];
            continue;
        }
        [paths addObject:arg];
    }
    [opts setObject:paths forKey:kArgPaths];
    return opts;
}

static NSString* AbsolutePath(NSString* path) {
    path = [path stringByExpandingTildeInPath];
    if (![path isAbsolutePath]) {
        path = [[[NSFileManager defaultManager] currentDirectoryPath] stringByAppendingPathComponent:path];
    }
    return [path stringByStandardizingPath];
}

static NSDate* ModificationDate(NSString* path) {
    NSDictionary* attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
    return [attrs objectForKey:NSFileModificationDate];
}

static NSString* ShortTabLabel(NSString* name) {
    if ([name length] <= kMaxTabLabelChars) {
        return name ?: @"";
    }
    NSUInteger half = (kMaxTabLabelChars - 1) / 2;
    return [NSString
        stringWithFormat:@"%@…%@", [name substringToIndex:half], [name substringFromIndex:[name length] - half]];
}

static NSImage* TextImage(NSString* text) {
    NSDictionary* attrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName : [NSColor blackColor],
    };
    NSSize textSize = [text sizeWithAttributes:attrs];
    NSSize size = NSMakeSize(ceil(textSize.width) + 2, 18);
    NSImage* image = [NSImage imageWithSize:size
                                    flipped:NO
                             drawingHandler:^BOOL(NSRect rect) {
                               (void)rect;
                               [text drawAtPoint:NSMakePoint(1, floor((18 - textSize.height) / 2.0))
                                   withAttributes:attrs];
                               return YES;
                             }];
    [image setTemplate:YES];
    return image;
}

// SF Symbol (macOS 11+) or a small text fallback.
static NSImage* ToolbarImage(NSString* symbolName, NSString* description, NSString* fallbackText) {
    NSImage* image = nil;
    if ([NSImage respondsToSelector:@selector(imageWithSystemSymbolName:accessibilityDescription:)]) {
        image = [NSImage imageWithSystemSymbolName:symbolName accessibilityDescription:description];
    }
    if (!image) {
        return TextImage(fallbackText);
    }
    [image setTemplate:YES];
    return image;
}

static NSString* KeyString(unichar c) {
    return [NSString stringWithCharacters:&c length:1];
}

static NSMenuItem* AddItem(NSMenu* menu, NSString* title, SEL action, id target, NSString* key,
                           NSEventModifierFlags modifiers) {
    NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key ?: @""] autorelease];
    [item setTarget:target];
    if ([key length] > 0) {
        [item setKeyEquivalentModifierMask:modifiers];
    }
    [menu addItem:item];
    return item;
}

static NSMenu* AddSubmenu(NSMenu* parent, NSString* title) {
    NSMenuItem* holder = [[[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""] autorelease];
    NSMenu* menu = [[[NSMenu alloc] initWithTitle:title] autorelease];
    [holder setSubmenu:menu];
    [parent addItem:holder];
    return menu;
}

// Files a document link must never launch: programs, installers, scripts.
static BOOL IsRiskyLinkTarget(NSString* path) {
    static NSArray* risky = nil;
    if (!risky) {
        risky = [@[
            @"app",     @"command", @"tool",    @"terminal",    @"sh",       @"bash",   @"zsh",    @"csh",
            @"ksh",     @"py",      @"pl",      @"rb",          @"php",      @"jar",    @"pkg",    @"mpkg",
            @"dmg",     @"scpt",    @"scptd",   @"applescript", @"workflow", @"action", @"osax",   @"prefpane",
            @"fileloc", @"webloc",  @"inetloc", @"url",         @"kext",     @"plugin", @"bundle", @"service",
        ] retain];
    }
    if ([risky containsObject:[[path pathExtension] lowercaseString]]) {
        return YES;
    }
    BOOL isDir = NO;
    NSFileManager* fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:path isDirectory:&isDir] && isDir) {
        return [[NSWorkspace sharedWorkspace] isFilePackageAtPath:path];
    }
    return [fm isExecutableFileAtPath:path];
}

#pragma mark - Opening in the background

@class SumatraTabState;

// A document being opened on a loader thread (MacOpenDocumentAsync). Main
// thread only, except -passwordForFile:attempt:, which the loader calls.
@interface SumatraOpenRequest : NSObject
@property(nonatomic, copy) NSString* path;
@property(nonatomic, assign) SumatraTabState* tab; // nil once the tab is gone
@property(nonatomic, assign) id owner;             // the app delegate
@property(nonatomic, assign) SumatraSelfTest* selfTest;
@property(nonatomic) BOOL cancelled;
- (const char*)passwordForFile:(const char*)fileName attempt:(int)attempt;
@end

@implementation SumatraOpenRequest {
    char* _password; // last answer; the bridge copies it right away
    BOOL _showPassword;
}

- (void)dealloc {
    [self forgetPassword];
    [_path release];
    [super dealloc];
}

- (void)forgetPassword {
    if (_password) {
        memset(_password, 0, strlen(_password));
        free(_password);
        _password = nullptr;
    }
}

// Loader thread. The prompt runs on the main thread (app-modal), which never
// waits for a loader, so this can't deadlock; a closed tab isn't prompted for.
- (const char*)passwordForFile:(const char*)fileName attempt:(int)attempt {
    if (_selfTest) {
        return [_selfTest passwordForAttempt:attempt];
    }
    __block char* password = nullptr;
    __block bool show = _showPassword;
    dispatch_sync(dispatch_get_main_queue(), ^{
      if (_cancelled || !fileName) {
          return;
      }
      bool remember = false;
      int len = 0;
      MacGuiShowPasswordDialog(nullptr, fileName, (int)strlen(fileName), attempt > 1, false, false, show, &remember,
                               &show, &password, &len);
    });
    _showPassword = show;
    [self forgetPassword];
    _password = password;
    return _password;
}

@end

static const char* AskPassword(void* context, const char* fileName, int attempt) {
    return [(SumatraOpenRequest*)context passwordForFile:fileName attempt:attempt];
}

#pragma mark - Tab state

@interface SumatraTabState : NSObject
@property(nonatomic) void* document; // owned; closed with MacCloseDocument
@property(nonatomic, retain) SumatraOpenRequest* loadRequest; // set while the document is opening
@property(nonatomic) int requestedPage;                       // go here once opened (bookmarks)
@property(nonatomic, copy) NSString* path;
@property(nonatomic, copy) NSString* canonicalPath;
@property(nonatomic) int pageCount;
@property(nonatomic) int currentPage;
@property(nonatomic) int rotation;
@property(nonatomic) CGFloat zoom;
@property(nonatomic) BOOL continuous;
@property(nonatomic) NSPoint scrollOrigin;
@property(nonatomic) BOOL hasScrollOrigin;
@property(nonatomic) BOOL needsInitialScroll;
// FileState.ScrollPos of scrollPage (see -captureScrollState:)
@property(nonatomic) BOOL hasScrollState;
@property(nonatomic) int scrollPage;
@property(nonatomic) double scrollX;
@property(nonatomic) double scrollY;
@property(nonatomic) int findToken;
@property(nonatomic) int selectAllToken; // text being prepared for Select All
@property(nonatomic) int selectAllPercent;
@property(nonatomic) int historyIndex;
@property(nonatomic, retain) NSMutableArray* history; // NSNumber page numbers
@property(nonatomic, retain) NSDate* modificationDate;
@end

@implementation SumatraTabState

- (instancetype)init {
    self = [super init];
    if (self) {
        _history = [[NSMutableArray alloc] init];
        _zoom = kMacZoomFitPage;
        _continuous = YES;
        _currentPage = 1;
    }
    return self;
}

- (void)dealloc {
    [_loadRequest setCancelled:YES];
    [_loadRequest setTab:nil];
    [_loadRequest release];
    [_path release];
    [_canonicalPath release];
    [_history release];
    [_modificationDate release];
    [super dealloc];
}

@end

// A page image of the active document, kept while the page is (nearly) visible.
@interface SumatraCachedImage : NSObject
@property(nonatomic) CGImageRef image; // retained
@property(nonatomic) float renderZoom;
@property(nonatomic) int rotation;
@end

@implementation SumatraCachedImage

- (void)dealloc {
    if (_image) {
        CGImageRelease(_image);
    }
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

#pragma mark - Drop target

// Window content view; accepts files dropped anywhere on the window.
@interface SumatraDropView : NSView
@property(nonatomic, assign) id dropTarget; // receives -openPaths:
@end

@implementation SumatraDropView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    }
    return self;
}

- (NSArray*)droppedPaths:(id<NSDraggingInfo>)info {
    NSDictionary* options = @{NSPasteboardURLReadingFileURLsOnlyKey : [NSNumber numberWithBool:YES]};
    NSArray* urls = [[info draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ] options:options];
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* url in urls) {
        if ([url isFileURL] && [url path]) {
            [paths addObject:[url path]];
        }
    }
    return paths;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [[self droppedPaths:sender] count] > 0 ? NSDragOperationCopy : NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return [[self droppedPaths:sender] count] > 0 ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    return [[self droppedPaths:sender] count] > 0;
}

// Opening can show a password prompt, so it runs after the drag session ends.
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray* paths = [self droppedPaths:sender];
    if ([paths count] == 0 || !_dropTarget) {
        return NO;
    }
    [_dropTarget performSelector:@selector(openPaths:) withObject:paths afterDelay:0];
    return YES;
}

@end

#pragma mark - App delegate

@interface SumatraAppDelegate : NSObject <NSApplicationDelegate,
                                          NSWindowDelegate,
                                          NSToolbarDelegate,
                                          NSTextFieldDelegate,
                                          NSSplitViewDelegate,
                                          NSMenuDelegate,
                                          SumatraDocumentViewOwner,
                                          SumatraSidebarHost,
                                          SumatraSelfTestHost>
- (void)installMainMenu;
- (void)openPaths:(NSArray*)paths;
- (void)pageRenderReady;
- (void)findFinishedForDocument:(void*)document token:(int)token found:(BOOL)found;
- (void)openRequest:(SumatraOpenRequest*)request finished:(void*)document error:(MacOpenError)error;
- (void)textPreparedForDocument:(void*)document token:(int)token done:(int)done total:(int)total;
@end

typedef void (^SumatraAlertDone)(NSModalResponse response);

// Called by PageRenderService on the main queue; the service drops the call
// once its document is closed.
static void PageRenderReady(void* context) {
    SumatraAppDelegate* delegate = (SumatraAppDelegate*)context;
    if ([NSThread isMainThread]) {
        [delegate pageRenderReady];
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      [delegate pageRenderReady];
    });
}

// Called on the main queue; may arrive after the document was closed, so the
// delegate matches document and token against its tabs before using either.
static void FindDone(void* context, void* document, int token, bool found) {
    [(SumatraAppDelegate*)context findFinishedForDocument:document token:token found:found ? YES : NO];
}

// Main queue; may arrive after the document was closed (checked by token).
static void TextPrepared(void* context, void* document, int token, int done, int total) {
    [(SumatraAppDelegate*)context textPreparedForDocument:document token:token done:done total:total];
}

// Main thread; balances the retain taken when the open started.
static void OpenDone(void* context, void* document, MacOpenError error) {
    SumatraOpenRequest* request = (SumatraOpenRequest*)context;
    [(SumatraAppDelegate*)[request owner] openRequest:request finished:document error:error];
    [request release];
}

@implementation SumatraAppDelegate {
    NSWindow* _window;
    NSSplitView* _splitView;
    NSScrollView* _scrollView;
    SumatraDocumentView* _documentView;
    SumatraSidebar* _sidebar;
    NSToolbar* _toolbar;
    NSSegmentedControl* _tabSelector;
    NSTextField* _pageField;
    NSTextField* _pageCountLabel;
    NSSearchField* _searchField;
    NSProgressIndicator* _findSpinner;
    NSTextField* _findStatus;
    NSView* _documentPane; // find bar + scroll view
    NSView* _findBar;      // used when the toolbar's search field isn't on screen
    NSSearchField* _findBarField;
    NSProgressIndicator* _findBarSpinner;
    NSTextField* _findBarStatus;
    NSMenu* _recentMenu;
    NSMenu* _bookmarksMenu;
    NSMenu* _windowMenu;

    NSMutableArray* _tabs;
    SumatraTabState* _active; // element of _tabs, not retained separately
    NSMutableArray* _closedPaths;
    NSMutableArray* _pendingOpen;
    NSMutableArray* _alertQueue;
    NSMutableDictionary* _imageCache; // NSNumber page -> SumatraCachedImage, active tab only
    NSString* _findText;

    dispatch_source_t _fileWatcher;
    NSString* _watchedPath;
    id _keyMonitor;

    BOOL _launched;
    BOOL _alertShowing;
    BOOL _inLayout;
    BOOL _layoutAgain;
    BOOL _refreshScheduled;
    BOOL _liveMagnify;
    BOOL _programmaticScroll;
    BOOL _pinnedPage;
    BOOL _sidebarVisible;
    BOOL _adjustingSidebar;
    CGFloat _sidebarWidth;
    CGFloat _smartZoomReturn;
    double _currentLayoutZoom;
    double _lastWheelFlip;
    int _lastNotifiedPage;
    int _openDepth;
    void* _printingDocument;

    NSDictionary* _commandLine;
    NSString* _settingsPath;
    NSString* _tempPrefsDir; // -for-testing without -prefs-dir; removed on quit
    BOOL _testing;           // -for-testing / -self-test: no session, no user defaults
    SumatraSelfTest* _selfTest;
    BOOL _selfTestDone;
    int _selfTestExitCode;
    BOOL _visibleRendered;
    int _lastOpenError;

    NSMutableArray* _openQueue; // SumatraOpenRequest not started yet
    int _opensRunning;
}

#pragma mark - Launch and shutdown

// -prefs-dir, else a fresh temporary directory for -for-testing, else
// ~/Library/Application Support/SumatraPDF.
- (NSString*)chooseSettingsPath {
    NSString* dir = [_commandLine objectForKey:kArgPrefsDir];
    if ([dir length] > 0) {
        return [AbsolutePath(dir) stringByAppendingPathComponent:kSettingsFileName];
    }
    if (_testing) {
        NSString* name = [NSString stringWithFormat:@"SumatraPDF-testing-%d", (int)getpid()];
        dir = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
        [_tempPrefsDir release];
        _tempPrefsDir = [dir copy];
        return [dir stringByAppendingPathComponent:kSettingsFileName];
    }
    NSArray* supportDirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* supportDir = [supportDirs count] ? [supportDirs objectAtIndex:0] : NSTemporaryDirectory();
    return [[supportDir stringByAppendingPathComponent:@"SumatraPDF"] stringByAppendingPathComponent:kSettingsFileName];
}

// User defaults hold UI state (sidebar); test runs neither read nor write them.
- (void)saveDefault:(id)value forKey:(NSString*)key {
    if (!_testing) {
        [[NSUserDefaults standardUserDefaults] setObject:value forKey:key];
    }
}

- (void)applicationWillFinishLaunching:(NSNotification*)notification {
    (void)notification;
    _commandLine = [ParseCommandLine() retain];
    _testing = [_commandLine objectForKey:kArgForTesting] || [_commandLine objectForKey:kArgSelfTest];
    _settingsPath = [[self chooseSettingsPath] copy];
    MacPrefsInit([_settingsPath fileSystemRepresentation]);

    _tabs = [[NSMutableArray alloc] init];
    _closedPaths = [[NSMutableArray alloc] init];
    _pendingOpen = [[NSMutableArray alloc] init];
    _alertQueue = [[NSMutableArray alloc] init];
    _openQueue = [[NSMutableArray alloc] init];
    _imageCache = [[NSMutableDictionary alloc] init];

    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    [defaults registerDefaults:@{
        kDefSidebarVisible : [NSNumber numberWithBool:NO],
        kDefSidebarWidth : [NSNumber numberWithDouble:kSidebarDefaultWidth],
        kDefSidebarMode : [NSNumber numberWithInteger:SumatraSidebarModeOutline],
    }];
    _sidebarVisible = NO;
    _sidebarWidth = kSidebarDefaultWidth;
    if (!_testing) {
        _sidebarVisible = [defaults boolForKey:kDefSidebarVisible];
        _sidebarWidth = MAX(kSidebarMinWidth, MIN(kSidebarMaxWidth, (CGFloat)[defaults doubleForKey:kDefSidebarWidth]));
    }

    // tabs are our own (toolbar selector + Window menu), not NSWindow tabbing
    [NSWindow setAllowsAutomaticWindowTabbing:NO];
    [self createMainWindow];
    [self installKeyMonitor];
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    _launched = YES;
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    NSArray* argPaths = [_commandLine objectForKey:kArgPaths];
    NSString* report = [_commandLine objectForKey:kArgSelfTest];
    if (report) {
        double timeout = [[_commandLine objectForKey:kArgSelfTestTimeout] doubleValue];
        _selfTest = [[SumatraSelfTest alloc] initWithHost:self
                                               reportPath:AbsolutePath(report)
                                             manifestPath:[_commandLine objectForKey:kArgSelfTestManifest]
                                                    paths:argPaths
                                                 findWord:[_commandLine objectForKey:kArgSelfTestFind]
                                                  timeout:timeout > 0 ? timeout : kSelfTestTimeout];
        [self openExternalPaths:_pendingOpen];
        [_pendingOpen removeAllObjects];
        // from a timer: the self-test pumps the run loop, which can't drain
        // the main queue from inside a main-queue block
        [_selfTest performSelector:@selector(start) withObject:nil afterDelay:kSelfTestStartDelay];
        return;
    }

    NSMutableArray* paths = [NSMutableArray arrayWithArray:argPaths];
    [paths addObjectsFromArray:_pendingOpen];
    [_pendingOpen removeAllObjects];
    if ([paths count] > 0) {
        [self openPaths:paths];
    } else if (!_testing) {
        [self restoreSession];
    }
}

// Finder / `open` requests. The self-test opens its documents itself.
- (void)openExternalPaths:(NSArray*)paths {
    if (!_selfTest) {
        [self openPaths:paths];
        return;
    }
    for (NSString* path in paths) {
        [_selfTest recordIgnored:path];
    }
}

- (void)application:(NSApplication*)application openURLs:(NSArray*)urls {
    (void)application;
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* url in urls) {
        if ([url isFileURL] && [url path]) {
            [paths addObject:[url path]];
        }
    }
    [self openExternalPaths:paths];
}

- (void)application:(NSApplication*)sender openFiles:(NSArray*)filenames {
    [self openExternalPaths:filenames];
    [sender replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

- (BOOL)application:(NSApplication*)sender openFile:(NSString*)filename {
    (void)sender;
    [self openExternalPaths:@[ filename ]];
    return YES;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    (void)sender;
    if (!flag) {
        [_window makeKeyAndOrderFront:nil];
    }
    return YES;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [self stopWatching];
    if (_keyMonitor) {
        [NSEvent removeMonitor:_keyMonitor];
        [_keyMonitor release];
        _keyMonitor = nil;
    }

    NSInteger activeIndex = [self activeIndex];
    if (_active) {
        _active.scrollOrigin = [[_scrollView contentView] bounds].origin;
    }
    MacPrefsBeginSession();
    for (SumatraTabState* tab in _tabs) {
        if (!tab.document) {
            // still opening: keep its saved state
            MacPrefsAppendSession(FsPath(tab.path), nullptr);
            continue;
        }
        MacPrefsViewState state = [self prefsStateForTab:tab];
        MacPrefsAppendSession(FsPath(tab.path), &state);
    }
    MacPrefsFinishSession((int)MAX(activeIndex, (NSInteger)0));

    // loader threads still running are abandoned; their documents never arrive
    [_openQueue removeAllObjects];
    [self deactivateActiveTab];
    for (SumatraTabState* tab in _tabs) {
        [tab.loadRequest setCancelled:YES];
        [tab.loadRequest setTab:nil];
        [self saveTabState:tab];
        void* doc = tab.document;
        tab.document = nullptr;
        [_sidebar documentWillClose:doc];
        MacCloseDocument(doc);
    }
    [_tabs removeAllObjects];
    [_sidebar shutdown];
    MacPrefsShutdown();
    MacShutdown();
    if (_tempPrefsDir) {
        [[NSFileManager defaultManager] removeItemAtPath:_tempPrefsDir error:nil];
    }
    if (_selfTestDone) {
        fflush(stdout);
        fflush(stderr);
        exit(_selfTestExitCode);
    }
}

- (void)restoreSession {
    int count = MacPrefsSessionCount();
    int activeIndex = MacPrefsSessionActiveTab();
    NSString* activePath = nil;
    NSMutableArray* paths = [NSMutableArray array];
    for (int i = 0; i < count; i++) {
        MacPrefsViewState state = {};
        char* pathFs = MacPrefsCopySessionTab(i, &state);
        NSString* path = StringFromFs(pathFs);
        MacFreeString(pathFs);
        // silently skip documents that were moved or deleted since
        if (!path || ![[NSFileManager defaultManager] fileExistsAtPath:path]) {
            continue;
        }
        if (i == activeIndex) {
            activePath = path;
        }
        [paths addObject:path];
    }
    [self openPaths:paths];
    int idx = activePath ? [self tabIndexForPath:activePath] : -1;
    if (idx >= 0) {
        [self activateTabAtIndex:idx];
    }
}

- (void)createMainWindow {
    NSRect frame = NSMakeRect(0, 0, 1000, 820);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                       NSWindowStyleMaskResizable;
    _window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
    [_window setReleasedWhenClosed:NO];
    [_window setTitle:@"SumatraPDF"];
    [_window setDelegate:self];
    [_window setMinSize:NSMakeSize(480, 360)];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window setAcceptsMouseMovedEvents:YES];

    SumatraDropView* content = [[[SumatraDropView alloc] initWithFrame:frame] autorelease];
    [content setDropTarget:self];
    [_window setContentView:content];
    NSRect bounds = [content bounds];

    _splitView = [[NSSplitView alloc] initWithFrame:bounds];
    [_splitView setVertical:YES];
    [_splitView setDividerStyle:NSSplitViewDividerStyleThin];
    [_splitView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_splitView setDelegate:self];

    _sidebar = [[SumatraSidebar alloc] initWithHost:self];
    NSView* sidebarView = [_sidebar view];
    [sidebarView setFrame:NSMakeRect(0, 0, _sidebarWidth, bounds.size.height)];
    NSInteger mode = _testing ? SumatraSidebarModeOutline
                              : [[NSUserDefaults standardUserDefaults] integerForKey:kDefSidebarMode];
    [_sidebar setMode:mode == SumatraSidebarModeThumbnails ? SumatraSidebarModeThumbnails : SumatraSidebarModeOutline];

    NSRect scrollFrame =
        NSMakeRect(0, 0, MAX(kDocumentMinWidth, bounds.size.width - _sidebarWidth), bounds.size.height);
    _scrollView = [[NSScrollView alloc] initWithFrame:scrollFrame];
    [_scrollView setHasVerticalScroller:YES];
    [_scrollView setHasHorizontalScroller:YES];
    [_scrollView setAutohidesScrollers:YES];
    [_scrollView setBorderType:NSNoBorder];
    [_scrollView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_scrollView setDrawsBackground:YES];
    [_scrollView setBackgroundColor:[NSColor colorWithCalibratedWhite:0.18 alpha:1.0]];

    _documentView = [[SumatraDocumentView alloc] initWithFrame:[[_scrollView contentView] bounds]];
    [_documentView setOwner:self];
    [_scrollView setDocumentView:_documentView];

    NSClipView* clip = [_scrollView contentView];
    [clip setPostsBoundsChangedNotifications:YES];
    [clip setPostsFrameChangedNotifications:YES];
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserver:self selector:@selector(clipBoundsChanged:) name:NSViewBoundsDidChangeNotification object:clip];
    [center addObserver:self selector:@selector(clipFrameChanged:) name:NSViewFrameDidChangeNotification object:clip];

    // document pane: the find bar (hidden) above the scroll view
    _documentPane = [[NSView alloc] initWithFrame:scrollFrame];
    [_documentPane setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_documentPane addSubview:_scrollView];
    [self createFindBar];

    [_splitView addSubview:sidebarView];
    [_splitView addSubview:_documentPane];
    [content addSubview:_splitView];
    [self setSidebarVisible:_sidebarVisible];

    [self installToolbar];
    [_window center];
    if (_testing) {
        [_window setRestorable:NO];
    } else {
        [_window setFrameAutosaveName:@"SumatraPDFMainWindow"];
    }
    [self showEmptyState];
    [_window makeFirstResponder:_documentView];
}

// Keys that menus can't bind reliably: ⌃⇥ / ⌃⇧⇥ (tab switching, otherwise
// eaten by the key view loop) and ⌘= (zoom in without shift).
- (void)installKeyMonitor {
    _keyMonitor = [[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                         handler:^NSEvent*(NSEvent* event) {
                                                           return [self handleMonitoredKey:event];
                                                         }] retain];
}

- (NSEvent*)handleMonitoredKey:(NSEvent*)event {
    if ([event window] != _window) {
        return event;
    }
    NSEventModifierFlags mods = [event modifierFlags] & (NSEventModifierFlagShift | NSEventModifierFlagControl |
                                                         NSEventModifierFlagOption | NSEventModifierFlagCommand);
    const unsigned short kTabKeyCode = 48;
    if ([event keyCode] == kTabKeyCode && (mods & NSEventModifierFlagControl) &&
        !(mods & (NSEventModifierFlagCommand | NSEventModifierFlagOption))) {
        [self selectRelativeTab:(mods & NSEventModifierFlagShift) ? -1 : 1];
        return nil;
    }
    if (mods == NSEventModifierFlagCommand && [[event charactersIgnoringModifiers] isEqualToString:@"="] && _active) {
        [self zoomIn:nil];
        return nil;
    }
    return event;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [NSObject cancelPreviousPerformRequestsWithTarget:self];
    [self stopWatching];
    [_window setDelegate:nil];
    [_toolbar setDelegate:nil];
    [_splitView setDelegate:nil];
    [_documentView setOwner:nil];
    [_window release];
    [_splitView release];
    [_scrollView release];
    [_documentView release];
    [_sidebar release];
    [_toolbar release];
    [_tabSelector release];
    [_pageField release];
    [_pageCountLabel release];
    [_searchField release];
    [_findSpinner release];
    [_findStatus release];
    [_documentPane release];
    [_findBar release];
    [_findBarField release];
    [_findBarSpinner release];
    [_findBarStatus release];
    [_tabs release];
    [_closedPaths release];
    [_pendingOpen release];
    [_alertQueue release];
    [_openQueue release];
    [_imageCache release];
    [_findText release];
    [_commandLine release];
    [_settingsPath release];
    [_tempPrefsDir release];
    [_selfTest release];
    [super dealloc];
}

#pragma mark - Alerts

// Sheets on the main window, one at a time; alerts arriving meanwhile queue up.
- (void)presentAlert:(NSAlert*)alert completion:(SumatraAlertDone)done {
    // the self-test can't click: record the text and answer Cancel (or OK)
    if (_selfTest) {
        [_selfTest recordAlert:[alert messageText] info:[alert informativeText]];
        if (done) {
            done([[alert buttons] count] > 1 ? NSAlertSecondButtonReturn : NSAlertFirstButtonReturn);
        }
        return;
    }
    SumatraAlertDone copied = nil;
    if (done) {
        copied = [[done copy] autorelease];
    }
    if (![_window isVisible]) {
        NSModalResponse response = [alert runModal];
        if (copied) {
            copied(response);
        }
        return;
    }
    if (_alertShowing || [_window attachedSheet]) {
        [_alertQueue addObject:@[ alert, copied ? (id)copied : (id)[NSNull null] ]];
        return;
    }
    _alertShowing = YES;
    [alert beginSheetModalForWindow:_window
                  completionHandler:^(NSModalResponse response) {
                    _alertShowing = NO;
                    if (copied) {
                        copied(response);
                    }
                    dispatch_async(dispatch_get_main_queue(), ^{
                      [self presentNextAlert];
                    });
                  }];
}

- (void)presentNextAlert {
    if (_alertShowing || [_alertQueue count] == 0) {
        return;
    }
    NSArray* entry = [[[_alertQueue objectAtIndex:0] retain] autorelease];
    [_alertQueue removeObjectAtIndex:0];
    id stored = [entry objectAtIndex:1];
    SumatraAlertDone done = nil;
    if (stored != [NSNull null]) {
        done = (SumatraAlertDone)stored;
    }
    [self presentAlert:[entry objectAtIndex:0] completion:done];
}

- (void)showAlertWithMessage:(NSString*)message info:(NSString*)info style:(NSAlertStyle)style {
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setAlertStyle:style];
    [alert setMessageText:message];
    if ([info length] > 0) {
        [alert setInformativeText:info];
    }
    [self presentAlert:alert completion:nil];
}

- (void)reportOpenError:(MacOpenError)err path:(NSString*)path {
    _lastOpenError = (int)err;
    NSString* name = [[NSFileManager defaultManager] displayNameAtPath:path];
    if ([name length] == 0) {
        name = [path lastPathComponent];
    }
    NSString* message = nil;
    NSString* info = nil;
    NSAlertStyle style = NSAlertStyleWarning;
    switch (err) {
        case MacOpenError::NotFound:
            message = [NSString stringWithFormat:@"“%@” could not be found.", name];
            info = [path stringByAbbreviatingWithTildeInPath];
            break;
        case MacOpenError::Unreadable:
            message = [NSString stringWithFormat:@"“%@” could not be read.", name];
            info = @"Check that you have permission to read the file.";
            break;
        case MacOpenError::Unsupported: {
            char* formats = MacCopySupportedFormats();
            NSString* list = StringFromUtf8(formats);
            MacFreeString(formats);
            message = [NSString stringWithFormat:@"“%@” is not a document SumatraPDF can open.", name];
            info = list ? [NSString stringWithFormat:@"Supported formats: %@.", list] : nil;
            break;
        }
        case MacOpenError::PasswordCancelled:
            message = [NSString stringWithFormat:@"“%@” was not opened.", name];
            info = @"The document is protected by a password and no valid password was entered.";
            style = NSAlertStyleInformational;
            break;
        case MacOpenError::RendererFailed:
            message = [NSString stringWithFormat:@"“%@” could not be displayed.", name];
            info = @"The page renderer could not be started.";
            break;
        case MacOpenError::Damaged:
        case MacOpenError::None:
            message = [NSString stringWithFormat:@"“%@” could not be opened.", name];
            info = @"The file may be damaged, or it is not a valid document of its type.";
            break;
    }
    if (!_active) {
        [_documentView setMessage:message];
    }
    [self showAlertWithMessage:message info:info style:style];
}

#pragma mark - Opening documents

- (void)openPaths:(NSArray*)paths {
    if (!_launched) {
        [_pendingOpen addObjectsFromArray:paths];
        return;
    }
    NSMutableSet* seen = [NSMutableSet set];
    for (NSString* path in paths) {
        if (![path isKindOfClass:[NSString class]] || [path length] == 0) {
            continue;
        }
        NSString* canonical = CanonicalPath(ResolveDocumentPath(path));
        if ([seen containsObject:canonical]) {
            continue;
        }
        [seen addObject:canonical];
        [self openPath:path];
    }
}

- (int)tabIndexForPath:(NSString*)path {
    NSString* canonical = CanonicalPath(path);
    for (NSUInteger i = 0; i < [_tabs count]; i++) {
        SumatraTabState* tab = [_tabs objectAtIndex:i];
        if ([tab.canonicalPath isEqualToString:canonical]) {
            return (int)i;
        }
    }
    return -1;
}

// Adds a tab at once and opens the document on a loader thread (at most
// kMaxConcurrentOpens at a time); -completeOpen: fills the tab in.
- (BOOL)openPath:(NSString*)path {
    path = ResolveDocumentPath(path);
    if ([path length] == 0) {
        return NO;
    }
    int existing = [self tabIndexForPath:path];
    if (existing >= 0) {
        [self activateTabAtIndex:existing];
        return YES;
    }

    SumatraTabState* tab = [[[SumatraTabState alloc] init] autorelease];
    tab.path = path;
    tab.canonicalPath = CanonicalPath(path);
    tab.needsInitialScroll = YES;
    SumatraOpenRequest* request = [[[SumatraOpenRequest alloc] init] autorelease];
    request.path = path;
    request.tab = tab;
    request.owner = self;
    request.selfTest = _selfTest;
    tab.loadRequest = request;
    [_openQueue addObject:request];
    [_tabs addObject:tab];
    [self activateTabAtIndex:(int)[_tabs count] - 1];
    [_window makeKeyAndOrderFront:nil];
    [self startQueuedOpens];
    return YES;
}

- (void)startQueuedOpens {
    while (_opensRunning < kMaxConcurrentOpens && [_openQueue count] > 0) {
        SumatraOpenRequest* request = [[[_openQueue objectAtIndex:0] retain] autorelease];
        [_openQueue removeObjectAtIndex:0];
        if (request.cancelled) {
            continue;
        }
        [request retain]; // released by OpenDone()
        if (MacOpenDocumentAsync(FsPath(request.path), AskPassword, request, PageRenderReady, self, OpenDone, request)) {
            _opensRunning++;
            continue;
        }
        [request release];
        // no loader thread: open here, with app-modal password prompts
        MacOpenError err = MacOpenError::None;
        _openDepth++;
        void* doc = MacOpenDocumentEx((void*)_window, FsPath(request.path), PageRenderReady, self, &err);
        _openDepth--;
        [self completeOpen:request document:doc error:err];
    }
}

- (void)openRequest:(SumatraOpenRequest*)request finished:(void*)document error:(MacOpenError)error {
    _opensRunning--;
    [self completeOpen:request document:document error:error];
    [self startQueuedOpens];
}

// The tab gets its document, or goes away with an error; a document whose tab
// was closed meanwhile is closed.
- (void)completeOpen:(SumatraOpenRequest*)request document:(void*)doc error:(MacOpenError)err {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(showLoadingIndicator:) object:request];
    SumatraTabState* tab = request.tab;
    if (request.cancelled || !tab || tab.loadRequest != request) {
        if (doc) {
            [_sidebar documentWillClose:doc];
            MacCloseDocument(doc);
        }
        return;
    }
    tab.loadRequest = nil;
    if (!doc) {
        NSUInteger idx = [_tabs indexOfObjectIdenticalTo:tab];
        NSString* path = [[tab.path retain] autorelease];
        if (idx != NSNotFound) {
            [self removeTabAtIndex:(int)idx rememberClosed:NO];
        }
        [self reportOpenError:err path:path];
        return;
    }

    tab.document = doc;
    tab.pageCount = MacPageCount(doc);
    tab.modificationDate = ModificationDate(tab.path);
    [self applySavedState:tab];
    if (tab.requestedPage > 0) {
        tab.currentPage = MAX(1, MIN(tab.pageCount, tab.requestedPage));
        tab.hasScrollState = NO;
        tab.requestedPage = 0;
    }
    if (tab != _active) {
        [self refreshTabSelector];
        return;
    }
    [_documentView setBusy:NO];
    [_documentView setMessage:nil];
    [_sidebar documentChanged];
    [self restoreViewForActiveTab];
    [self startWatchingActiveTab];
    [self refreshTabSelector];
    [self updateWindowTitle];
    [self updatePageControls];
}

- (void)applySavedState:(SumatraTabState*)tab {
    MacPrefsViewState state = {};
    if (!MacPrefsOpenDocument(FsPath(tab.path), &state) || !state.valid) {
        return;
    }
    tab.continuous = state.continuous ? YES : NO;
    tab.rotation = ((state.rotation % 360) + 360) % 360 / 90 * 90;
    tab.currentPage = MAX(1, MIN(tab.pageCount, state.pageNo));
    // like the Windows app, a page that no longer exists loses the position
    BOOL samePage = state.pageNo == tab.currentPage;
    tab.hasScrollState = YES;
    tab.scrollPage = tab.currentPage;
    tab.scrollX = samePage ? state.scrollX : -1;
    tab.scrollY = samePage ? state.scrollY : -1;
    if (state.zoomVirtual > 0) {
        tab.zoom = MAX(kZoomMin, MIN(kZoomMax, (CGFloat)(state.zoomVirtual / 100.0)));
    } else {
        tab.zoom = state.zoomVirtual == kMacZoomFitWidth ? kMacZoomFitWidth : kMacZoomFitPage;
    }
}

// Opening takes a while: say so in the document area.
- (void)showLoadingIndicator:(SumatraOpenRequest*)request {
    SumatraTabState* tab = request.tab;
    if (request.cancelled || !tab || tab != _active || tab.document) {
        return;
    }
    NSString* name = [[NSFileManager defaultManager] displayNameAtPath:tab.path];
    [_documentView setMessage:[NSString stringWithFormat:@"Opening “%@”…", name]];
    [_documentView setBusy:YES];
}

- (MacPrefsViewState)prefsStateForTab:(SumatraTabState*)tab {
    [self captureScrollState:tab];
    MacPrefsViewState state = {};
    state.valid = true;
    state.continuous = tab.continuous;
    state.zoomVirtual = tab.zoom > 0 ? tab.zoom * 100.0 : tab.zoom;
    state.rotation = tab.rotation;
    state.pageNo = tab.currentPage;
    state.scrollX = -1;
    state.scrollY = -1;
    if (tab.hasScrollState) {
        state.pageNo = tab.scrollPage;
        state.scrollX = tab.scrollX;
        state.scrollY = tab.scrollY;
    }
    return state;
}

- (void)saveTabState:(SumatraTabState*)tab {
    if (!tab.path || !tab.document) {
        return;
    }
    MacPrefsViewState state = [self prefsStateForTab:tab];
    MacPrefsSaveDocument(FsPath(tab.path), &state);
}

#pragma mark - Tabs

// The active tab once its document has opened.
- (SumatraTabState*)loadedTab {
    return _active.document ? _active : nil;
}

- (NSInteger)activeIndex {
    if (!_active) {
        return -1;
    }
    NSUInteger idx = [_tabs indexOfObjectIdenticalTo:_active];
    return idx == NSNotFound ? -1 : (NSInteger)idx;
}

- (void)refreshTabSelector {
    NSInteger count = (NSInteger)[_tabs count];
    [_tabSelector setSegmentCount:count];
    for (NSInteger i = 0; i < count; i++) {
        SumatraTabState* tab = [_tabs objectAtIndex:(NSUInteger)i];
        [_tabSelector setLabel:ShortTabLabel([tab.path lastPathComponent]) forSegment:i];
        [_tabSelector setImage:tab.loadRequest ? ToolbarImage(@"hourglass", @"Opening", @"…") : nil forSegment:i];
        NSString* tip = tab.loadRequest ? [NSString stringWithFormat:@"%@ (opening…)", tab.path] : tab.path;
        [_tabSelector setToolTip:tip forSegment:i];
    }
    NSInteger active = [self activeIndex];
    if (active >= 0) {
        [_tabSelector setSelectedSegment:active];
    }
    [_tabSelector setEnabled:count > 0];
}

// Frees the render cache of the tab being left: only the active document keeps
// rendered pages, so memory stays bounded with many tabs.
- (void)deactivateActiveTab {
    SumatraTabState* tab = _active;
    if (!tab) {
        return;
    }
    [self captureScrollState:tab];
    tab.scrollOrigin = [[_scrollView contentView] bounds].origin;
    tab.hasScrollOrigin = YES;
    tab.findToken = 0;
    [self stopSelectAll:tab];
    [self stopWatching];
    MacFindCancel(tab.document);
    MacCancelPendingRenders(tab.document);
    MacResetRenderer(tab.document);
    _active = nil;
    [_imageCache removeAllObjects];
    [self setFindBusy:NO];
    [self showFindStatus:nil];
}

- (void)activateTabAtIndex:(int)index {
    if (index < 0 || index >= (int)[_tabs count]) {
        return;
    }
    SumatraTabState* tab = [_tabs objectAtIndex:(NSUInteger)index];
    if (tab == _active) {
        [self refreshTabSelector];
        return;
    }
    [self deactivateActiveTab];
    _active = tab;
    _pinnedPage = NO;
    _smartZoomReturn = 0;
    _lastNotifiedPage = 0;
    [_documentView setMessage:nil];

    if (tab.loadRequest) {
        [self performSelector:@selector(showLoadingIndicator:)
                   withObject:tab.loadRequest
                   afterDelay:kLoadingIndicatorDelay];
    }

    // documents in background tabs aren't watched; catch up on activation
    NSDate* date = ModificationDate(tab.path);
    if (tab.document && tab.modificationDate && date && ![date isEqualToDate:tab.modificationDate]) {
        [self reloadTab:tab];
        if (_active != tab) {
            return;
        }
    }

    [_sidebar documentChanged];
    [self restoreViewForActiveTab];
    [self startWatchingActiveTab];
    [self refreshTabSelector];
    [self updateWindowTitle];
    [self updatePageControls];
    [_window makeFirstResponder:_documentView];
}

- (void)restoreViewForActiveTab {
    SumatraTabState* tab = _active;
    [self updateLayout];
    if (!tab.document) {
        return;
    }
    if (tab.needsInitialScroll || !tab.hasScrollOrigin) {
        tab.needsInitialScroll = NO;
        if (tab.hasScrollState) {
            [self restoreScrollState];
        } else {
            [self showPage:tab.currentPage position:PagePosition::Top];
        }
        return;
    }
    [self scrollToOrigin:tab.scrollOrigin];
    [self updateLayout];
}

- (void)closeTabAtIndex:(int)index {
    [self removeTabAtIndex:index rememberClosed:YES];
}

// Closes a tab; one still opening stops waiting for its document.
- (void)removeTabAtIndex:(int)index rememberClosed:(BOOL)remember {
    if (index < 0 || index >= (int)[_tabs count]) {
        return;
    }
    SumatraTabState* tab = [[[_tabs objectAtIndex:(NSUInteger)index] retain] autorelease];
    if (tab.document && tab.document == _printingDocument) {
        NSBeep();
        return;
    }
    SumatraOpenRequest* request = tab.loadRequest;
    if (request) {
        request.cancelled = YES;
        request.tab = nil;
        [_openQueue removeObjectIdenticalTo:request];
        tab.loadRequest = nil;
    }
    BOOL wasActive = tab == _active;
    if (wasActive) {
        [self deactivateActiveTab];
    }
    [self saveTabState:tab];
    if (remember && tab.path) {
        [_closedPaths removeObject:tab.path];
        [_closedPaths addObject:tab.path];
        while ([_closedPaths count] > kMaxClosedTabs) {
            [_closedPaths removeObjectAtIndex:0];
        }
    }
    [_tabs removeObjectAtIndex:(NSUInteger)index];
    void* doc = tab.document;
    tab.document = nullptr;
    [_sidebar documentWillClose:doc];
    MacCloseDocument(doc);

    if (!wasActive) {
        [self refreshTabSelector];
        return;
    }
    if ([_tabs count] > 0) {
        [self activateTabAtIndex:MIN(index, (int)[_tabs count] - 1)];
        return;
    }
    [_documentView setMessage:nil];
    [_sidebar documentChanged];
    [self refreshTabSelector];
    [self showEmptyState];
}

- (void)selectRelativeTab:(int)direction {
    int count = (int)[_tabs count];
    if (count < 2) {
        return;
    }
    int current = (int)MAX([self activeIndex], (NSInteger)0);
    [self activateTabAtIndex:(current + direction + count) % count];
}

- (void)showEmptyState {
    [_documentView setBusy:NO];
    [_documentView setPages:nil];
    [_documentView setFrameSize:[[_scrollView contentView] bounds].size];
    [self updateWindowTitle];
    [self updatePageControls];
    [self refreshTabSelector];
}

- (void)updateWindowTitle {
    NSString* path = _active.path;
    if (!path) {
        [_window setTitle:@"SumatraPDF"];
        [_window setRepresentedURL:nil];
        return;
    }
    [_window setRepresentedURL:[NSURL fileURLWithPath:path]];
    NSString* name = [[NSFileManager defaultManager] displayNameAtPath:path];
    [_window setTitle:[name length] ? name : [path lastPathComponent]];
}

- (void)updatePageControls {
    SumatraTabState* tab = _active;
    BOOL has = tab.document != nullptr;
    if ([_pageField currentEditor] == nil) {
        [_pageField setStringValue:has ? [NSString stringWithFormat:@"%d", tab.currentPage] : @""];
    }
    [_pageField setEnabled:has];
    [_pageCountLabel setStringValue:has ? [NSString stringWithFormat:@"of %d", tab.pageCount] : @""];
    [_searchField setEnabled:has];
    [_findBarField setEnabled:has];
    NSString* subtitle = has ? [NSString stringWithFormat:@"Page %d of %d", tab.currentPage, tab.pageCount] : @"";
    if (has && tab.selectAllToken != 0) {
        subtitle = [NSString stringWithFormat:@"Selecting text… %d%%", tab.selectAllPercent];
    }
    if ([_window respondsToSelector:@selector(setSubtitle:)]) {
        [_window performSelector:@selector(setSubtitle:) withObject:subtitle];
    }
    [_toolbar validateVisibleItems];
}

- (void)pageStateChanged {
    [self updatePageControls];
    int page = _active ? _active.currentPage : 0;
    if (page == _lastNotifiedPage) {
        return;
    }
    _lastNotifiedPage = page;
    if (page > 0) {
        [_sidebar currentPageChanged:page];
    }
    NSAccessibilityPostNotification(_documentView, NSAccessibilityValueChangedNotification);
}

#pragma mark - File watching

- (void)stopWatching {
    if (_fileWatcher) {
        dispatch_source_cancel(_fileWatcher);
        dispatch_release(_fileWatcher);
        _fileWatcher = nullptr;
    }
    [_watchedPath release];
    _watchedPath = nil;
}

// Reloads the active document after writes or an atomic replace (e.g. a
// LaTeX build), debounced so a half-written file isn't opened.
- (void)startWatchingActiveTab {
    [self stopWatching];
    NSString* path = _active.path;
    if (!path || !_active.document) {
        return;
    }
    int fd = open(FsPath(path), O_EVTONLY);
    if (fd < 0) {
        return;
    }
    unsigned long mask = DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME;
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, (uintptr_t)fd, mask, dispatch_get_main_queue());
    if (!source) {
        close(fd);
        return;
    }
    NSString* watched = [[path copy] autorelease];
    dispatch_source_set_event_handler(source, ^{
      [self fileChangedOnDisk:watched];
    });
    dispatch_source_set_cancel_handler(source, ^{
      close(fd);
    });
    dispatch_resume(source);
    _fileWatcher = source;
    _watchedPath = [watched retain];
}

- (void)fileChangedOnDisk:(NSString*)path {
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(reloadIfChanged:) object:path];
    [self performSelector:@selector(reloadIfChanged:) withObject:path afterDelay:kReloadDelay];
}

- (void)reloadIfChanged:(NSString*)path {
    SumatraTabState* tab = [self loadedTab];
    if (!tab || ![tab.path isEqualToString:path]) {
        return;
    }
    // never replace a document under a modal loop (password prompt, printing)
    if (_openDepth > 0 || [NSApp modalWindow] || (_printingDocument && tab.document == _printingDocument)) {
        [self performSelector:@selector(reloadIfChanged:) withObject:path afterDelay:1.0];
        return;
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        return;
    }
    [self reloadTab:tab];
}

// Replaces the tab's document with a fresh copy; keeps the old one if the new
// file can't be opened (e.g. still being written).
- (void)reloadTab:(SumatraTabState*)tab {
    MacOpenError err = MacOpenError::None;
    _openDepth++;
    void* doc = MacOpenDocumentEx((void*)_window, FsPath(tab.path), PageRenderReady, self, &err);
    _openDepth--;
    BOOL isActive = tab == _active;
    if (!doc) {
        if (isActive) {
            [self startWatchingActiveTab];
        }
        return;
    }
    NSPoint origin = [[_scrollView contentView] bounds].origin;
    void* old = tab.document;
    MacFindCancel(old);
    [_sidebar documentWillClose:old];
    MacCloseDocument(old);
    tab.document = doc;
    tab.pageCount = MacPageCount(doc);
    tab.currentPage = MAX(1, MIN(tab.pageCount, tab.currentPage));
    tab.modificationDate = ModificationDate(tab.path);
    tab.findToken = 0;
    tab.selectAllToken = 0;
    [tab.history removeAllObjects];
    tab.historyIndex = 0;
    if (!isActive) {
        return;
    }
    [_imageCache removeAllObjects];
    [self setFindBusy:NO];
    [_sidebar documentChanged];
    [self updateLayout];
    [self scrollToOrigin:origin];
    [self updateLayout];
    [self startWatchingActiveTab];
}

#pragma mark - Layout and rendering

- (CGFloat)backingScale {
    CGFloat scale = [_window backingScaleFactor];
    return scale > 0 ? scale : 1.0;
}

- (BOOL)buildLayout:(MacDocumentLayout*)layout {
    SumatraTabState* tab = _active;
    if (!tab || !tab.document) {
        return NO;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    MacLayoutParams params = {};
    params.continuous = tab.continuous;
    params.startPage = MAX(1, tab.currentPage);
    params.viewX = (int)floor(visible.origin.x);
    params.viewY = (int)floor(visible.origin.y);
    params.viewWidth = (int)MAX(1.0, floor(visible.size.width));
    params.viewHeight = (int)MAX(1.0, floor(visible.size.height));
    params.zoomVirtual = tab.zoom > 0 ? tab.zoom * 100.0 : tab.zoom;
    params.backingScale = [self backingScale];
    params.rotation = tab.rotation;
    return MacLayoutDocument(tab.document, &params, layout);
}

- (NSRect)frameOfPage:(int)pageNo zoom:(double*)zoomOut {
    MacDocumentLayout layout = {};
    NSRect frame = NSZeroRect;
    if (![self buildLayout:&layout]) {
        return frame;
    }
    if (pageNo >= 1 && pageNo <= layout.pageCount && layout.pages[pageNo - 1].shown) {
        MacLayoutPage* p = &layout.pages[pageNo - 1];
        frame = NSMakeRect(p->x, p->y, p->width, p->height);
        if (zoomOut) {
            *zoomOut = p->layoutZoom;
        }
    }
    MacFreeDocumentLayout(&layout);
    return frame;
}

// The exact render if available (copied once from the render service into
// _imageCache), else a request plus the best stale image as placeholder.
- (CGImageRef)imageForPage:(int)pageNo renderZoom:(float)renderZoom request:(BOOL)request exact:(BOOL*)exact {
    SumatraTabState* tab = _active;
    NSNumber* key = [NSNumber numberWithInt:pageNo];
    SumatraCachedImage* cached = [_imageCache objectForKey:key];
    int rotation = tab.rotation;
    *exact = YES;
    if (cached && cached.renderZoom == renderZoom && cached.rotation == rotation) {
        return cached.image;
    }
    MacRenderedPage page = {};
    if (MacCopyRenderedPage(tab.document, pageNo, renderZoom, rotation, &page)) {
        CGImageRef image = SumatraCreateImage(&page);
        MacFreeRenderedPage(&page);
        if (image) {
            SumatraCachedImage* entry = [[[SumatraCachedImage alloc] init] autorelease];
            entry.image = image;
            entry.renderZoom = renderZoom;
            entry.rotation = rotation;
            CGImageRelease(image);
            [_imageCache setObject:entry forKey:key];
            return entry.image;
        }
    }
    *exact = NO;
    if (request) {
        MacRequestPage(tab.document, pageNo, renderZoom, rotation, 0);
    }
    if (cached && cached.rotation == rotation) {
        return cached.image;
    }
    return nullptr;
}

- (void)pruneImageCacheFrom:(int)first to:(int)last {
    for (NSNumber* key in [_imageCache allKeys]) {
        int pageNo = [key intValue];
        if (pageNo < first || pageNo > last) {
            [_imageCache removeObjectForKey:key];
        }
    }
}

- (NSArray*)selectionRectsForPage:(const MacLayoutPage*)lp {
    MacDisplayRect* rects = nullptr;
    int count = MacCopySelectionRects(_active.document, lp->pageNo, lp->layoutZoom, _active.rotation, &rects);
    NSMutableArray* result = count > 0 ? [NSMutableArray arrayWithCapacity:(NSUInteger)count] : nil;
    for (int i = 0; i < count; i++) {
        MacDisplayRect r = rects[i];
        [result addObject:[NSValue valueWithRect:NSMakeRect(lp->x + r.x, lp->y + r.y, r.width, r.height)]];
    }
    free(rects);
    return result;
}

- (NSArray*)highlightRects:(Highlight)kind forPage:(const MacLayoutPage*)lp {
    if (kind == Highlight::Selection) {
        return [self selectionRectsForPage:lp];
    }
    void* doc = _active.document;
    int rotation = _active.rotation;
    int count =
        kind == Highlight::Find ? MacFindResultRectCount(doc, lp->pageNo) : MacSelectionRectCount(doc, lp->pageNo);
    if (count <= 0) {
        return nil;
    }
    NSMutableArray* rects = [NSMutableArray arrayWithCapacity:(NSUInteger)count];
    for (int i = 0; i < count; i++) {
        MacDisplayRect r = {};
        bool ok = kind == Highlight::Find ? MacFindResultRect(doc, lp->pageNo, i, lp->layoutZoom, rotation, &r)
                                          : MacSelectionRect(doc, lp->pageNo, i, lp->layoutZoom, rotation, &r);
        if (ok) {
            [rects addObject:[NSValue valueWithRect:NSMakeRect(lp->x + r.x, lp->y + r.y, r.width, r.height)]];
        }
    }
    return rects;
}

// Layout passes re-enter through scroll notifications (setting the canvas size
// can move the clip view); those are folded into at most a few repeats.
- (void)updateLayout {
    if (_inLayout) {
        _layoutAgain = YES;
        return;
    }
    _inLayout = YES;
    for (int i = 0; i < 3; i++) {
        _layoutAgain = NO;
        [self layoutOnce];
        if (!_layoutAgain) {
            break;
        }
    }
    _inLayout = NO;
}

- (void)layoutOnce {
    SumatraTabState* tab = _active;
    _visibleRendered = NO;
    if (tab && !tab.document) {
        // still opening (-showLoadingIndicator: may add a spinner)
        [_documentView setPages:nil];
        [_documentView setFrameSize:[[_scrollView contentView] bounds].size];
        [self updatePageControls];
        return;
    }
    if (!tab) {
        [self showEmptyState];
        return;
    }
    MacDocumentLayout layout = {};
    if (![self buildLayout:&layout]) {
        [_documentView setPages:nil];
        [_documentView setMessage:@"The document could not be laid out."];
        return;
    }
    // chaptered ebooks can gain pages after opening; the bridge then sends a
    // page-ready callback, which lays out again
    if (layout.pageCount != tab.pageCount) {
        tab.pageCount = layout.pageCount;
        [_sidebar documentChanged];
    }
    NSSize canvas = NSMakeSize(layout.canvasWidth, layout.canvasHeight);
    if (!NSEqualSizes([_documentView frame].size, canvas)) {
        [_documentView setFrameSize:canvas];
    }

    // pages scrolled out of view lose their queued renders
    BOOL request = !_liveMagnify;
    if (request) {
        MacCancelPendingRenders(tab.document);
    }
    NSMutableArray* pages = [NSMutableArray array];
    int first = 0;
    int last = 0;
    BOOL allExact = request;
    for (int i = 0; i < layout.pageCount; i++) {
        MacLayoutPage* lp = &layout.pages[i];
        if (!lp->shown || lp->visibleRatio <= 0) {
            continue;
        }
        SumatraPageImage* page = [[[SumatraPageImage alloc] init] autorelease];
        page.pageNo = lp->pageNo;
        page.frame = NSMakeRect(lp->x, lp->y, lp->width, lp->height);
        page.layoutZoom = lp->layoutZoom;
        BOOL exact = NO;
        page.image = [self imageForPage:lp->pageNo renderZoom:(float)lp->renderZoom request:request exact:&exact];
        allExact = allExact && exact;
        page.findRects = [self highlightRects:Highlight::Find forPage:lp];
        page.selectionRects = [self highlightRects:Highlight::Selection forPage:lp];
        [pages addObject:page];
        if (first == 0) {
            first = lp->pageNo;
        }
        last = lp->pageNo;
    }

    if (!_pinnedPage && layout.currentPage >= 1) {
        tab.currentPage = layout.currentPage;
    }
    int current = MAX(1, MIN(layout.pageCount, tab.currentPage));
    MacLayoutPage* currentPage = &layout.pages[current - 1];
    if (currentPage->shown && currentPage->layoutZoom > 0) {
        _currentLayoutZoom = currentPage->layoutZoom;
    }

    // prefetch neighbours; single page mode lays out one page, so reuse its zoom
    if (request && first > 0) {
        for (int d = 1; d <= 2; d++) {
            int neighbours[] = {last + d, first - d};
            for (int pageNo : neighbours) {
                if (pageNo < 1 || pageNo > layout.pageCount) {
                    continue;
                }
                MacLayoutPage* np = &layout.pages[pageNo - 1];
                double zoom = np->shown ? np->renderZoom : currentPage->renderZoom;
                MacRequestPage(tab.document, pageNo, (float)zoom, tab.rotation, d);
            }
        }
        [self pruneImageCacheFrom:first - 2 to:last + 2];
    }
    MacFreeDocumentLayout(&layout);

    _visibleRendered = allExact && [pages count] > 0;
    [_documentView setBusy:NO];
    [_documentView setMessage:nil];
    [_documentView setPages:pages];
    [self pageStateChanged];
}

- (void)pageRenderReady {
    if (_refreshScheduled) {
        return;
    }
    _refreshScheduled = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
      _refreshScheduled = NO;
      if (_active) {
          [self updateLayout];
      }
    });
}

#pragma mark - Scrolling

- (void)scrollToOrigin:(NSPoint)origin {
    NSClipView* clip = [_scrollView contentView];
    NSRect visible = [clip bounds];
    NSSize docSize = [_documentView frame].size;
    CGFloat maxX = MAX(0.0, docSize.width - visible.size.width);
    CGFloat maxY = MAX(0.0, docSize.height - visible.size.height);
    origin.x = floor(MAX(0.0, MIN(maxX, origin.x)));
    origin.y = floor(MAX(0.0, MIN(maxY, origin.y)));
    if (NSEqualPoints(origin, visible.origin)) {
        return;
    }
    _programmaticScroll = YES;
    [clip scrollToPoint:origin];
    [_scrollView reflectScrolledClipView:clip];
    _programmaticScroll = NO;
}

- (void)clipBoundsChanged:(NSNotification*)notification {
    (void)notification;
    if (_programmaticScroll || !_active) {
        return;
    }
    if (_inLayout) {
        _layoutAgain = YES;
        return;
    }
    _pinnedPage = NO;
    [self updateLayout];
}

- (void)clipFrameChanged:(NSNotification*)notification {
    (void)notification;
    if (!_active) {
        [self showEmptyState];
        return;
    }
    if (_inLayout) {
        _layoutAgain = YES;
        return;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    [self relayoutKeepingAnchor:[self anchorAtClipPoint:NSMakePoint(visible.size.width / 2.0, 0)]];
}

- (ViewAnchor)anchorAtClipPoint:(NSPoint)clipPoint {
    ViewAnchor anchor = {0, 0, 0, clipPoint};
    NSRect visible = [[_scrollView contentView] bounds];
    NSPoint p = NSMakePoint(visible.origin.x + clipPoint.x, visible.origin.y + clipPoint.y);
    SumatraPageImage* best = nil;
    CGFloat bestDistance = CGFLOAT_MAX;
    for (SumatraPageImage* page in [_documentView pages]) {
        NSRect f = [page frame];
        CGFloat d = 0;
        if (p.y < NSMinY(f)) {
            d = NSMinY(f) - p.y;
        } else if (p.y > NSMaxY(f)) {
            d = p.y - NSMaxY(f);
        }
        if (d < bestDistance) {
            bestDistance = d;
            best = page;
        }
    }
    if (!best) {
        return anchor;
    }
    NSRect f = [best frame];
    anchor.pageNo = [best pageNo];
    anchor.fx = f.size.width > 0 ? (p.x - f.origin.x) / f.size.width : 0;
    anchor.fy = f.size.height > 0 ? (p.y - f.origin.y) / f.size.height : 0;
    return anchor;
}

- (void)relayoutKeepingAnchor:(ViewAnchor)anchor {
    [self updateLayout];
    if (anchor.pageNo > 0) {
        NSRect f = [self frameOfPage:anchor.pageNo zoom:nullptr];
        if (!NSIsEmptyRect(f)) {
            NSPoint p = NSMakePoint(f.origin.x + (anchor.fx * f.size.width), f.origin.y + (anchor.fy * f.size.height));
            [self scrollToOrigin:NSMakePoint(p.x - anchor.clipPoint.x, p.y - anchor.clipPoint.y)];
        }
    }
    [self updateLayout];
}

- (NSPoint)clipCenter {
    NSRect visible = [[_scrollView contentView] bounds];
    return NSMakePoint(visible.size.width / 2.0, visible.size.height / 2.0);
}

// Scrolls so pageNo's top (or bottom) edge is at the top (bottom) of the view.
- (void)showPage:(int)pageNo position:(PagePosition)position {
    SumatraTabState* tab = [self loadedTab];
    if (!tab) {
        return;
    }
    tab.currentPage = MAX(1, MIN(tab.pageCount, pageNo));
    _pinnedPage = YES;
    [self updateLayout];
    NSRect f = [self frameOfPage:tab.currentPage zoom:nullptr];
    NSRect visible = [[_scrollView contentView] bounds];
    NSPoint origin = visible.origin;
    if (!NSIsEmptyRect(f)) {
        if (position == PagePosition::Bottom) {
            origin.y = NSMaxY(f) - visible.size.height + kPageTopGap;
        } else {
            origin.y = f.origin.y - kPageTopGap;
        }
    }
    [self scrollToOrigin:origin];
    [self updateLayout];
}

static BOOL SwapsAxes(int rotation) {
    return rotation == 90 || rotation == 270;
}

// The Windows app's ScrollState (DisplayModel::GetScrollState): the first
// visible page and the page point at the view's top-left, clamped into the
// page. A coordinate is -1 where the page's edge (margin) is in view; unlike
// Windows, -1 goes to the page axis matching that screen axis when rotated.
- (void)captureScrollState:(SumatraTabState*)tab {
    MacDocumentLayout layout = {};
    if (tab != _active || !tab.document || ![self buildLayout:&layout]) {
        return;
    }
    NSRect visible = [[_scrollView contentView] bounds];

    // the current page shown the way -showPage: shows it (after go to page, or
    // with all pages in view): that page is what the user is on
    int current = tab.currentPage;
    if (current >= 1 && current <= layout.pageCount && layout.pages[current - 1].shown) {
        MacLayoutPage* cp = &layout.pages[current - 1];
        NSRect f = NSMakeRect(cp->x, cp->y, cp->width, cp->height);
        CGFloat maxY = MAX(0.0, layout.canvasHeight - visible.size.height);
        CGFloat topY = floor(MAX(0.0, MIN(maxY, f.origin.y - kPageTopGap)));
        if (NSIntersectsRect(f, visible) && fabs(visible.origin.y - topY) < 1.0 && f.origin.x >= visible.origin.x) {
            tab.scrollPage = current;
            tab.scrollX = -1;
            tab.scrollY = -1;
            tab.hasScrollState = YES;
            MacFreeDocumentLayout(&layout);
            return;
        }
    }

    for (int i = 0; i < layout.pageCount; i++) {
        MacLayoutPage* lp = &layout.pages[i];
        NSRect f = NSMakeRect(lp->x, lp->y, lp->width, lp->height);
        if (!lp->shown || !NSIntersectsRect(f, visible)) {
            continue;
        }
        double vx = MAX(visible.origin.x, f.origin.x) - f.origin.x;
        double vy = MAX(visible.origin.y, f.origin.y) - f.origin.y;
        double px = 0;
        double py = 0;
        if (!MacPagePointFromView(tab.document, lp->pageNo, vx, vy, lp->layoutZoom, tab.rotation, &px, &py)) {
            break;
        }
        BOOL marginX = f.origin.x > visible.origin.x;
        BOOL marginY = f.origin.y > visible.origin.y;
        BOOL swap = SwapsAxes(tab.rotation);
        tab.scrollPage = lp->pageNo;
        tab.scrollX = (swap ? marginY : marginX) ? -1 : px;
        tab.scrollY = (swap ? marginX : marginY) ? -1 : py;
        tab.hasScrollState = YES;
        break;
    }
    MacFreeDocumentLayout(&layout);
}

// Shows the active tab's saved scroll state (-captureScrollState:).
- (void)restoreScrollState {
    SumatraTabState* tab = _active;
    [self showPage:tab.scrollPage position:PagePosition::Top];
    if (tab.scrollX < 0 && tab.scrollY < 0) {
        return;
    }
    double zoom = 0;
    NSRect f = [self frameOfPage:tab.scrollPage zoom:&zoom];
    double vx = 0;
    double vy = 0;
    if (NSIsEmptyRect(f) || !MacViewPointFromPage(tab.document, tab.scrollPage, MAX(tab.scrollX, 0.0),
                                                  MAX(tab.scrollY, 0.0), zoom, tab.rotation, &vx, &vy)) {
        return;
    }
    BOOL swap = SwapsAxes(tab.rotation);
    NSPoint origin = [[_scrollView contentView] bounds].origin;
    if ((swap ? tab.scrollY : tab.scrollX) >= 0) {
        origin.x = round(f.origin.x + vx);
    }
    if ((swap ? tab.scrollX : tab.scrollY) >= 0) {
        origin.y = round(f.origin.y + vy);
    }
    _pinnedPage = NO;
    [self scrollToOrigin:origin];
    [self updateLayout];
}

- (void)pushHistoryFrom:(int)from to:(int)to {
    SumatraTabState* tab = _active;
    if (!tab || from == to) {
        return;
    }
    NSMutableArray* history = tab.history;
    NSInteger idx = tab.historyIndex;
    if ([history count] == 0) {
        [history addObject:[NSNumber numberWithInt:from]];
        idx = 0;
    } else {
        while ((NSInteger)[history count] > idx + 1) {
            [history removeLastObject];
        }
        [history replaceObjectAtIndex:(NSUInteger)idx withObject:[NSNumber numberWithInt:from]];
    }
    [history addObject:[NSNumber numberWithInt:to]];
    while ([history count] > kMaxHistory) {
        [history removeObjectAtIndex:0];
    }
    tab.historyIndex = (int)[history count] - 1;
}

// A jump (link, outline, go to page, first/last page, find) that Back undoes.
- (void)goToPage:(int)pageNo {
    SumatraTabState* tab = [self loadedTab];
    if (!tab) {
        return;
    }
    pageNo = MAX(1, MIN(tab.pageCount, pageNo));
    [self pushHistoryFrom:tab.currentPage to:pageNo];
    [self showPage:pageNo position:PagePosition::Top];
}

- (void)scrollVerticallyBy:(CGFloat)dy {
    SumatraTabState* tab = [self loadedTab];
    if (!tab) {
        return;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    CGFloat maxY = MAX(0.0, NSHeight([_documentView frame]) - visible.size.height);
    if (!tab.continuous) {
        if (dy > 0 && visible.origin.y >= maxY - 0.5) {
            if (tab.currentPage < tab.pageCount) {
                [self showPage:tab.currentPage + 1 position:PagePosition::Top];
            }
            return;
        }
        if (dy < 0 && visible.origin.y <= 0.5) {
            if (tab.currentPage > 1) {
                [self showPage:tab.currentPage - 1 position:PagePosition::Bottom];
            }
            return;
        }
    }
    _pinnedPage = NO;
    [self scrollToOrigin:NSMakePoint(visible.origin.x, visible.origin.y + dy)];
    [self updateLayout];
}

#pragma mark - SumatraDocumentViewOwner

- (void*)documentHandle {
    return _active ? _active.document : nullptr;
}

- (int)documentRotation {
    return _active ? _active.rotation : 0;
}

- (NSString*)documentAccessibilityLabel {
    if (!_active) {
        return @"No document open";
    }
    if (!_active.document) {
        return [NSString stringWithFormat:@"Opening %@", [_active.path lastPathComponent]];
    }
    return [NSString stringWithFormat:@"%@, page %d of %d", [_active.path lastPathComponent], _active.currentPage,
                                      _active.pageCount];
}

- (void)documentLinkClickedOnPage:(int)pageNo x:(double)x y:(double)y zoom:(double)zoom {
    SumatraTabState* tab = _active;
    if (!tab) {
        return;
    }
    MacLink link = {};
    if (!MacLinkAtPoint(tab.document, pageNo, x, y, zoom, tab.rotation, &link)) {
        return;
    }
    MacLinkKind kind = link.kind;
    int target = link.pageNo;
    NSString* value = StringFromUtf8(link.value);
    MacFreeLink(&link);
    if (kind == MacLinkKind::Page) {
        [self goToPage:target];
    } else if (kind == MacLinkKind::Url && value) {
        [self openLinkURL:value];
    } else if (kind == MacLinkKind::File && value) {
        [self openLinkedFile:value];
    }
}

- (void)documentSelectionChanged {
    [self updateLayout];
}

- (void)documentScrollLines:(int)lines {
    [self scrollVerticallyBy:lines * kLineScroll];
}

- (void)documentScrollScreens:(int)screens {
    NSRect visible = [[_scrollView contentView] bounds];
    [self scrollVerticallyBy:screens * MAX(kLineScroll, visible.size.height - kLineScroll)];
}

// Left/right scroll sideways when the page is wider than the view, else flip pages.
- (void)documentHorizontalArrow:(int)direction {
    if (!_active) {
        return;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    CGFloat maxX = MAX(0.0, NSWidth([_documentView frame]) - visible.size.width);
    CGFloat x = MAX(0.0, MIN(maxX, visible.origin.x + (direction * kLineScroll)));
    if (maxX > 0.5 && fabs(x - visible.origin.x) > 0.5) {
        _pinnedPage = NO;
        [self scrollToOrigin:NSMakePoint(x, visible.origin.y)];
        [self updateLayout];
        return;
    }
    if (direction > 0) {
        [self goToNextPage:nil];
    } else {
        [self goToPrevPage:nil];
    }
}

- (void)documentPanBy:(NSPoint)delta {
    if (!_active) {
        return;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    _pinnedPage = NO;
    [self scrollToOrigin:NSMakePoint(visible.origin.x + delta.x, visible.origin.y + delta.y)];
    [self updateLayout];
}

// While the pinch is in progress existing images are scaled; renders are
// requested once it ends.
- (void)documentMagnify:(CGFloat)magnification atPoint:(NSPoint)point ending:(BOOL)ending {
    if (!_active) {
        return;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    NSPoint clipPoint = NSMakePoint(point.x - visible.origin.x, point.y - visible.origin.y);
    _liveMagnify = !ending;
    _smartZoomReturn = 0;
    [self setZoom:[self displayZoom] * (1.0 + magnification) anchorClipPoint:clipPoint];
}

// Two-finger double tap: zoom in around the point, again to go back.
- (void)documentSmartMagnifyAtPoint:(NSPoint)point {
    if (!_active) {
        return;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    NSPoint clipPoint = NSMakePoint(point.x - visible.origin.x, point.y - visible.origin.y);
    CGFloat target = 0;
    if (_smartZoomReturn != 0) {
        target = _smartZoomReturn;
        _smartZoomReturn = 0;
    } else {
        _smartZoomReturn = _active.zoom;
        target = MIN(kZoomMax, MAX(1.0, [self displayZoom] * 2.0));
    }
    [self setZoom:target anchorClipPoint:clipPoint];
}

// Single page mode: the wheel moves to the next/previous page at the page
// edges, once per gesture (or per interval for mouse wheels).
- (BOOL)documentWheelFlip:(NSEvent*)event {
    SumatraTabState* tab = [self loadedTab];
    if (!tab || tab.continuous) {
        return NO;
    }
    CGFloat dy = [event scrollingDeltaY];
    if (fabs(dy) < 0.5 || fabs(dy) < fabs([event scrollingDeltaX])) {
        return NO;
    }
    NSRect visible = [[_scrollView contentView] bounds];
    CGFloat maxY = MAX(0.0, NSHeight([_documentView frame]) - visible.size.height);
    BOOL towardEnd = dy < 0;
    BOOL atEdge = towardEnd ? visible.origin.y >= maxY - 0.5 : visible.origin.y <= 0.5;
    if (!atEdge) {
        return NO;
    }
    NSEventPhase phase = [event phase];
    BOOL gesture = phase != NSEventPhaseNone || [event momentumPhase] != NSEventPhaseNone;
    if (gesture && phase != NSEventPhaseBegan) {
        return YES;
    }
    double now = [NSDate timeIntervalSinceReferenceDate];
    if (now - _lastWheelFlip < kWheelFlipInterval) {
        return YES;
    }
    _lastWheelFlip = now;
    if (towardEnd && tab.currentPage < tab.pageCount) {
        [self showPage:tab.currentPage + 1 position:PagePosition::Top];
    } else if (!towardEnd && tab.currentPage > 1) {
        [self showPage:tab.currentPage - 1 position:PagePosition::Bottom];
    }
    return YES;
}

// Esc: leave full screen, else clear the selection and search highlights.
- (void)documentCancel {
    if ([_window styleMask] & NSWindowStyleMaskFullScreen) {
        [_window toggleFullScreen:nil];
        return;
    }
    void* doc = [self documentHandle];
    if (!doc) {
        return;
    }
    MacClearSelection(doc);
    [self stopSelectAll:_active];
    MacFindCancel(doc);
    MacFindClear(doc);
    _active.findToken = 0;
    [self setFindBusy:NO];
    [self showFindStatus:nil];
    [self updateLayout];
}

#pragma mark - SumatraSidebarHost

- (void*)sidebarDocumentHandle {
    return [self documentHandle];
}

- (int)sidebarCurrentPage {
    return _active ? _active.currentPage : 0;
}

- (int)sidebarDocumentRotation {
    return [self documentRotation];
}

- (void)sidebarGoToPage:(int)pageNo {
    [self goToPage:pageNo];
}

#pragma mark - SumatraSelfTestHost

- (NSWindow*)selfTestWindow {
    return _window;
}

- (NSView*)selfTestDocumentView {
    return _documentView;
}

- (NSSearchField*)selfTestSearchField {
    return [_searchField window] == _window ? _searchField : nil;
}

- (NSTextField*)selfTestPageField {
    return [_pageField window] == _window ? _pageField : nil;
}

- (NSString*)selfTestActivePath {
    return _active.path;
}

- (struct SumatraTestState)selfTestState {
    struct SumatraTestState s = {};
    SumatraTabState* tab = _active;
    s.tabCount = (int)[_tabs count];
    s.hasTab = tab != nil;
    s.loading = tab && !tab.document;
    s.lastOpenError = _lastOpenError;
    s.findBarVisible = ![_findBar isHidden];
    s.sidebarVisible = _sidebarVisible;
    s.thumbnails = _sidebarVisible ? [_sidebar visibleThumbnailCount] : 0;
    if (!tab.document) {
        return s;
    }
    s.pageCount = tab.pageCount;
    s.currentPage = tab.currentPage;
    s.zoom = tab.zoom;
    s.displayZoom = [self displayZoom];
    s.rotation = tab.rotation;
    s.continuous = tab.continuous;
    s.rendered = _visibleRendered;
    s.findPending = tab.findToken != 0;
    s.findPage = MacFindResultPage(tab.document);
    s.hasSelection = MacHasSelection(tab.document);
    s.selecting = tab.selectAllToken != 0;
    NSRect f = [self frameOfPage:tab.currentPage zoom:nullptr];
    if (!NSIsEmptyRect(f)) {
        NSPoint origin = [[_scrollView contentView] bounds].origin;
        s.pageOffsetX = origin.x - f.origin.x;
        s.pageOffsetY = origin.y - f.origin.y;
    }
    return s;
}

// Printing without panels to a PDF file (pages first..last), like Print › Save as PDF.
- (BOOL)selfTestPrintToPDF:(NSString*)path firstPage:(int)first lastPage:(int)last {
    if (!_active.document || _printingDocument) {
        return NO;
    }
    NSPrintInfo* info = [[[NSPrintInfo sharedPrintInfo] copy] autorelease];
    NSMutableDictionary* dict = [info dictionary];
    [dict setObject:NSPrintSaveJob forKey:NSPrintJobDisposition];
    [dict setObject:[NSURL fileURLWithPath:path] forKey:NSPrintJobSavingURL];
    [dict setObject:[NSNumber numberWithBool:NO] forKey:NSPrintAllPages];
    [dict setObject:[NSNumber numberWithInt:first] forKey:NSPrintFirstPage];
    [dict setObject:[NSNumber numberWithInt:last] forKey:NSPrintLastPage];
    NSPrintOperation* operation = [self printOperationWithInfo:info];
    [operation setShowsPrintPanel:NO];
    [operation setShowsProgressPanel:NO];
    return [self runPrintOperation:operation];
}

// What quitting and launching again does to the settings file.
- (void)selfTestReloadPrefs {
    MacPrefsShutdown();
    MacPrefsInit([_settingsPath fileSystemRepresentation]);
}

// Quits through the normal shutdown path; applicationWillTerminate: exits with exitCode.
- (void)selfTestFinished:(int)exitCode {
    _selfTestExitCode = exitCode;
    _selfTestDone = YES;
    [NSApp terminate:nil];
    exit(exitCode);
}

#pragma mark - Links

- (void)openLinkURL:(NSString*)value {
    NSURL* url = [NSURL URLWithString:value];
    NSString* scheme = [[url scheme] lowercaseString];
    if ([scheme isEqualToString:@"file"]) {
        [self openLinkedFile:[url path]];
        return;
    }
    NSArray* allowed = @[ @"http", @"https", @"mailto", @"ftp" ];
    if (!url || !scheme || ![allowed containsObject:scheme]) {
        NSBeep();
        return;
    }
    if (_selfTest) {
        [_selfTest recordIgnored:value];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:url];
}

// Supported documents open in a new tab; other files only after confirmation,
// and programs never.
- (void)openLinkedFile:(NSString*)value {
    NSString* path = [value stringByExpandingTildeInPath];
    if (![path isAbsolutePath]) {
        path = [[_active.path stringByDeletingLastPathComponent] stringByAppendingPathComponent:path];
    }
    path = [path stringByStandardizingPath];
    NSString* name = [path lastPathComponent];
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        [self showAlertWithMessage:[NSString stringWithFormat:@"The linked file “%@” could not be found.", name]
                              info:[path stringByAbbreviatingWithTildeInPath]
                             style:NSAlertStyleWarning];
        return;
    }
    BOOL isPackage = [[NSWorkspace sharedWorkspace] isFilePackageAtPath:path];
    if (!isPackage && MacIsSupportedPath(FsPath(path))) {
        [self openPath:path];
        return;
    }
    if (isPackage || IsRiskyLinkTarget(path)) {
        [self showAlertWithMessage:[NSString stringWithFormat:@"“%@” was not opened.", name]
                              info:@"SumatraPDF doesn't open programs, scripts or installers from documents."
                             style:NSAlertStyleWarning];
        return;
    }
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:[NSString stringWithFormat:@"Open “%@”?", name]];
    [alert setInformativeText:[NSString stringWithFormat:@"The document links to this file. It will be opened "
                                                         @"with its default application.\n\n%@",
                                                         [path stringByAbbreviatingWithTildeInPath]]];
    [alert addButtonWithTitle:@"Open"];
    [alert addButtonWithTitle:@"Cancel"];
    NSURL* url = [NSURL fileURLWithPath:path];
    [self presentAlert:alert
            completion:^(NSModalResponse response) {
              if (response == NSAlertFirstButtonReturn) {
                  [[NSWorkspace sharedWorkspace] openURL:url];
              }
            }];
}

#pragma mark - Toolbar

static NSArray* ToolbarDefaultItems() {
    return @[
        kToolbarSidebar, kToolbarPrevPage, kToolbarNextPage, kToolbarPage, kToolbarTabs,
        NSToolbarFlexibleSpaceItemIdentifier, kToolbarZoomOut, kToolbarZoomIn, kToolbarFitWidth, kToolbarRotateLeft,
        kToolbarRotateRight, kToolbarSearch
    ];
}

static NSArray* ToolbarAllowedItems() {
    NSMutableArray* items = [NSMutableArray arrayWithArray:ToolbarDefaultItems()];
    [items addObjectsFromArray:@[
        kToolbarOpen,
        kToolbarZoomActual,
        kToolbarFitPage,
        NSToolbarSpaceItemIdentifier,
    ]];
    return items;
}

- (void)installToolbar {
    NSToolbar* toolbar = [[[NSToolbar alloc] initWithIdentifier:kToolbarIdentifier] autorelease];
    [toolbar setDelegate:self];
    [toolbar setDisplayMode:NSToolbarDisplayModeIconOnly];
    [toolbar setAllowsUserCustomization:YES];
    [toolbar setAutosavesConfiguration:!_testing];
    [_toolbar release];
    _toolbar = [toolbar retain];
    [_window setToolbar:toolbar];
}

- (NSArray*)toolbarDefaultItemIdentifiers:(NSToolbar*)toolbar {
    (void)toolbar;
    return ToolbarDefaultItems();
}

- (NSArray*)toolbarAllowedItemIdentifiers:(NSToolbar*)toolbar {
    (void)toolbar;
    return ToolbarAllowedItems();
}

- (NSToolbarItem*)buttonItem:(NSString*)identifier
                       label:(NSString*)label
                     tooltip:(NSString*)tooltip
                       image:(NSImage*)image
                      action:(SEL)action {
    NSToolbarItem* item = [[[NSToolbarItem alloc] initWithItemIdentifier:identifier] autorelease];
    [item setLabel:label];
    [item setPaletteLabel:label];
    [item setToolTip:tooltip];
    [item setImage:image];
    [item setTarget:self];
    [item setAction:action];
    return item;
}

- (NSTextField*)makeLabel:(NSRect)frame {
    NSTextField* label = [[[NSTextField alloc] initWithFrame:frame] autorelease];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setFont:[NSFont systemFontOfSize:12]];
    [label setTextColor:[NSColor secondaryLabelColor]];
    return label;
}

// View items: only the instance inserted into the toolbar is remembered;
// the customization palette gets throwaway copies.
- (NSToolbarItem*)pageItem:(NSString*)identifier inserted:(BOOL)inserted {
    NSToolbarItem* item = [[[NSToolbarItem alloc] initWithItemIdentifier:identifier] autorelease];
    [item setLabel:@"Page"];
    [item setPaletteLabel:@"Page Number"];
    [item setToolTip:@"Current page; type a page number and press Return"];
    NSView* container = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 118, 24)] autorelease];
    NSTextField* field = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 1, 50, 22)] autorelease];
    [field setAlignment:NSTextAlignmentRight];
    [field setFont:[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular]];
    [field setTarget:self];
    [field setAction:@selector(pageFieldAction:)];
    [field setDelegate:self];
    [field setAccessibilityLabel:@"Page number"];
    NSTextField* countLabel = [self makeLabel:NSMakeRect(54, 4, 64, 16)];
    [countLabel setFont:[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular]];
    [countLabel setAccessibilityLabel:@"Page count"];
    [container addSubview:field];
    [container addSubview:countLabel];
    [item setView:container];
    [item setMinSize:NSMakeSize(118, 24)];
    [item setMaxSize:NSMakeSize(118, 24)];
    [item setVisibilityPriority:NSToolbarItemVisibilityPriorityHigh];
    if (inserted) {
        [_pageField release];
        _pageField = [field retain];
        [_pageCountLabel release];
        _pageCountLabel = [countLabel retain];
        [self updatePageControls];
    }
    return item;
}

- (NSToolbarItem*)tabsItem:(NSString*)identifier inserted:(BOOL)inserted {
    NSToolbarItem* item = [[[NSToolbarItem alloc] initWithItemIdentifier:identifier] autorelease];
    [item setLabel:@"Documents"];
    [item setPaletteLabel:@"Documents"];
    [item setToolTip:@"Open documents (⌃⇥ switches, ⌘W closes)"];
    NSSegmentedControl* control = [[[NSSegmentedControl alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)] autorelease];
    [control setSegmentStyle:NSSegmentStyleTexturedRounded];
    [control setTrackingMode:NSSegmentSwitchTrackingSelectOne];
    [control setTarget:self];
    [control setAction:@selector(selectTab:)];
    [control setAccessibilityLabel:@"Open documents"];
    [item setView:control];
    [item setMinSize:NSMakeSize(100, 24)];
    [item setMaxSize:NSMakeSize(520, 24)];
    if (inserted) {
        [_tabSelector release];
        _tabSelector = [control retain];
        [self refreshTabSelector];
    }
    return item;
}

- (NSToolbarItem*)searchItem:(NSString*)identifier inserted:(BOOL)inserted {
    NSToolbarItem* item = [[[NSToolbarItem alloc] initWithItemIdentifier:identifier] autorelease];
    [item setLabel:@"Search"];
    [item setPaletteLabel:@"Search"];
    [item setToolTip:@"Find text (↩ next, ⇧↩ previous)"];
    NSView* container = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)] autorelease];
    NSSearchField* field = [[[NSSearchField alloc] initWithFrame:NSMakeRect(0, 1, 200, 22)] autorelease];
    [field setPlaceholderString:@"Search"];
    [[field cell] setSendsWholeSearchString:YES];
    [field setTarget:self];
    [field setAction:@selector(searchFieldAction:)];
    [field setDelegate:(id)self];
    [field setAccessibilityLabel:@"Search in document"];
    NSProgressIndicator* spinner = [[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(206, 4, 16, 16)] autorelease];
    [spinner setStyle:NSProgressIndicatorStyleSpinning];
    [spinner setControlSize:NSControlSizeSmall];
    [spinner setDisplayedWhenStopped:NO];
    [spinner setAccessibilityLabel:@"Searching"];
    NSTextField* status = [self makeLabel:NSMakeRect(226, 4, 74, 16)];
    [status setAccessibilityLabel:@"Search status"];
    [container addSubview:field];
    [container addSubview:spinner];
    [container addSubview:status];
    [item setView:container];
    [item setMinSize:NSMakeSize(300, 24)];
    [item setMaxSize:NSMakeSize(300, 24)];
    // narrow windows move other items into the overflow menu first
    [item setVisibilityPriority:NSToolbarItemVisibilityPriorityHigh];
    if (inserted) {
        [_searchField release];
        _searchField = [field retain];
        [_findSpinner release];
        _findSpinner = [spinner retain];
        [_findStatus release];
        _findStatus = [status retain];
        [_searchField setStringValue:_findText ?: @""];
        [_searchField setEnabled:_active != nil];
    }
    return item;
}

- (NSToolbarItem*)toolbar:(NSToolbar*)toolbar
        itemForItemIdentifier:(NSString*)identifier
    willBeInsertedIntoToolbar:(BOOL)flag {
    (void)toolbar;
    if ([identifier isEqualToString:kToolbarPage]) {
        return [self pageItem:identifier inserted:flag];
    }
    if ([identifier isEqualToString:kToolbarTabs]) {
        return [self tabsItem:identifier inserted:flag];
    }
    if ([identifier isEqualToString:kToolbarSearch]) {
        return [self searchItem:identifier inserted:flag];
    }
    if ([identifier isEqualToString:kToolbarSidebar]) {
        return [self buttonItem:identifier
                          label:@"Sidebar"
                        tooltip:@"Show or hide the sidebar (⌥⌘S)"
                          image:ToolbarImage(@"sidebar.left", @"Sidebar", @"☰")
                         action:@selector(toggleSidebar:)];
    }
    if ([identifier isEqualToString:kToolbarOpen]) {
        return [self buttonItem:identifier
                          label:@"Open"
                        tooltip:@"Open a document (⌘O)"
                          image:ToolbarImage(@"doc", @"Open", @"Open")
                         action:@selector(openDocument:)];
    }
    if ([identifier isEqualToString:kToolbarPrevPage]) {
        return [self buttonItem:identifier
                          label:@"Previous"
                        tooltip:@"Previous page (⌥⌘↑)"
                          image:ToolbarImage(@"chevron.up", @"Previous page", @"▲")
                         action:@selector(goToPrevPage:)];
    }
    if ([identifier isEqualToString:kToolbarNextPage]) {
        return [self buttonItem:identifier
                          label:@"Next"
                        tooltip:@"Next page (⌥⌘↓)"
                          image:ToolbarImage(@"chevron.down", @"Next page", @"▼")
                         action:@selector(goToNextPage:)];
    }
    if ([identifier isEqualToString:kToolbarZoomOut]) {
        return [self buttonItem:identifier
                          label:@"Zoom Out"
                        tooltip:@"Zoom out (⌘-)"
                          image:ToolbarImage(@"minus.magnifyingglass", @"Zoom out", @"−")
                         action:@selector(zoomOut:)];
    }
    if ([identifier isEqualToString:kToolbarZoomActual]) {
        return [self buttonItem:identifier
                          label:@"Actual Size"
                        tooltip:@"Actual size (⌘0)"
                          image:ToolbarImage(@"1.magnifyingglass", @"Actual size", @"1:1")
                         action:@selector(zoomActualSize:)];
    }
    if ([identifier isEqualToString:kToolbarZoomIn]) {
        return [self buttonItem:identifier
                          label:@"Zoom In"
                        tooltip:@"Zoom in (⌘+)"
                          image:ToolbarImage(@"plus.magnifyingglass", @"Zoom in", @"+")
                         action:@selector(zoomIn:)];
    }
    if ([identifier isEqualToString:kToolbarFitPage]) {
        return [self buttonItem:identifier
                          label:@"Zoom to Fit"
                        tooltip:@"Fit the whole page (⌘9)"
                          image:ToolbarImage(@"arrow.down.right.and.arrow.up.left", @"Zoom to fit", @"Fit")
                         action:@selector(zoomFitPage:)];
    }
    if ([identifier isEqualToString:kToolbarFitWidth]) {
        return [self buttonItem:identifier
                          label:@"Zoom to Width"
                        tooltip:@"Fit the page width (⌘8)"
                          image:ToolbarImage(@"arrow.left.and.right", @"Zoom to width", @"↔")
                         action:@selector(zoomFitWidth:)];
    }
    if ([identifier isEqualToString:kToolbarRotateLeft]) {
        return [self buttonItem:identifier
                          label:@"Rotate Left"
                        tooltip:@"Rotate left (⌘L)"
                          image:ToolbarImage(@"rotate.left", @"Rotate left", @"⟲")
                         action:@selector(rotateLeft:)];
    }
    if ([identifier isEqualToString:kToolbarRotateRight]) {
        return [self buttonItem:identifier
                          label:@"Rotate Right"
                        tooltip:@"Rotate right (⌘R)"
                          image:ToolbarImage(@"rotate.right", @"Rotate right", @"⟳")
                         action:@selector(rotateRight:)];
    }
    return nil;
}

- (BOOL)validateToolbarItem:(NSToolbarItem*)item {
    return [self canPerformAction:[item action]];
}

- (IBAction)selectTab:(id)sender {
    (void)sender;
    [self activateTabAtIndex:(int)[_tabSelector selectedSegment]];
}

- (IBAction)pageFieldAction:(id)sender {
    (void)sender;
    SumatraTabState* tab = _active;
    if (!tab) {
        return;
    }
    int pageNo = [_pageField intValue];
    [_window makeFirstResponder:_documentView];
    if (pageNo < 1 || pageNo > tab.pageCount) {
        NSBeep();
    } else {
        [self goToPage:pageNo];
    }
    [self updatePageControls];
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)command {
    (void)textView;
    if (command != @selector(cancelOperation:)) {
        return NO;
    }
    if (control == _searchField || control == _findBarField) {
        [self documentCancel];
        [self hideFindBar:nil];
        [_window makeFirstResponder:_documentView];
        return YES;
    }
    if (control == _pageField) {
        [_window makeFirstResponder:_documentView];
        [self updatePageControls];
        return YES;
    }
    return NO;
}

#pragma mark - Find

- (void)setFindBusy:(BOOL)busy {
    if (busy) {
        [_findSpinner startAnimation:nil];
        [_findBarSpinner startAnimation:nil];
    } else {
        [_findSpinner stopAnimation:nil];
        [_findBarSpinner stopAnimation:nil];
    }
}

- (void)showFindStatus:(NSString*)status {
    NSColor* color = status ? [NSColor systemRedColor] : [NSColor secondaryLabelColor];
    [_findStatus setStringValue:status ?: @""];
    [_findStatus setTextColor:color];
    [_findBarStatus setStringValue:status ?: @""];
    [_findBarStatus setTextColor:color];
    if ([status length] > 0) {
        id element = [_findBar isHidden] ? (_findStatus ?: (id)_window) : (id)_findBarStatus;
        NSAccessibilityPostNotificationWithUserInfo(element, NSAccessibilityAnnouncementRequestedNotification,
                                                    @{NSAccessibilityAnnouncementKey : status});
    }
}

// The toolbar's search field is on screen (not hidden, removed or in the overflow menu).
- (BOOL)toolbarSearchFieldVisible {
    return [_toolbar isVisible] && _searchField && [_searchField window] == _window &&
           ![_searchField isHiddenOrHasHiddenAncestor];
}

// Find bar above the document: ⌘F uses it when the toolbar can't show its search field.
- (void)createFindBar {
    NSRect bounds = [_documentPane bounds];
    _findBar = [[NSView alloc] initWithFrame:NSMakeRect(0, bounds.size.height - kFindBarHeight, bounds.size.width,
                                                        kFindBarHeight)];
    [_findBar setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [_findBar setHidden:YES];

    _findBarField = [[NSSearchField alloc] initWithFrame:NSMakeRect(8, 4, 220, 22)];
    [_findBarField setPlaceholderString:@"Find in document"];
    [[_findBarField cell] setSendsWholeSearchString:YES];
    [_findBarField setTarget:self];
    [_findBarField setAction:@selector(searchFieldAction:)];
    [_findBarField setDelegate:(id)self];
    [_findBarField setAccessibilityLabel:@"Search in document"];

    NSButton* prev = [NSButton buttonWithImage:ToolbarImage(@"chevron.up", @"Previous match", @"▲")
                                        target:self
                                        action:@selector(findPrevious:)];
    [prev setFrame:NSMakeRect(234, 3, 30, 24)];
    [prev setToolTip:@"Previous match (⇧⌘G)"];
    NSButton* next = [NSButton buttonWithImage:ToolbarImage(@"chevron.down", @"Next match", @"▼")
                                        target:self
                                        action:@selector(findNext:)];
    [next setFrame:NSMakeRect(266, 3, 30, 24)];
    [next setToolTip:@"Next match (⌘G)"];

    _findBarSpinner = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(302, 7, 16, 16)];
    [_findBarSpinner setStyle:NSProgressIndicatorStyleSpinning];
    [_findBarSpinner setControlSize:NSControlSizeSmall];
    [_findBarSpinner setDisplayedWhenStopped:NO];
    [_findBarSpinner setAccessibilityLabel:@"Searching"];
    _findBarStatus = [[self makeLabel:NSMakeRect(322, 7, 90, 16)] retain];
    [_findBarStatus setAccessibilityLabel:@"Search status"];

    NSButton* done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(hideFindBar:)];
    [done setFrame:NSMakeRect(bounds.size.width - 76, 3, 68, 24)];
    [done setAutoresizingMask:NSViewMinXMargin];

    for (NSView* view in @[ _findBarField, prev, next, _findBarSpinner, _findBarStatus, done ]) {
        [_findBar addSubview:view];
    }
    [_documentPane addSubview:_findBar];
}

- (void)layoutDocumentPane {
    NSRect bounds = [_documentPane bounds];
    CGFloat barHeight = [_findBar isHidden] ? 0 : kFindBarHeight;
    [_findBar setFrame:NSMakeRect(0, bounds.size.height - kFindBarHeight, bounds.size.width, kFindBarHeight)];
    [_scrollView setFrame:NSMakeRect(0, 0, bounds.size.width, MAX(0.0, bounds.size.height - barHeight))];
}

- (void)showFindBar {
    if ([_findBar isHidden]) {
        [_findBar setHidden:NO];
        [self layoutDocumentPane];
    }
    [_findBarField setStringValue:_findText ?: @""];
    [_window makeFirstResponder:_findBarField];
    [_findBarField selectText:nil];
}

- (IBAction)hideFindBar:(id)sender {
    (void)sender;
    if ([_findBar isHidden]) {
        return;
    }
    BOOL hadFocus = [[_findBarField currentEditor] isEqual:[_window firstResponder]];
    [_findBar setHidden:YES];
    [self layoutDocumentPane];
    if (hadFocus || sender) {
        [_window makeFirstResponder:_documentView];
    }
}

- (void)startFind:(NSString*)text direction:(MacFindDirection)direction {
    SumatraTabState* tab = [self loadedTab];
    if (!tab || [text length] == 0) {
        return;
    }
    // text may be _findText itself: copy before releasing the old value
    NSString* newText = [[text copy] autorelease];
    BOOL sameText = [_findText isEqualToString:newText];
    [_findText release];
    _findText = [newText retain];
    MacFindMode mode = sameText ? MacFindMode::Next : MacFindMode::Restart;
    int token = MacFindStart(tab.document, tab.currentPage, [newText UTF8String], direction, mode, FindDone, self);
    if (token == 0) {
        NSBeep();
        return;
    }
    tab.findToken = token;
    [self showFindStatus:nil];
    [self setFindBusy:YES];
}

- (void)findFinishedForDocument:(void*)document token:(int)token found:(BOOL)found {
    SumatraTabState* tab = _active;
    if (!tab || tab.document != document || tab.findToken != token) {
        return;
    }
    tab.findToken = 0;
    [self setFindBusy:NO];
    if (!found) {
        [self showFindStatus:@"Not found"];
        NSBeep();
        [self updateLayout];
        return;
    }
    [self showFindStatus:nil];
    [self revealFindResult];
}

// Scrolls the current hit into view (centered if it wasn't visible).
- (void)revealFindResult {
    SumatraTabState* tab = _active;
    void* doc = tab.document;
    int pageNo = MacFindResultPage(doc);
    if (pageNo <= 0) {
        [self updateLayout];
        return;
    }
    if (pageNo != tab.currentPage) {
        [self pushHistoryFrom:tab.currentPage to:pageNo];
    }
    tab.currentPage = pageNo;
    _pinnedPage = YES;
    [self updateLayout];
    double zoom = 0;
    NSRect f = [self frameOfPage:pageNo zoom:&zoom];
    if (NSIsEmptyRect(f)) {
        return;
    }
    NSRect hit = NSMakeRect(f.origin.x, f.origin.y, f.size.width, MIN(f.size.height, 40.0));
    MacDisplayRect r = {};
    if (MacFindResultRect(doc, pageNo, 0, zoom, tab.rotation, &r)) {
        hit = NSMakeRect(f.origin.x + r.x, f.origin.y + r.y, r.width, r.height);
    }
    NSRect visible = [[_scrollView contentView] bounds];
    if (!NSContainsRect(visible, hit)) {
        NSPoint origin = visible.origin;
        if (NSMinY(hit) < NSMinY(visible) || NSMaxY(hit) > NSMaxY(visible)) {
            origin.y = NSMidY(hit) - (visible.size.height / 2.0);
        }
        if (NSMinX(hit) < NSMinX(visible) || NSMaxX(hit) > NSMaxX(visible)) {
            origin.x = NSMidX(hit) - (visible.size.width / 2.0);
        }
        [self scrollToOrigin:origin];
    }
    [self updateLayout];
}

- (IBAction)searchFieldAction:(id)sender {
    NSSearchField* field = sender == _findBarField ? _findBarField : _searchField;
    NSString* text = [field stringValue];
    if ([text length] == 0) {
        [self documentCancel];
        [_findText release];
        _findText = nil;
        return;
    }
    BOOL backward = ([[NSApp currentEvent] modifierFlags] & NSEventModifierFlagShift) != 0;
    [self startFind:text direction:backward ? MacFindDirection::Backward : MacFindDirection::Forward];
}

// ⌘F: the toolbar's search field when it's on screen, else the find bar.
- (IBAction)findDocument:(id)sender {
    (void)sender;
    if (!_active) {
        return;
    }
    if (![self toolbarSearchFieldVisible]) {
        [self showFindBar];
        return;
    }
    [self hideFindBar:nil];
    [_window makeFirstResponder:_searchField];
    [_searchField selectText:nil];
}

- (IBAction)findNext:(id)sender {
    (void)sender;
    if ([_findText length] == 0) {
        [self findDocument:nil];
        return;
    }
    [self startFind:_findText direction:MacFindDirection::Forward];
}

- (IBAction)findPrevious:(id)sender {
    (void)sender;
    if ([_findText length] == 0) {
        [self findDocument:nil];
        return;
    }
    [self startFind:_findText direction:MacFindDirection::Backward];
}

- (IBAction)useSelectionForFind:(id)sender {
    (void)sender;
    char* textUtf8 = MacCopySelectionText([self documentHandle]);
    NSString* text = StringFromUtf8(textUtf8);
    MacFreeString(textUtf8);
    NSArray* parts = [text componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSMutableArray* words = [NSMutableArray array];
    for (NSString* part in parts) {
        if ([part length] > 0) {
            [words addObject:part];
        }
    }
    text = [words componentsJoinedByString:@" "];
    if ([text length] == 0) {
        NSBeep();
        return;
    }
    [_findText release];
    _findText = [text copy];
    [_searchField setStringValue:text];
    [_findBarField setStringValue:text];
}

#pragma mark - Zoom, rotation, view mode

- (CGFloat)displayZoom {
    if (!_active || _currentLayoutZoom <= 0) {
        return 1.0;
    }
    double dpi = MacFileDPI(_active.document);
    return (CGFloat)(_currentLayoutZoom * (dpi > 0 ? dpi : 96.0) / 72.0);
}

- (void)setZoom:(CGFloat)zoom anchorClipPoint:(NSPoint)clipPoint {
    SumatraTabState* tab = [self loadedTab];
    if (!tab) {
        return;
    }
    if (zoom > 0) {
        zoom = MAX(kZoomMin, MIN(kZoomMax, zoom));
    }
    ViewAnchor anchor = [self anchorAtClipPoint:clipPoint];
    tab.zoom = zoom;
    if (!_liveMagnify) {
        MacResetRenderer(tab.document);
    }
    [self relayoutKeepingAnchor:anchor];
}

- (CGFloat)nextZoomLevel:(int)direction {
    CGFloat current = [self displayZoom];
    int n = (int)(sizeof(kZoomLevels) / sizeof(kZoomLevels[0]));
    if (direction > 0) {
        for (int i = 0; i < n; i++) {
            if (kZoomLevels[i] > current * 1.01) {
                return kZoomLevels[i];
            }
        }
        return kZoomMax;
    }
    for (int i = n - 1; i >= 0; i--) {
        if (kZoomLevels[i] < current * 0.99) {
            return kZoomLevels[i];
        }
    }
    return kZoomMin;
}

- (IBAction)zoomIn:(id)sender {
    (void)sender;
    _smartZoomReturn = 0;
    [self setZoom:[self nextZoomLevel:1] anchorClipPoint:[self clipCenter]];
}

- (IBAction)zoomOut:(id)sender {
    (void)sender;
    _smartZoomReturn = 0;
    [self setZoom:[self nextZoomLevel:-1] anchorClipPoint:[self clipCenter]];
}

- (IBAction)zoomActualSize:(id)sender {
    (void)sender;
    _smartZoomReturn = 0;
    [self setZoom:1.0 anchorClipPoint:[self clipCenter]];
}

- (IBAction)zoomFitPage:(id)sender {
    (void)sender;
    _smartZoomReturn = 0;
    [self setZoom:kMacZoomFitPage anchorClipPoint:[self clipCenter]];
}

- (IBAction)zoomFitWidth:(id)sender {
    (void)sender;
    _smartZoomReturn = 0;
    [self setZoom:kMacZoomFitWidth anchorClipPoint:NSMakePoint([self clipCenter].x, 0)];
}

- (void)rotateBy:(int)degrees {
    SumatraTabState* tab = [self loadedTab];
    if (!tab) {
        return;
    }
    tab.rotation = (tab.rotation + degrees + 360) % 360;
    [_imageCache removeAllObjects];
    MacResetRenderer(tab.document);
    [_sidebar documentChanged];
    [self showPage:tab.currentPage position:PagePosition::Top];
}

- (IBAction)rotateLeft:(id)sender {
    (void)sender;
    [self rotateBy:-90];
}

- (IBAction)rotateRight:(id)sender {
    (void)sender;
    [self rotateBy:90];
}

- (void)setContinuous:(BOOL)continuous {
    SumatraTabState* tab = [self loadedTab];
    if (!tab || tab.continuous == continuous) {
        return;
    }
    tab.continuous = continuous;
    [self showPage:tab.currentPage position:PagePosition::Top];
}

- (IBAction)setSinglePageView:(id)sender {
    (void)sender;
    [self setContinuous:NO];
}

- (IBAction)setContinuousPageView:(id)sender {
    (void)sender;
    [self setContinuous:YES];
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    if (_active) {
        [self updateLayout];
    }
}

- (void)windowWillClose:(NSNotification*)notification {
    if ([notification object] == _window) {
        [NSApp performSelector:@selector(terminate:) withObject:nil afterDelay:0];
    }
}

#pragma mark - Sidebar

- (void)setSidebarVisible:(BOOL)visible {
    _sidebarVisible = visible;
    _adjustingSidebar = YES;
    NSView* view = [_sidebar view];
    [view setHidden:!visible];
    [_splitView adjustSubviews];
    if (visible) {
        [_splitView setPosition:_sidebarWidth ofDividerAtIndex:0];
    }
    _adjustingSidebar = NO;
    [self saveDefault:[NSNumber numberWithBool:visible] forKey:kDefSidebarVisible];
}

- (void)showSidebarMode:(SumatraSidebarMode)mode {
    [_sidebar setMode:mode];
    [self saveDefault:[NSNumber numberWithInteger:mode] forKey:kDefSidebarMode];
    if (!_sidebarVisible) {
        [self setSidebarVisible:YES];
    }
}

- (IBAction)toggleSidebar:(id)sender {
    (void)sender;
    [self setSidebarVisible:!_sidebarVisible];
}

- (IBAction)showOutline:(id)sender {
    (void)sender;
    [self showSidebarMode:SumatraSidebarModeOutline];
}

- (IBAction)showThumbnails:(id)sender {
    (void)sender;
    [self showSidebarMode:SumatraSidebarModeThumbnails];
}

- (BOOL)splitView:(NSSplitView*)splitView canCollapseSubview:(NSView*)subview {
    (void)splitView;
    return subview == [_sidebar view];
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMinCoordinate:(CGFloat)proposedMinimumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    (void)splitView;
    return dividerIndex == 0 ? kSidebarMinWidth : proposedMinimumPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMaxCoordinate:(CGFloat)proposedMaximumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    if (dividerIndex != 0) {
        return proposedMaximumPosition;
    }
    return MAX(kSidebarMinWidth, MIN(kSidebarMaxWidth, NSWidth([splitView bounds]) - kDocumentMinWidth));
}

// Window resizes go to the document, not the sidebar.
- (BOOL)splitView:(NSSplitView*)splitView shouldAdjustSizeOfSubview:(NSView*)view {
    (void)splitView;
    return view != [_sidebar view];
}

// Tracks the divider: dragging it collapses or re-expands the sidebar.
- (void)splitViewDidResizeSubviews:(NSNotification*)notification {
    (void)notification;
    if (_adjustingSidebar) {
        return;
    }
    NSView* view = [_sidebar view];
    BOOL collapsed = [view isHidden] || [_splitView isSubviewCollapsed:view];
    if (collapsed != !_sidebarVisible) {
        _sidebarVisible = !collapsed;
        [self saveDefault:[NSNumber numberWithBool:_sidebarVisible] forKey:kDefSidebarVisible];
    }
    if (collapsed) {
        return;
    }
    CGFloat width = NSWidth([view frame]);
    if (width >= kSidebarMinWidth && fabs(width - _sidebarWidth) >= 1.0) {
        _sidebarWidth = width;
        [self saveDefault:[NSNumber numberWithDouble:width] forKey:kDefSidebarWidth];
    }
}

#pragma mark - Menu actions

- (IBAction)openDocument:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:YES];
    [panel setCanChooseDirectories:NO];
    [panel setCanChooseFiles:YES];
    char* extsUtf8 = MacCopySupportedExtensions();
    NSString* exts = StringFromUtf8(extsUtf8);
    MacFreeString(extsUtf8);
    if ([exts length] > 0) {
        [panel setAllowedFileTypes:[exts componentsSeparatedByString:@";"]];
    }
    if ([panel runModal] != NSModalResponseOK) {
        return;
    }
    NSMutableArray* paths = [NSMutableArray array];
    for (NSURL* url in [panel URLs]) {
        if ([url path]) {
            [paths addObject:[url path]];
        }
    }
    [self openPaths:paths];
}

- (IBAction)openRecentItem:(id)sender {
    NSString* path = [sender representedObject];
    if ([path isKindOfClass:[NSString class]]) {
        [self openPaths:@[ path ]];
    }
}

// ⌘W closes the tab; with no tabs left it closes the window (and quits).
- (IBAction)closeTab:(id)sender {
    NSInteger idx = [self activeIndex];
    if (idx < 0) {
        [_window performClose:sender];
        return;
    }
    [self closeTabAtIndex:(int)idx];
}

- (IBAction)reopenClosedTab:(id)sender {
    (void)sender;
    if ([_closedPaths count] == 0) {
        return;
    }
    NSString* path = [[[_closedPaths lastObject] retain] autorelease];
    [_closedPaths removeLastObject];
    [self openPaths:@[ path ]];
}

- (IBAction)selectNextTab:(id)sender {
    (void)sender;
    [self selectRelativeTab:1];
}

- (IBAction)selectPreviousTab:(id)sender {
    (void)sender;
    [self selectRelativeTab:-1];
}

- (IBAction)selectTabFromMenu:(id)sender {
    NSUInteger idx = [_tabs indexOfObjectIdenticalTo:[sender representedObject]];
    if (idx != NSNotFound) {
        [self activateTabAtIndex:(int)idx];
    }
}

- (IBAction)showInFinder:(id)sender {
    (void)sender;
    if (_active.path) {
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:_active.path] ]];
    }
}

- (IBAction)showProperties:(id)sender {
    (void)sender;
    SumatraTabState* tab = _active;
    if (!tab) {
        return;
    }
    NSMutableString* text = [NSMutableString string];
    [text appendFormat:@"File: %@\nPages: %d\n", [tab.path stringByAbbreviatingWithTildeInPath], tab.pageCount];
    int count = MacPropertyCount(tab.document);
    for (int i = 0; i < count; i++) {
        char* nameUtf8 = MacCopyPropertyName(tab.document, i);
        char* valueUtf8 = MacCopyPropertyValue(tab.document, i);
        NSString* name = StringFromUtf8(nameUtf8);
        NSString* value = StringFromUtf8(valueUtf8);
        MacFreeString(nameUtf8);
        MacFreeString(valueUtf8);
        [text appendFormat:@"%@: %@\n", name ?: @"", value ?: @""];
    }

    NSScrollView* scroll = [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 520, 280)] autorelease];
    [scroll setHasVerticalScroller:YES];
    [scroll setBorderType:NSBezelBorder];
    NSSize size = [scroll contentSize];
    NSTextView* textView = [[[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)] autorelease];
    [textView setMinSize:NSMakeSize(0, size.height)];
    [textView setMaxSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
    [textView setVerticallyResizable:YES];
    [textView setHorizontallyResizable:NO];
    [textView setAutoresizingMask:NSViewWidthSizable];
    [[textView textContainer] setContainerSize:NSMakeSize(size.width, CGFLOAT_MAX)];
    [[textView textContainer] setWidthTracksTextView:YES];
    [textView setString:text];
    [textView setEditable:NO];
    [textView setSelectable:YES];
    [textView setFont:[NSFont systemFontOfSize:12]];
    [textView setAccessibilityLabel:@"Document properties"];
    [scroll setDocumentView:textView];

    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:[tab.path lastPathComponent] ?: @"Document Properties"];
    [alert setInformativeText:@"Document properties"];
    [alert addButtonWithTitle:@"OK"];
    [alert setAccessoryView:scroll];
    [self presentAlert:alert completion:nil];
}

// Fit-to-paper printing through the engine's print render path. App-modal;
// the document can't be closed or reloaded until it finishes.
- (NSPrintOperation*)printOperationWithInfo:(NSPrintInfo*)info {
    SumatraTabState* tab = _active;
    SumatraPrintView* view = [[[SumatraPrintView alloc] initWithDocument:tab.document
                                                               pageCount:tab.pageCount
                                                                rotation:tab.rotation] autorelease];
    [info setTopMargin:kPrintMargin];
    [info setBottomMargin:kPrintMargin];
    [info setLeftMargin:kPrintMargin];
    [info setRightMargin:kPrintMargin];
    NSPrintOperation* operation = [NSPrintOperation printOperationWithView:view printInfo:info];
    [operation setJobTitle:[tab.path lastPathComponent]];
    return operation;
}

- (BOOL)runPrintOperation:(NSPrintOperation*)operation {
    _printingDocument = _active.document;
    BOOL ok = [operation runOperation];
    _printingDocument = nullptr;
    return ok;
}

- (IBAction)printDocument:(id)sender {
    (void)sender;
    if (!_active.document || _printingDocument) {
        return;
    }
    NSPrintInfo* info = [[[NSPrintInfo sharedPrintInfo] copy] autorelease];
    NSPrintOperation* operation = [self printOperationWithInfo:info];
    [operation setShowsPrintPanel:YES];
    [operation setShowsProgressPanel:YES];
    [self runPrintOperation:operation];
}

- (IBAction)copy:(id)sender {
    (void)sender;
    char* textUtf8 = MacCopySelectionText([self documentHandle]);
    NSString* text = StringFromUtf8(textUtf8);
    MacFreeString(textUtf8);
    if ([text length] == 0) {
        return;
    }
    NSPasteboard* pasteboard = [self pasteboard];
    [pasteboard clearContents];
    [pasteboard setString:text forType:NSPasteboardTypeString];
}

// The self-test copies to a private pasteboard, not the user's clipboard.
- (NSPasteboard*)pasteboard {
    if (_selfTest) {
        return [NSPasteboard pasteboardWithName:kSelfTestPasteboard];
    }
    return [NSPasteboard generalPasteboard];
}

// Long documents: the page text is extracted on a worker first (progress in
// the window subtitle, Esc cancels), so the UI doesn't stall.
- (IBAction)selectAll:(id)sender {
    (void)sender;
    SumatraTabState* tab = [self loadedTab];
    if (!tab || tab.selectAllToken != 0) {
        return;
    }
    if (tab.pageCount > kSelectAllSyncPages) {
        int token = MacPrepareTextStart(tab.document, TextPrepared, self);
        if (token != 0) {
            tab.selectAllToken = token;
            tab.selectAllPercent = 0;
            [self setFindBusy:YES];
            [self updatePageControls];
            return;
        }
    }
    MacSelectAll(tab.document);
    [self updateLayout];
}

- (void)textPreparedForDocument:(void*)document token:(int)token done:(int)done total:(int)total {
    SumatraTabState* tab = [self loadedTab];
    if (!tab || tab.document != document || tab.selectAllToken != token) {
        return;
    }
    if (done < total) {
        tab.selectAllPercent = total > 0 ? (int)((long long)done * 100 / total) : 0;
        [self updatePageControls];
        return;
    }
    tab.selectAllToken = 0;
    [self setFindBusy:tab.findToken != 0];
    MacSelectAll(tab.document);
    [self updateLayout];
    [self updatePageControls];
}

- (void)stopSelectAll:(SumatraTabState*)tab {
    if (tab.selectAllToken == 0) {
        return;
    }
    tab.selectAllToken = 0;
    MacPrepareTextCancel(tab.document);
    [self setFindBusy:tab.findToken != 0];
    [self updatePageControls];
}

- (IBAction)goToNextPage:(id)sender {
    (void)sender;
    if (_active && _active.currentPage < _active.pageCount) {
        [self showPage:_active.currentPage + 1 position:PagePosition::Top];
    }
}

- (IBAction)goToPrevPage:(id)sender {
    (void)sender;
    if (_active && _active.currentPage > 1) {
        [self showPage:_active.currentPage - 1 position:PagePosition::Top];
    }
}

- (IBAction)goToFirstPage:(id)sender {
    (void)sender;
    [self goToPage:1];
}

- (IBAction)goToLastPage:(id)sender {
    (void)sender;
    if (_active) {
        [self goToPage:_active.pageCount];
    }
}

- (IBAction)goToPageDialog:(id)sender {
    (void)sender;
    SumatraTabState* tab = _active;
    if (!tab) {
        return;
    }
    if ([_toolbar isVisible] && [_pageField window] == _window) {
        [_window makeFirstResponder:_pageField];
        [_pageField selectText:nil];
        return;
    }
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:[NSString stringWithFormat:@"Go to page (1–%d):", tab.pageCount]];
    [alert addButtonWithTitle:@"Go"];
    [alert addButtonWithTitle:@"Cancel"];
    NSTextField* input = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 24)] autorelease];
    [input setStringValue:[NSString stringWithFormat:@"%d", tab.currentPage]];
    [input setAccessibilityLabel:@"Page number"];
    [alert setAccessoryView:input];
    [[alert window] setInitialFirstResponder:input];
    [self presentAlert:alert
            completion:^(NSModalResponse response) {
              int pageNo = [input intValue];
              if (response == NSAlertFirstButtonReturn && _active && pageNo >= 1 && pageNo <= _active.pageCount) {
                  [self goToPage:pageNo];
              }
            }];
}

- (IBAction)goBack:(id)sender {
    (void)sender;
    SumatraTabState* tab = _active;
    if (!tab || tab.historyIndex <= 0 || tab.historyIndex >= (int)[tab.history count]) {
        return;
    }
    [tab.history replaceObjectAtIndex:(NSUInteger)tab.historyIndex withObject:[NSNumber numberWithInt:tab.currentPage]];
    tab.historyIndex = tab.historyIndex - 1;
    [self showPage:[[tab.history objectAtIndex:(NSUInteger)tab.historyIndex] intValue] position:PagePosition::Top];
}

- (IBAction)goForward:(id)sender {
    (void)sender;
    SumatraTabState* tab = _active;
    if (!tab || tab.historyIndex + 1 >= (int)[tab.history count]) {
        return;
    }
    [tab.history replaceObjectAtIndex:(NSUInteger)tab.historyIndex withObject:[NSNumber numberWithInt:tab.currentPage]];
    tab.historyIndex = tab.historyIndex + 1;
    [self showPage:[[tab.history objectAtIndex:(NSUInteger)tab.historyIndex] intValue] position:PagePosition::Top];
}

- (IBAction)toggleFavorite:(id)sender {
    (void)sender;
    SumatraTabState* tab = _active;
    if (!tab.path || tab.currentPage < 1) {
        return;
    }
    const char* path = FsPath(tab.path);
    if (MacPrefsHasFavorite(path, tab.currentPage)) {
        MacPrefsRemoveFavorite(path, tab.currentPage);
    } else {
        MacPrefsAddFavorite(path, tab.currentPage);
    }
}

- (IBAction)openFavorite:(id)sender {
    NSArray* target = [sender representedObject];
    if (![target isKindOfClass:[NSArray class]] || [target count] != 2) {
        return;
    }
    NSString* path = [target objectAtIndex:0];
    int pageNo = [[target objectAtIndex:1] intValue];
    if (!_active || ![_active.canonicalPath isEqualToString:CanonicalPath(path)]) {
        if (![self openPath:path]) {
            return;
        }
    }
    if (_active.document) {
        [self goToPage:pageNo];
    } else {
        _active.requestedPage = pageNo;
    }
}

- (IBAction)showCommandPalette:(id)sender {
    (void)sender;
    NSMenuItem* item = SumatraRunCommandPalette(_window);
    if (!item || ![item action]) {
        return;
    }
    [_window makeKeyAndOrderFront:nil];
    [NSApp sendAction:[item action] to:[item target] from:item];
}

- (IBAction)showKeyboardShortcuts:(id)sender {
    (void)sender;
    SumatraShowKeyboardShortcuts();
}

- (IBAction)openWebsite:(id)sender {
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:kWebsiteURL]];
}

- (IBAction)openManual:(id)sender {
    (void)sender;
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:kManualURL]];
}

#pragma mark - Validation

- (BOOL)canPerformAction:(SEL)action {
    SumatraTabState* tab = _active;
    BOOL has = tab.document != nullptr;
    if (action == @selector(goToPrevPage:) || action == @selector(goToFirstPage:)) {
        return has && tab.currentPage > 1;
    }
    if (action == @selector(goToNextPage:) || action == @selector(goToLastPage:)) {
        return has && tab.currentPage < tab.pageCount;
    }
    if (action == @selector(goBack:)) {
        return has && tab.historyIndex > 0 && tab.historyIndex < (int)[tab.history count];
    }
    if (action == @selector(goForward:)) {
        return has && tab.historyIndex + 1 < (int)[tab.history count];
    }
    if (action == @selector(copy:) || action == @selector(useSelectionForFind:)) {
        return has && MacHasSelection(tab.document);
    }
    if (action == @selector(reopenClosedTab:)) {
        return [_closedPaths count] > 0;
    }
    if (action == @selector(selectNextTab:) || action == @selector(selectPreviousTab:)) {
        return [_tabs count] > 1;
    }
    if (action == @selector(zoomIn:)) {
        return has && [self displayZoom] < kZoomMax - 0.001;
    }
    if (action == @selector(zoomOut:)) {
        return has && [self displayZoom] > kZoomMin + 0.001;
    }
    if (action == @selector(printDocument:)) {
        return has && !_printingDocument;
    }
    if (action == @selector(showInFinder:) || action == @selector(showProperties:) ||
        action == @selector(goToPageDialog:) || action == @selector(rotateLeft:) || action == @selector(rotateRight:) ||
        action == @selector(zoomActualSize:) || action == @selector(zoomFitPage:) ||
        action == @selector(zoomFitWidth:) || action == @selector(setSinglePageView:) ||
        action == @selector(setContinuousPageView:) || action == @selector(findDocument:) ||
        action == @selector(findNext:) || action == @selector(findPrevious:) || action == @selector(selectAll:) ||
        action == @selector(toggleFavorite:)) {
        return has;
    }
    return YES;
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    SEL action = [item action];
    SumatraTabState* tab = _active;
    NSControlStateValue on = NSControlStateValueOn;
    NSControlStateValue off = NSControlStateValueOff;
    if (action == @selector(setSinglePageView:)) {
        [item setState:tab && !tab.continuous ? on : off];
    } else if (action == @selector(setContinuousPageView:)) {
        [item setState:tab && tab.continuous ? on : off];
    } else if (action == @selector(toggleSidebar:)) {
        [item setTitle:_sidebarVisible ? @"Hide Sidebar" : @"Show Sidebar"];
    } else if (action == @selector(showOutline:)) {
        [item setState:_sidebarVisible && [_sidebar mode] == SumatraSidebarModeOutline ? on : off];
    } else if (action == @selector(showThumbnails:)) {
        [item setState:_sidebarVisible && [_sidebar mode] == SumatraSidebarModeThumbnails ? on : off];
    } else if (action == @selector(toggleFavorite:)) {
        BOOL favorite = tab.path && MacPrefsHasFavorite(FsPath(tab.path), tab.currentPage);
        [item setTitle:favorite ? @"Remove Bookmark" : @"Add Bookmark"];
    } else if (action == @selector(selectTabFromMenu:)) {
        [item setState:[item representedObject] == tab ? on : off];
        return YES;
    } else if (action == @selector(openRecentItem:) || action == @selector(openFavorite:)) {
        return YES;
    }
    return [self canPerformAction:action];
}

#pragma mark - Menus

- (void)rebuildRecentMenu:(NSMenu*)menu {
    [menu removeAllItems];
    int count = MacPrefsRecentCount();
    for (int i = 0; i < count; i++) {
        char* pathFs = MacPrefsCopyRecentPath(i);
        NSString* path = StringFromFs(pathFs);
        MacFreeString(pathFs);
        if (!path) {
            continue;
        }
        NSMenuItem* item = AddItem(menu, [path lastPathComponent], @selector(openRecentItem:), self, @"", 0);
        [item setRepresentedObject:path];
        [item setToolTip:[path stringByAbbreviatingWithTildeInPath]];
    }
    if ([[menu itemArray] count] == 0) {
        NSMenuItem* none = AddItem(menu, @"No Recent Documents", nil, nil, @"", 0);
        [none setEnabled:NO];
    }
}

- (void)rebuildBookmarksMenu:(NSMenu*)menu {
    [menu removeAllItems];
    int count = MacPrefsFavoriteCount();
    for (int i = 0; i < count; i++) {
        char* pathFs = MacPrefsCopyFavoritePath(i);
        NSString* path = StringFromFs(pathFs);
        MacFreeString(pathFs);
        int pageNo = MacPrefsFavoritePage(i);
        if (!path || pageNo < 1) {
            continue;
        }
        NSString* title = [NSString stringWithFormat:@"%@ — page %d", [path lastPathComponent], pageNo];
        NSMenuItem* item = AddItem(menu, title, @selector(openFavorite:), self, @"", 0);
        [item setRepresentedObject:@[ path, [NSNumber numberWithInt:pageNo] ]];
        [item setToolTip:[path stringByAbbreviatingWithTildeInPath]];
    }
    if ([[menu itemArray] count] == 0) {
        NSMenuItem* none = AddItem(menu, @"No Bookmarks", nil, nil, @"", 0);
        [none setEnabled:NO];
    }
}

// Lists the open documents after "Show Previous Tab" in the Window menu.
- (void)rebuildWindowMenu:(NSMenu*)menu {
    for (NSMenuItem* item in [[[menu itemArray] copy] autorelease]) {
        if ([item action] == @selector(selectTabFromMenu:) || [item tag] == kTabMenuSeparatorTag) {
            [menu removeItem:item];
        }
    }
    NSInteger anchor = [menu indexOfItemWithTarget:self andAction:@selector(selectPreviousTab:)];
    if (anchor < 0 || [_tabs count] == 0) {
        return;
    }
    NSInteger idx = anchor + 1;
    NSMenuItem* separator = [NSMenuItem separatorItem];
    [separator setTag:kTabMenuSeparatorTag];
    [menu insertItem:separator atIndex:idx++];
    for (SumatraTabState* tab in _tabs) {
        NSString* title = [tab.path lastPathComponent] ?: @"Document";
        NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:title
                                                       action:@selector(selectTabFromMenu:)
                                                keyEquivalent:@""] autorelease];
        [item setTarget:self];
        [item setRepresentedObject:tab];
        [menu insertItem:item atIndex:idx++];
    }
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu == _recentMenu) {
        [self rebuildRecentMenu:menu];
    } else if (menu == _bookmarksMenu) {
        [self rebuildBookmarksMenu:menu];
    } else if (menu == _windowMenu) {
        [self rebuildWindowMenu:menu];
    }
}

// Menus follow macOS conventions and Preview's shortcuts; keep
// kShortcutRows (MacPanels.mm) and docs/mac/keyboard-shortcuts.md in sync.
- (void)installMainMenu {
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    const NSEventModifierFlags shiftCmd = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    const NSEventModifierFlags optCmd = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    const NSEventModifierFlags ctrlCmd = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    NSMenu* mainMenu = [[[NSMenu alloc] initWithTitle:@""] autorelease];

    NSMenu* appMenu = AddSubmenu(mainMenu, @"SumatraPDF");
    AddItem(appMenu, @"About SumatraPDF", @selector(orderFrontStandardAboutPanel:), nil, @"", 0);
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenu* servicesMenu = AddSubmenu(appMenu, @"Services");
    [NSApp setServicesMenu:servicesMenu];
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddItem(appMenu, @"Hide SumatraPDF", @selector(hide:), nil, @"h", cmd);
    AddItem(appMenu, @"Hide Others", @selector(hideOtherApplications:), nil, @"h", optCmd);
    AddItem(appMenu, @"Show All", @selector(unhideAllApplications:), nil, @"", 0);
    [appMenu addItem:[NSMenuItem separatorItem]];
    AddItem(appMenu, @"Quit SumatraPDF", @selector(terminate:), nil, @"q", cmd);

    NSMenu* fileMenu = AddSubmenu(mainMenu, @"File");
    AddItem(fileMenu, @"Open…", @selector(openDocument:), self, @"o", cmd);
    _recentMenu = AddSubmenu(fileMenu, @"Open Recent");
    [_recentMenu setDelegate:self];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    AddItem(fileMenu, @"Close Tab", @selector(closeTab:), self, @"w", cmd);
    AddItem(fileMenu, @"Close Window", @selector(performClose:), nil, @"w", shiftCmd);
    AddItem(fileMenu, @"Reopen Closed Tab", @selector(reopenClosedTab:), self, @"t", shiftCmd);
    [fileMenu addItem:[NSMenuItem separatorItem]];
    AddItem(fileMenu, @"Show in Finder", @selector(showInFinder:), self, @"", 0);
    AddItem(fileMenu, @"Properties", @selector(showProperties:), self, @"i", cmd);
    [fileMenu addItem:[NSMenuItem separatorItem]];
    AddItem(fileMenu, @"Page Setup…", @selector(runPageLayout:), nil, @"p", shiftCmd);
    AddItem(fileMenu, @"Print…", @selector(printDocument:), self, @"p", cmd);

    // standard actions (nil target) so text fields get cut/copy/paste/undo
    NSMenu* editMenu = AddSubmenu(mainMenu, @"Edit");
    AddItem(editMenu, @"Undo", @selector(undo:), nil, @"z", cmd);
    AddItem(editMenu, @"Redo", @selector(redo:), nil, @"z", shiftCmd);
    [editMenu addItem:[NSMenuItem separatorItem]];
    AddItem(editMenu, @"Cut", @selector(cut:), nil, @"x", cmd);
    AddItem(editMenu, @"Copy", @selector(copy:), nil, @"c", cmd);
    AddItem(editMenu, @"Paste", @selector(paste:), nil, @"v", cmd);
    AddItem(editMenu, @"Select All", @selector(selectAll:), nil, @"a", cmd);
    [editMenu addItem:[NSMenuItem separatorItem]];
    NSMenu* findMenu = AddSubmenu(editMenu, @"Find");
    AddItem(findMenu, @"Find…", @selector(findDocument:), self, @"f", cmd);
    AddItem(findMenu, @"Find Next", @selector(findNext:), self, @"g", cmd);
    AddItem(findMenu, @"Find Previous", @selector(findPrevious:), self, @"g", shiftCmd);
    AddItem(findMenu, @"Use Selection for Find", @selector(useSelectionForFind:), self, @"e", cmd);

    NSMenu* viewMenu = AddSubmenu(mainMenu, @"View");
    AddItem(viewMenu, @"Show Sidebar", @selector(toggleSidebar:), self, @"s", optCmd);
    AddItem(viewMenu, @"Table of Contents", @selector(showOutline:), self, @"3", optCmd);
    AddItem(viewMenu, @"Thumbnails", @selector(showThumbnails:), self, @"2", optCmd);
    [viewMenu addItem:[NSMenuItem separatorItem]];
    AddItem(viewMenu, @"Single Page", @selector(setSinglePageView:), self, @"", 0);
    AddItem(viewMenu, @"Continuous Scroll", @selector(setContinuousPageView:), self, @"", 0);
    [viewMenu addItem:[NSMenuItem separatorItem]];
    AddItem(viewMenu, @"Actual Size", @selector(zoomActualSize:), self, @"0", cmd);
    AddItem(viewMenu, @"Zoom to Fit", @selector(zoomFitPage:), self, @"9", cmd);
    AddItem(viewMenu, @"Zoom to Width", @selector(zoomFitWidth:), self, @"8", cmd);
    AddItem(viewMenu, @"Zoom In", @selector(zoomIn:), self, @"+", cmd);
    AddItem(viewMenu, @"Zoom Out", @selector(zoomOut:), self, @"-", cmd);
    [viewMenu addItem:[NSMenuItem separatorItem]];
    AddItem(viewMenu, @"Rotate Left", @selector(rotateLeft:), self, @"l", cmd);
    AddItem(viewMenu, @"Rotate Right", @selector(rotateRight:), self, @"r", cmd);
    [viewMenu addItem:[NSMenuItem separatorItem]];
    AddItem(viewMenu, @"Show Toolbar", @selector(toggleToolbarShown:), nil, @"t", optCmd);
    AddItem(viewMenu, @"Customize Toolbar…", @selector(runToolbarCustomizationPalette:), nil, @"", 0);
    AddItem(viewMenu, @"Command Palette…", @selector(showCommandPalette:), self, @"k", cmd);
    [viewMenu addItem:[NSMenuItem separatorItem]];
    AddItem(viewMenu, @"Enter Full Screen", @selector(toggleFullScreen:), nil, @"f", ctrlCmd);

    NSMenu* goMenu = AddSubmenu(mainMenu, @"Go");
    AddItem(goMenu, @"Previous Page", @selector(goToPrevPage:), self, KeyString(NSUpArrowFunctionKey), optCmd);
    AddItem(goMenu, @"Next Page", @selector(goToNextPage:), self, KeyString(NSDownArrowFunctionKey), optCmd);
    AddItem(goMenu, @"First Page", @selector(goToFirstPage:), self, KeyString(NSUpArrowFunctionKey), cmd);
    AddItem(goMenu, @"Last Page", @selector(goToLastPage:), self, KeyString(NSDownArrowFunctionKey), cmd);
    AddItem(goMenu, @"Go to Page…", @selector(goToPageDialog:), self, @"g", optCmd);
    [goMenu addItem:[NSMenuItem separatorItem]];
    AddItem(goMenu, @"Back", @selector(goBack:), self, @"[", cmd);
    AddItem(goMenu, @"Forward", @selector(goForward:), self, @"]", cmd);
    [goMenu addItem:[NSMenuItem separatorItem]];
    AddItem(goMenu, @"Add Bookmark", @selector(toggleFavorite:), self, @"d", cmd);
    _bookmarksMenu = AddSubmenu(goMenu, @"Bookmarks");
    [_bookmarksMenu setDelegate:self];

    _windowMenu = AddSubmenu(mainMenu, @"Window");
    [_windowMenu setDelegate:self];
    AddItem(_windowMenu, @"Minimize", @selector(performMiniaturize:), nil, @"m", cmd);
    AddItem(_windowMenu, @"Zoom", @selector(performZoom:), nil, @"", 0);
    [_windowMenu addItem:[NSMenuItem separatorItem]];
    // "}" / "{" are ⇧] / ⇧[ on US layouts; ⌃⇥ is handled by the key monitor
    AddItem(_windowMenu, @"Show Next Tab", @selector(selectNextTab:), self, @"}", cmd);
    AddItem(_windowMenu, @"Show Previous Tab", @selector(selectPreviousTab:), self, @"{", cmd);
    [_windowMenu addItem:[NSMenuItem separatorItem]];
    AddItem(_windowMenu, @"Bring All to Front", @selector(arrangeInFront:), nil, @"", 0);
    [NSApp setWindowsMenu:_windowMenu];

    NSMenu* helpMenu = AddSubmenu(mainMenu, @"Help");
    AddItem(helpMenu, @"SumatraPDF Help", @selector(openManual:), self, @"", 0);
    AddItem(helpMenu, @"Keyboard Shortcuts", @selector(showKeyboardShortcuts:), self, @"", 0);
    [helpMenu addItem:[NSMenuItem separatorItem]];
    AddItem(helpMenu, @"SumatraPDF Website", @selector(openWebsite:), self, @"", 0);
    [NSApp setHelpMenu:helpMenu];

    [NSApp setMainMenu:mainMenu];
}

@end

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];

    SumatraAppDelegate* delegate = [[SumatraAppDelegate alloc] init];
    [app setDelegate:delegate];
    [delegate installMainMenu];
    [app run];

    MacShutdown();
    [delegate release];
    [pool release];
    MacFinalize();
    return 0;
}
