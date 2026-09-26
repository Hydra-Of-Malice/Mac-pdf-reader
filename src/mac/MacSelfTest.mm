/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Self-test mode: runs inside the app on a real Mac (e.g. a CI runner, where
// UI scripting has no Accessibility permission). Documents are opened the way
// Finder opens them (-openPaths:) and commands are sent through the main menu
// items, so validation and the responder chain are exercised too. Waiting
// pumps the run loop, so renders, find results and thumbnails keep arriving.
// Start it from a run loop timer, never from a main-queue block: blocks posted
// to the main queue don't run while another main-queue block is running.

#import <Cocoa/Cocoa.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mac/SumatraMacEngine.h"
#import "mac/MacDocumentView.h"
#import "mac/MacSelfTest.h"

static const double kDefaultWaitSeconds = 30.0;
static const double kPollSeconds = 0.02;
static const double kSettleSeconds = 0.3;
static const int kMaxPasswords = 8;
static const int kLinkScanPages = 8;
static const double kLinkScanSteps = 128.0;
static const double kScrollTolerance = 3.0;
static const CGFloat kScrollProbe = 60.0;
static const int kMaxSnapshotSamples = 250;
static const NSUInteger kWhiteLevel = 230;

// "case/step" being run, printed by the watchdog
static char gCurrentStep[256];

static double Now() {
    return CFAbsoluteTimeGetCurrent();
}

static NSString* CanonicalTestPath(NSString* path) {
    return [[path stringByStandardizingPath] stringByResolvingSymlinksInPath];
}

static BOOL SamePath(NSString* a, NSString* b) {
    return a && b && [CanonicalTestPath(a) isEqualToString:CanonicalTestPath(b)];
}

static NSString* StringValue(id value) {
    return [value isKindOfClass:[NSString class]] ? (NSString*)value : nil;
}

static BOOL HasNumber(NSDictionary* dict, NSString* key) {
    return [[dict objectForKey:key] isKindOfClass:[NSNumber class]];
}

static NSString* SafeFileName(NSString* name) {
    NSMutableString* s = [NSMutableString string];
    for (NSUInteger i = 0; i < [name length]; i++) {
        unichar c = [name characterAtIndex:i];
        BOOL ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        [s appendFormat:@"%C", ok ? c : (unichar)'_'];
    }
    return s;
}

static NSMenuItem* FindMenuItem(NSMenu* menu, SEL action) {
    for (NSMenuItem* item in [menu itemArray]) {
        if ([item action] == action) {
            return item;
        }
        if ([item hasSubmenu]) {
            NSMenuItem* found = FindMenuItem([item submenu], action);
            if (found) {
                return found;
            }
        }
    }
    return nil;
}

static const char* OpenErrorName(int err) {
    switch ((MacOpenError)err) {
        case MacOpenError::None:
            return "none";
        case MacOpenError::NotFound:
            return "not found";
        case MacOpenError::Unreadable:
            return "unreadable";
        case MacOpenError::Unsupported:
            return "unsupported";
        case MacOpenError::PasswordCancelled:
            return "password cancelled";
        case MacOpenError::Damaged:
            return "damaged";
        case MacOpenError::RendererFailed:
            return "renderer failed";
    }
    return "?";
}

static const char* SelfTestPassword(void* context, const char* fileName, int attempt) {
    (void)fileName;
    return [(SumatraSelfTest*)context passwordForAttempt:attempt];
}

@implementation SumatraSelfTest {
    id<SumatraSelfTestHost> _host; // the app delegate, lives for the whole process
    NSString* _reportPath;
    NSString* _outDir;
    NSString* _manifestPath;
    NSArray* _paths;
    NSString* _findWord;
    double _timeout;
    double _startTime;
    dispatch_source_t _watchdog;

    NSMutableDictionary* _report;
    NSMutableArray* _results;
    NSMutableArray* _ignored;
    NSMutableDictionary* _case; // result of the running case
    NSMutableArray* _steps;
    NSMutableArray* _alerts;
    NSString* _caseId;
    BOOL _caseOk;
    double _waitSeconds;

    char* _passwords[kMaxPasswords];
    int _passwordCount;
    int _prompts; // written by the loader thread
}

- (instancetype)initWithHost:(id<SumatraSelfTestHost>)host
                  reportPath:(NSString*)reportPath
                manifestPath:(NSString*)manifestPath
                       paths:(NSArray*)paths
                    findWord:(NSString*)findWord
                     timeout:(double)timeoutSeconds {
    self = [super init];
    if (!self) {
        return nil;
    }
    _host = host;
    _reportPath = [[reportPath stringByStandardizingPath] copy];
    _outDir = [[_reportPath stringByDeletingLastPathComponent] copy];
    _manifestPath = [manifestPath copy];
    _paths = [paths copy];
    _findWord = [findWord copy];
    _timeout = timeoutSeconds;
    _report = [[NSMutableDictionary alloc] init];
    _results = [[NSMutableArray alloc] init];
    _ignored = [[NSMutableArray alloc] init];
    return self;
}

- (void)dealloc {
    [self setPasswords:nil];
    if (_watchdog) {
        dispatch_source_cancel(_watchdog);
        dispatch_release(_watchdog);
    }
    [_reportPath release];
    [_outDir release];
    [_manifestPath release];
    [_paths release];
    [_findWord release];
    [_report release];
    [_results release];
    [_ignored release];
    [_case release];
    [_steps release];
    [_alerts release];
    [_caseId release];
    [super dealloc];
}

#pragma mark - Hooks called by the app

- (void)recordAlert:(NSString*)message info:(NSString*)info {
    NSString* text = [info length] > 0 ? [NSString stringWithFormat:@"%@ %@", message ?: @"", info] : (message ?: @"");
    fprintf(stderr, "selftest:   alert: %s\n", [text UTF8String]);
    [_alerts addObject:text];
}

- (void)recordIgnored:(NSString*)what {
    [_ignored addObject:what ?: @""];
}

// Called on the thread that opens the document.
- (const char*)passwordForAttempt:(int)attempt {
    __atomic_add_fetch(&_prompts, 1, __ATOMIC_SEQ_CST);
    if (attempt < 1 || attempt > _passwordCount) {
        return nullptr;
    }
    return _passwords[attempt - 1];
}

- (void)setPasswords:(NSString*)list {
    for (int i = 0; i < _passwordCount; i++) {
        free(_passwords[i]);
        _passwords[i] = nullptr;
    }
    _passwordCount = 0;
    __atomic_store_n(&_prompts, 0, __ATOMIC_SEQ_CST);
    for (NSString* password in [list componentsSeparatedByString:@","]) {
        if (_passwordCount < kMaxPasswords) {
            _passwords[_passwordCount++] = strdup([password UTF8String]);
        }
    }
}

#pragma mark - Plumbing

// Aborts a run that hangs (e.g. the main thread is blocked); the report on disk
// then says "finished": false.
- (void)startWatchdog {
    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    _watchdog = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    if (!_watchdog) {
        return;
    }
    int64_t delay = (int64_t)(_timeout * (double)NSEC_PER_SEC);
    dispatch_source_set_timer(_watchdog, dispatch_time(DISPATCH_TIME_NOW, delay), DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
    double timeout = _timeout;
    dispatch_source_set_event_handler(_watchdog, ^{
      fprintf(stderr, "selftest: watchdog: no result after %.0f s, stuck in %s\n", timeout, gCurrentStep);
      fflush(stderr);
      _exit(3);
    });
    dispatch_resume(_watchdog);
}

// Pumps events and the run loop until cond() is true or the time is up.
- (BOOL)waitFor:(BOOL (^)(void))cond seconds:(double)seconds {
    double deadline = Now() + seconds;
    while (!cond()) {
        if (Now() >= deadline) {
            return cond();
        }
        @autoreleasepool {
            NSDate* until = [NSDate dateWithTimeIntervalSinceNow:kPollSeconds];
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:until
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event) {
                [NSApp sendEvent:event];
            }
            [[_host selfTestWindow] displayIfNeeded];
        }
    }
    return YES;
}

- (void)settle {
    [self waitFor:^BOOL {
      return NO;
    }
          seconds:kSettleSeconds];
}

- (struct SumatraTestState)state {
    return [_host selfTestState];
}

- (void)beginStep:(NSString*)name {
    snprintf(gCurrentStep, sizeof(gCurrentStep), "%s/%s", [_caseId UTF8String], [name UTF8String]);
}

- (void)step:(NSString*)name ok:(BOOL)ok since:(double)t0 detail:(NSString*)detail {
    double ms = (Now() - t0) * 1000.0;
    NSMutableDictionary* step = [NSMutableDictionary dictionary];
    [step setObject:name forKey:@"name"];
    [step setObject:[NSNumber numberWithBool:ok] forKey:@"ok"];
    [step setObject:[NSNumber numberWithDouble:round(ms)] forKey:@"ms"];
    if ([detail length] > 0) {
        [step setObject:detail forKey:@"detail"];
    }
    [_steps addObject:step];
    if (!ok) {
        _caseOk = NO;
    }
    fprintf(stderr, "selftest:   %s %6.0f ms  %s  %s\n", ok ? "ok  " : "FAIL", ms, [name UTF8String],
            [detail UTF8String] ?: "");
}

- (void)skip:(NSString*)name detail:(NSString*)detail {
    NSMutableDictionary* step = [NSMutableDictionary dictionary];
    [step setObject:name forKey:@"name"];
    [step setObject:[NSNumber numberWithBool:YES] forKey:@"ok"];
    [step setObject:[NSNumber numberWithBool:YES] forKey:@"skipped"];
    [step setObject:[NSNumber numberWithDouble:0] forKey:@"ms"];
    [step setObject:detail ?: @"" forKey:@"detail"];
    [_steps addObject:step];
    fprintf(stderr, "selftest:   skip          %s  %s\n", [name UTF8String], [detail UTF8String] ?: "");
}

// Sends a main menu item's action the way a click does: validate, then perform.
// Returns nil, or why it couldn't.
- (NSString*)invoke:(SEL)action {
    NSMenuItem* item = FindMenuItem([NSApp mainMenu], action);
    if (!item) {
        return [NSString stringWithFormat:@"no menu item for %@", NSStringFromSelector(action)];
    }
    NSMenu* menu = [item menu];
    [menu update];
    if (![item isEnabled]) {
        return [NSString stringWithFormat:@"menu item “%@” is disabled", [item title]];
    }
    [menu performActionForItemAtIndex:[menu indexOfItem:item]];
    return nil;
}

- (BOOL)waitRendered {
    return [self waitFor:^BOOL {
      struct SumatraTestState s = [self state];
      return s.hasTab && !s.loading && s.rendered;
    }
                 seconds:_waitSeconds];
}

// Runs a menu command, waits for the page(s) to be rendered again and checks the state.
- (BOOL)command:(SEL)action
           name:(NSString*)name
          check:(NSString* (^)(struct SumatraTestState s))check {
    [self beginStep:name];
    double t0 = Now();
    NSString* err = [self invoke:action];
    if (err) {
        [self step:name ok:NO since:t0 detail:err];
        return NO;
    }
    BOOL rendered = [self waitRendered];
    struct SumatraTestState s = [self state];
    NSString* problem = check ? check(s) : nil;
    if (!problem && !rendered) {
        problem = @"page not rendered in time";
    }
    NSString* detail =
        [NSString stringWithFormat:@"page %d/%d, zoom %.3g (%.0f%%), rotation %d", s.currentPage, s.pageCount, s.zoom,
                                   s.displayZoom * 100.0, s.rotation];
    if (problem) {
        detail = [NSString stringWithFormat:@"%@; %@", problem, detail];
    }
    [self step:name ok:problem == nil since:t0 detail:detail];
    return problem == nil;
}

// Types text into a text field through its field editor and presses Return.
- (void)enterText:(NSString*)text inField:(NSTextField*)field {
    NSWindow* window = [_host selfTestWindow];
    if ([field window] == window && [field currentEditor] == nil) {
        [window makeFirstResponder:field];
    }
    NSText* editor = [field currentEditor];
    if (editor) {
        [editor setString:text];
        [field validateEditing];
    } else {
        [field setStringValue:text];
    }
    [field sendAction:[field action] to:[field target]];
}

#pragma mark - Report

- (void)writeReport {
    [_report setObject:_results forKey:@"files"];
    [_report setObject:_ignored forKey:@"ignoredOpenRequests"];
    [_report setObject:[NSNumber numberWithDouble:round((Now() - _startTime) * 1000.0)] forKey:@"ms"];
    NSError* error = nil;
    NSData* json = [NSJSONSerialization dataWithJSONObject:_report options:NSJSONWritingPrettyPrinted error:&error];
    if (!json || ![json writeToFile:_reportPath atomically:YES]) {
        fprintf(stderr, "selftest: can't write %s\n", [_reportPath UTF8String]);
    }
}

- (NSString*)resolvePath:(NSString*)path root:(NSString*)root {
    if ([path isAbsolutePath]) {
        return [path stringByStandardizingPath];
    }
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* fromCwd = [[[fm currentDirectoryPath] stringByAppendingPathComponent:path] stringByStandardizingPath];
    if ([fm fileExistsAtPath:fromCwd] || !root) {
        return fromCwd;
    }
    return [[root stringByAppendingPathComponent:path] stringByStandardizingPath];
}

// Manifest fixtures (tests/mac/fixtures/manifest.json; paths relative to the
// repository) filtered by the command-line paths, then paths not in it.
- (NSArray*)buildCases {
    NSMutableArray* cases = [NSMutableArray array];
    NSMutableArray* wanted = [NSMutableArray array];
    for (NSString* path in _paths) {
        [wanted addObject:[self resolvePath:path root:nil]];
    }
    NSMutableSet* covered = [NSMutableSet set];

    if (_manifestPath) {
        NSString* manifest = [self resolvePath:_manifestPath root:nil];
        NSData* data = [NSData dataWithContentsOfFile:manifest];
        id json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        NSArray* fixtures = [json isKindOfClass:[NSDictionary class]] ? [json objectForKey:@"fixtures"] : nil;
        if (![fixtures isKindOfClass:[NSArray class]]) {
            [_report setObject:[NSString stringWithFormat:@"can't read manifest %@", manifest] forKey:@"error"];
            fixtures = nil;
        }
        // <root>/tests/mac/fixtures/manifest.json
        NSString* root = manifest;
        for (int i = 0; i < 4; i++) {
            root = [root stringByDeletingLastPathComponent];
        }
        for (NSDictionary* fixture in fixtures) {
            NSString* rel = [fixture isKindOfClass:[NSDictionary class]] ? StringValue([fixture objectForKey:@"path"]) : nil;
            if (!rel || [[fixture objectForKey:@"target"] isEqual:[NSNumber numberWithBool:NO]]) {
                continue;
            }
            NSString* path = [self resolvePath:rel root:root];
            BOOL match = [wanted count] == 0;
            for (NSString* w in wanted) {
                if (SamePath(w, path)) {
                    match = YES;
                    [covered addObject:w];
                }
            }
            if (!match) {
                continue;
            }
            NSMutableDictionary* c = [[fixture mutableCopy] autorelease];
            [c setObject:path forKey:@"absPath"];
            [cases addObject:c];
        }
    }

    for (NSString* path in wanted) {
        if ([covered containsObject:path]) {
            continue;
        }
        NSMutableDictionary* c = [NSMutableDictionary dictionary];
        [c setObject:[path lastPathComponent] forKey:@"id"];
        [c setObject:path forKey:@"path"];
        [c setObject:path forKey:@"absPath"];
        [c setObject:@"open" forKey:@"expect"];
        if ([_findWord length] > 0) {
            [c setObject:_findWord forKey:@"search"];
        }
        [cases addObject:c];
    }
    return cases;
}

#pragma mark - Run

- (void)start {
    _startTime = Now();
    snprintf(gCurrentStep, sizeof(gCurrentStep), "start");
    [self startWatchdog];
    [[NSFileManager defaultManager] createDirectoryAtPath:_outDir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    MacSetPasswordCallback(SelfTestPassword, self);

    NSArray* cases = [self buildCases];
    [_report setObject:[NSNumber numberWithBool:NO] forKey:@"finished"];
    [_report setObject:[NSNumber numberWithBool:NO] forKey:@"pass"];
    [_report setObject:[[NSProcessInfo processInfo] operatingSystemVersionString] forKey:@"os"];
    [self writeReport];
    fprintf(stderr, "selftest: %d case(s), report %s\n", (int)[cases count], [_reportPath UTF8String]);

    int failed = 0;
    int skipped = 0;
    for (NSDictionary* c in cases) {
        @autoreleasepool {
            [self runCase:c];
            if (![[_case objectForKey:@"pass"] boolValue]) {
                failed++;
            }
            if ([[_case objectForKey:@"skipped"] boolValue]) {
                skipped++;
            }
            [self writeReport];
        }
    }
    MacSetPasswordCallback(nullptr, nullptr);

    BOOL pass = failed == 0 && [cases count] > 0 && ![_report objectForKey:@"error"];
    [_report setObject:[NSNumber numberWithBool:YES] forKey:@"finished"];
    [_report setObject:[NSNumber numberWithBool:pass] forKey:@"pass"];
    [_report setObject:[NSNumber numberWithInt:(int)[cases count]] forKey:@"cases"];
    [_report setObject:[NSNumber numberWithInt:failed] forKey:@"failed"];
    [_report setObject:[NSNumber numberWithInt:skipped] forKey:@"skipped"];
    [self writeReport];
    fprintf(stderr, "selftest: %s: %d case(s), %d failed, %d skipped\n", pass ? "PASS" : "FAIL", (int)[cases count],
            failed, skipped);
    snprintf(gCurrentStep, sizeof(gCurrentStep), "shutdown");
    [_host selfTestFinished:pass ? 0 : 1];
}

- (void)runCase:(NSDictionary*)c {
    NSString* caseId = StringValue([c objectForKey:@"id"]) ?: @"case";
    NSString* path = [c objectForKey:@"absPath"];
    NSString* expect = StringValue([c objectForKey:@"expect"]) ?: @"open";
    [_caseId release];
    _caseId = [caseId copy];
    [_case release];
    _case = [[NSMutableDictionary alloc] init];
    [_steps release];
    _steps = [[NSMutableArray alloc] init];
    [_alerts release];
    _alerts = [[NSMutableArray alloc] init];
    _caseOk = YES;
    double timeoutMs = [[c objectForKey:@"timeoutMs"] doubleValue];
    _waitSeconds = timeoutMs > 0 ? timeoutMs / 1000.0 : kDefaultWaitSeconds;

    [_case setObject:caseId forKey:@"id"];
    [_case setObject:StringValue([c objectForKey:@"path"]) ?: path forKey:@"path"];
    [_case setObject:StringValue([c objectForKey:@"format"]) ?: @"" forKey:@"format"];
    [_case setObject:expect forKey:@"expect"];
    [_case setObject:_steps forKey:@"steps"];
    [_case setObject:_alerts forKey:@"alerts"];
    [_results addObject:_case];
    fprintf(stderr, "selftest: %s (%s, expect %s)\n", [caseId UTF8String], [[path lastPathComponent] UTF8String],
            [expect UTF8String]);

    double t0 = Now();
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        [self skip:@"open" detail:@"file not found"];
        [_case setObject:[NSNumber numberWithBool:YES] forKey:@"skipped"];
    } else {
        [self setPasswords:StringValue([c objectForKey:@"password"])];
        BOOL opened = [self openCase:c expect:expect];
        if (opened && [expect isEqualToString:@"open"]) {
            [self exercise:c];
        } else if (opened) {
            [self renderOnly];
        }
        [self closeAllTabs];
        [self setPasswords:nil];
    }
    [_case setObject:[NSNumber numberWithBool:_caseOk] forKey:@"pass"];
    [_case setObject:[NSNumber numberWithDouble:round((Now() - t0) * 1000.0)] forKey:@"ms"];
}

- (BOOL)openCase:(NSDictionary*)c expect:(NSString*)expect {
    NSString* path = [c objectForKey:@"absPath"];
    NSUInteger alertsBefore = [_alerts count];
    [self beginStep:@"open"];
    double t0 = Now();
    [_host openPaths:@[ path ]];
    __block BOOL opened = NO;
    BOOL settled = [self waitFor:^BOOL {
      struct SumatraTestState s = [self state];
      opened = s.hasTab && !s.loading && SamePath([_host selfTestActivePath], path);
      return opened || (!s.loading && [_alerts count] > alertsBefore);
    }
                         seconds:_waitSeconds];
    struct SumatraTestState s = [self state];
    int prompts = __atomic_load_n(&_prompts, __ATOMIC_SEQ_CST);
    BOOL alerted = [_alerts count] > alertsBefore;
    NSString* alert = alerted ? [_alerts lastObject] : @"no alert";
    NSString* detail = nil;
    BOOL ok = NO;
    if (!settled) {
        detail = @"timed out: neither opened nor reported an error";
    } else if ([expect isEqualToString:@"open"]) {
        ok = opened;
        detail = opened ? [NSString stringWithFormat:@"%d pages", s.pageCount]
                        : [NSString stringWithFormat:@"not opened (%s): %@", OpenErrorName(s.lastOpenError), alert];
    } else if ([expect isEqualToString:@"fail"]) {
        BOOL passwordCase = [[c objectForKey:@"prompts"] intValue] > 0;
        BOOL rightError = !passwordCase || s.lastOpenError == (int)MacOpenError::PasswordCancelled;
        ok = !opened && alerted && rightError;
        detail = opened ? @"opened but should fail"
                        : [NSString stringWithFormat:@"error: %s; %@", OpenErrorName(s.lastOpenError), alert];
    } else {
        ok = opened || alerted;
        detail = opened ? [NSString stringWithFormat:@"opened, %d pages", s.pageCount]
                        : [NSString stringWithFormat:@"error: %s; %@", OpenErrorName(s.lastOpenError), alert];
    }
    [self step:@"open" ok:ok since:t0 detail:detail];

    if (HasNumber(c, @"prompts")) {
        int want = [[c objectForKey:@"prompts"] intValue];
        NSString* d = [NSString stringWithFormat:@"%d password prompt(s), expected %d", prompts, want];
        [self step:@"password prompts" ok:prompts == want since:Now() detail:d];
    }
    if (!opened) {
        return NO;
    }
    if (HasNumber(c, @"pages") || HasNumber(c, @"minPages")) {
        int want = [[c objectForKey:HasNumber(c, @"pages") ? @"pages" : @"minPages"] intValue];
        BOOL exact = HasNumber(c, @"pages");
        BOOL pagesOk = exact ? s.pageCount == want : s.pageCount >= want;
        NSString* d = [NSString stringWithFormat:@"%d pages, expected %@%d", s.pageCount, exact ? @"" : @">= ", want];
        [self step:@"page count" ok:pagesOk since:Now() detail:d];
    }
    return YES;
}

// A document expected to maybe fail opened: it must render without hanging.
- (void)renderOnly {
    [self beginStep:@"render"];
    double t0 = Now();
    BOOL rendered = [self waitRendered];
    [self step:@"render" ok:YES since:t0 detail:rendered ? @"rendered" : @"not rendered (allowed)"];
}

- (void)exercise:(NSDictionary*)c {
    [self beginStep:@"render"];
    double t0 = Now();
    BOOL rendered = [self waitRendered];
    [self step:@"render first page" ok:rendered since:t0 detail:rendered ? nil : @"timed out"];
    if (!rendered) {
        return;
    }
    [self snapshot];
    [self navigation];
    [self viewModes];
    [self zoomAndRotation];
    [self sidebar];
    [self find:StringValue([c objectForKey:@"search"]) page:[[c objectForKey:@"searchPage"] intValue]];
    [self followLink];
    [self selectAndCopy:StringValue([c objectForKey:@"search"])];
    [self persistence:[c objectForKey:@"absPath"]];
}

#pragma mark - Steps

// PNG of the document view next to the report; fails if the current page is blank.
- (void)snapshot {
    [self beginStep:@"snapshot"];
    double t0 = Now();
    SumatraDocumentView* view = (SumatraDocumentView*)[_host selfTestDocumentView];
    NSRect visible = [view visibleRect];
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:visible];
    if (!rep || NSIsEmptyRect(visible)) {
        [self step:@"snapshot" ok:NO since:t0 detail:@"no bitmap"];
        return;
    }
    [view cacheDisplayInRect:visible toBitmapImageRep:rep];
    NSString* file = [SafeFileName(_caseId) stringByAppendingPathExtension:@"png"];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    BOOL written = [png writeToFile:[_outDir stringByAppendingPathComponent:file] atomically:YES];
    if (written) {
        [_case setObject:file forKey:@"snapshot"];
    }

    struct SumatraTestState s = [self state];
    SumatraPageImage* page = nil;
    for (SumatraPageImage* p in [view pages]) {
        if (!page || [p pageNo] == s.currentPage) {
            page = p;
        }
    }
    NSRect r = page ? NSIntersectionRect(NSInsetRect([page frame], 2, 2), visible) : NSZeroRect;
    int samples = 0;
    int inked = 0;
    if (!NSIsEmptyRect(r) && [rep pixelsWide] > 0) {
        // bitmap rows run top-down, like the flipped view
        double scale = (double)[rep pixelsWide] / visible.size.width;
        NSInteger x0 = (NSInteger)((r.origin.x - visible.origin.x) * scale);
        NSInteger y0 = (NSInteger)((r.origin.y - visible.origin.y) * scale);
        NSInteger x1 = MIN([rep pixelsWide], (NSInteger)((NSMaxX(r) - visible.origin.x) * scale));
        NSInteger y1 = MIN([rep pixelsHigh], (NSInteger)((NSMaxY(r) - visible.origin.y) * scale));
        NSInteger step = MAX((NSInteger)1, MAX(x1 - x0, y1 - y0) / kMaxSnapshotSamples);
        NSInteger nSamples = [rep samplesPerPixel];
        BOOL alphaFirst = [rep hasAlpha] && ([rep bitmapFormat] & NSBitmapFormatAlphaFirst) != 0;
        NSInteger firstColor = alphaFirst ? 1 : 0;
        NSInteger nColors = [rep hasAlpha] ? nSamples - 1 : nSamples;
        NSUInteger pixel[8] = {};
        for (NSInteger y = MAX((NSInteger)0, y0); y < y1; y += step) {
            for (NSInteger x = MAX((NSInteger)0, x0); x < x1; x += step) {
                [rep getPixel:pixel atX:x y:y];
                samples++;
                for (NSInteger i = 0; i < nColors && i + firstColor < 8; i++) {
                    if (pixel[i + firstColor] < kWhiteLevel) {
                        inked++;
                        break;
                    }
                }
            }
        }
    }
    NSString* detail = [NSString stringWithFormat:@"%@%@, %d of %d samples inked", file, written ? @"" : @" (not written)",
                                                   inked, samples];
    [self step:@"snapshot" ok:written && inked > 0 since:t0 detail:detail];
}

- (void)goToPageUsingField:(int)pageNo name:(NSString*)name {
    [self beginStep:name];
    double t0 = Now();
    NSString* err = [self invoke:@selector(goToPageDialog:)];
    NSTextField* field = [_host selfTestPageField];
    if (!err && !field) {
        err = @"no page number field in the toolbar";
    }
    if (err) {
        [self step:name ok:NO since:t0 detail:err];
        return;
    }
    [self enterText:[NSString stringWithFormat:@"%d", pageNo] inField:field];
    BOOL rendered = [self waitRendered];
    struct SumatraTestState s = [self state];
    BOOL ok = s.currentPage == pageNo && rendered;
    NSString* detail = [NSString stringWithFormat:@"page %d, expected %d%@", s.currentPage, pageNo,
                                                  rendered ? @"" : @", not rendered in time"];
    [self step:name ok:ok since:t0 detail:detail];
}

- (void)navigation {
    int count = [self state].pageCount;
    if (count < 2) {
        [self skip:@"navigation" detail:@"single page"];
        return;
    }
    // another case may have left this file's saved state elsewhere
    if ([self state].currentPage != 1) {
        [self invoke:@selector(goToFirstPage:)];
        [self waitRendered];
    }
    [self command:@selector(goToNextPage:)
             name:@"next page"
            check:^NSString*(struct SumatraTestState s) {
              return s.currentPage == 2 ? nil : @"not on page 2";
            }];
    [self command:@selector(goToPrevPage:)
             name:@"previous page"
            check:^NSString*(struct SumatraTestState s) {
              return s.currentPage == 1 ? nil : @"not on page 1";
            }];
    [self command:@selector(goToLastPage:)
             name:@"last page"
            check:^NSString*(struct SumatraTestState s) {
              return s.currentPage == count ? nil : @"not on the last page";
            }];
    [self command:@selector(goToFirstPage:)
             name:@"first page"
            check:^NSString*(struct SumatraTestState s) {
              return s.currentPage == 1 ? nil : @"not on page 1";
            }];
    int target = MIN(3, count);
    [self goToPageUsingField:target name:@"go to page"];
    [self command:@selector(goBack:)
             name:@"back"
            check:^NSString*(struct SumatraTestState s) {
              return s.currentPage == 1 ? nil : @"back didn't return to page 1";
            }];
}

- (void)viewModes {
    [self command:@selector(setSinglePageView:)
             name:@"single page view"
            check:^NSString*(struct SumatraTestState s) {
              return s.continuous ? @"still continuous" : nil;
            }];
    if ([self state].pageCount >= 2) {
        [self command:@selector(goToNextPage:)
                 name:@"single page: next page"
                check:^NSString*(struct SumatraTestState s) {
                  return s.currentPage == 2 ? nil : @"not on page 2";
                }];
        [self command:@selector(goToFirstPage:) name:@"single page: first page" check:nil];
    }
    [self command:@selector(setContinuousPageView:)
             name:@"continuous view"
            check:^NSString*(struct SumatraTestState s) {
              return s.continuous ? nil : @"not continuous";
            }];
}

- (void)zoomAndRotation {
    [self command:@selector(zoomActualSize:)
             name:@"actual size"
            check:^NSString*(struct SumatraTestState s) {
              return s.zoom == 1.0 ? nil : @"zoom isn't 100%";
            }];
    __block double before = [self state].displayZoom;
    [self command:@selector(zoomIn:)
             name:@"zoom in"
            check:^NSString*(struct SumatraTestState s) {
              return s.displayZoom > before ? nil : @"zoom didn't grow";
            }];
    before = [self state].displayZoom;
    [self command:@selector(zoomOut:)
             name:@"zoom out"
            check:^NSString*(struct SumatraTestState s) {
              return s.displayZoom < before ? nil : @"zoom didn't shrink";
            }];
    [self command:@selector(zoomFitWidth:)
             name:@"fit width"
            check:^NSString*(struct SumatraTestState s) {
              return s.zoom == -2.0 ? nil : @"not fit width";
            }];
    [self command:@selector(zoomFitPage:)
             name:@"fit page"
            check:^NSString*(struct SumatraTestState s) {
              return s.zoom == -1.0 ? nil : @"not fit page";
            }];
    [self command:@selector(rotateRight:)
             name:@"rotate right"
            check:^NSString*(struct SumatraTestState s) {
              return s.rotation == 90 ? nil : @"rotation isn't 90";
            }];
    [self command:@selector(rotateLeft:)
             name:@"rotate left"
            check:^NSString*(struct SumatraTestState s) {
              return s.rotation == 0 ? nil : @"rotation isn't 0";
            }];
    [self command:@selector(rotateLeft:)
             name:@"rotate left again"
            check:^NSString*(struct SumatraTestState s) {
              return s.rotation == 270 ? nil : @"rotation isn't 270";
            }];
    [self command:@selector(rotateRight:)
             name:@"rotate back"
            check:^NSString*(struct SumatraTestState s) {
              return s.rotation == 0 ? nil : @"rotation isn't 0";
            }];
}

- (void)sidebar {
    [self beginStep:@"outline"];
    double t0 = Now();
    NSString* err = [self invoke:@selector(showOutline:)];
    [self settle];
    BOOL visible = [self state].sidebarVisible;
    [self step:@"sidebar outline" ok:!err && visible since:t0 detail:err ?: (visible ? nil : @"sidebar hidden")];

    [self beginStep:@"thumbnails"];
    t0 = Now();
    err = [self invoke:@selector(showThumbnails:)];
    BOOL shown = !err && [self waitFor:^BOOL {
        return [self state].thumbnails > 0;
    }
                               seconds:_waitSeconds];
    NSString* detail = err ?: [NSString stringWithFormat:@"%d thumbnail(s) shown", [self state].thumbnails];
    [self step:@"sidebar thumbnails" ok:shown since:t0 detail:detail];

    [self beginStep:@"hide sidebar"];
    t0 = Now();
    err = [self invoke:@selector(toggleSidebar:)];
    [self settle];
    BOOL hidden = ![self state].sidebarVisible;
    [self step:@"hide sidebar" ok:!err && hidden since:t0 detail:err ?: (hidden ? nil : @"still visible")];
}

- (BOOL)waitFind {
    return [self waitFor:^BOOL {
      return ![self state].findPending;
    }
                 seconds:_waitSeconds];
}

- (void)findStep:(NSString*)name action:(SEL)action text:(NSString*)text expectPage:(int)expectPage {
    [self beginStep:name];
    double t0 = Now();
    NSString* err = [self invoke:action];
    if (!err && text) {
        NSSearchField* field = [_host selfTestSearchField];
        if (!field) {
            err = @"no search field in the toolbar";
        } else {
            [self enterText:text inField:field];
        }
    }
    if (err) {
        [self step:name ok:NO since:t0 detail:err];
        return;
    }
    BOOL done = [self waitFind];
    struct SumatraTestState s = [self state];
    NSString* problem = nil;
    if (!done) {
        problem = @"search didn't finish in time";
    } else if (s.findPage <= 0) {
        problem = @"not found";
    } else if (expectPage > 0 && s.findPage != expectPage) {
        problem = [NSString stringWithFormat:@"found on page %d, expected %d", s.findPage, expectPage];
    }
    NSString* detail = [NSString stringWithFormat:@"hit on page %d", s.findPage];
    if (problem) {
        detail = [NSString stringWithFormat:@"%@; %@", problem, detail];
    }
    [self step:name ok:problem == nil since:t0 detail:detail];
}

- (void)find:(NSString*)word page:(int)searchPage {
    if ([word length] == 0) {
        [self skip:@"find" detail:@"no search word for this document"];
        return;
    }
    [self findStep:@"find" action:@selector(findDocument:) text:word expectPage:searchPage];
    [self findStep:@"find next" action:@selector(findNext:) text:nil expectPage:0];
    [self findStep:@"find previous" action:@selector(findPrevious:) text:nil expectPage:0];
}

// First link to a page on the first pages, in page units at zoom 1, rotation 0.
- (BOOL)findPageLink:(int*)pageOut x:(double*)xOut y:(double*)yOut target:(int*)targetOut {
    void* doc = [_host documentHandle];
    int last = MIN([self state].pageCount, kLinkScanPages);
    for (int pageNo = 1; pageNo <= last; pageNo++) {
        double w = 0;
        double h = 0;
        if (!MacPageSize(doc, pageNo, &w, &h) || w <= 0 || h <= 0) {
            continue;
        }
        double step = MAX(w, h) / kLinkScanSteps;
        for (double y = step / 2; y < h; y += step) {
            for (double x = step / 2; x < w; x += step) {
                MacLink link = {};
                if (!MacLinkAtPoint(doc, pageNo, x, y, 1.0, 0, &link)) {
                    continue;
                }
                MacLinkKind kind = link.kind;
                int target = link.pageNo;
                MacFreeLink(&link);
                if (kind != MacLinkKind::Page) {
                    continue;
                }
                *pageOut = pageNo;
                *xOut = x;
                *yOut = y;
                *targetOut = target;
                return YES;
            }
        }
    }
    return NO;
}

- (SumatraPageImage*)shownPage:(int)pageNo {
    SumatraDocumentView* view = (SumatraDocumentView*)[_host selfTestDocumentView];
    for (SumatraPageImage* page in [view pages]) {
        if ([page pageNo] == pageNo) {
            return page;
        }
    }
    return nil;
}

// Clicks a link to a page with synthesized mouse events sent to the view.
- (void)followLink {
    int pageNo = 0;
    int target = 0;
    double x = 0;
    double y = 0;
    if (![self findPageLink:&pageNo x:&x y:&y target:&target]) {
        [self skip:@"follow link" detail:@"no link to a page on the first pages"];
        return;
    }
    if ([self state].currentPage != pageNo) {
        [self goToPageUsingField:pageNo name:@"go to link page"];
    }
    [self beginStep:@"follow link"];
    double t0 = Now();
    SumatraDocumentView* view = (SumatraDocumentView*)[_host selfTestDocumentView];
    SumatraPageImage* page = [self shownPage:pageNo];
    if (!page) {
        [self step:@"follow link" ok:NO since:t0 detail:@"link page not shown"];
        return;
    }
    double zoom = [page layoutZoom];
    NSPoint p = NSMakePoint(NSMinX([page frame]) + (x * zoom), NSMinY([page frame]) + (y * zoom));
    [view scrollRectToVisible:NSMakeRect(p.x - 40, p.y - 40, 80, 80)];
    [self settle];
    page = [self shownPage:pageNo];
    if (!page) {
        [self step:@"follow link" ok:NO since:t0 detail:@"link page scrolled away"];
        return;
    }
    zoom = [page layoutZoom];
    p = NSMakePoint(NSMinX([page frame]) + (x * zoom), NSMinY([page frame]) + (y * zoom));

    NSWindow* window = [_host selfTestWindow];
    NSPoint w = [view convertPoint:p toView:nil];
    NSTimeInterval ts = [[NSProcessInfo processInfo] systemUptime];
    NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                       location:w
                                  modifierFlags:0
                                      timestamp:ts
                                   windowNumber:[window windowNumber]
                                        context:nil
                                    eventNumber:0
                                     clickCount:1
                                       pressure:1.0];
    NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                     location:w
                                modifierFlags:0
                                    timestamp:ts + 0.05
                                 windowNumber:[window windowNumber]
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:0.0];
    [view mouseDown:down];
    [view mouseUp:up];
    BOOL rendered = [self waitRendered];
    struct SumatraTestState s = [self state];
    BOOL ok = s.currentPage == target && rendered;
    NSString* detail = [NSString stringWithFormat:@"link on page %d at (%.0f, %.0f) -> page %d, now on page %d", pageNo,
                                                  x, y, target, s.currentPage];
    [self step:@"follow link" ok:ok since:t0 detail:detail];
    if (target != pageNo) {
        [self command:@selector(goBack:)
                 name:@"back from link"
                check:^NSString*(struct SumatraTestState st) {
                  return st.currentPage == pageNo ? nil : @"not back on the link's page";
                }];
    }
}

- (void)selectAndCopy:(NSString*)word {
    [self beginStep:@"select all"];
    double t0 = Now();
    NSWindow* window = [_host selfTestWindow];
    [window makeFirstResponder:[_host selfTestDocumentView]];
    NSString* err = [self invoke:@selector(selectAll:)];
    [self settle];
    BOOL selected = [self state].hasSelection;
    BOOL textDoc = [word length] > 0;
    if (err || (textDoc && !selected)) {
        [self step:@"select all" ok:NO since:t0 detail:err ?: @"nothing selected"];
        return;
    }
    [self step:@"select all" ok:YES since:t0 detail:selected ? @"selected" : @"no text in this document"];
    if (!selected) {
        return;
    }

    [self beginStep:@"copy"];
    t0 = Now();
    NSPasteboard* pasteboard = [_host pasteboard];
    [pasteboard clearContents];
    err = [self invoke:@selector(copy:)];
    NSString* text = [pasteboard stringForType:NSPasteboardTypeString];
    BOOL ok = !err && [text length] > 0;
    if (ok && textDoc) {
        ok = [text rangeOfString:word options:NSCaseInsensitiveSearch].location != NSNotFound;
    }
    NSString* detail = err ?: [NSString stringWithFormat:@"%lu characters%@", (unsigned long)[text length],
                                                         ok || !textDoc ? @"" : @", search word missing"];
    [self step:@"copy" ok:ok since:t0 detail:detail];
}

// Close the tab, write and re-read the settings file, reopen: page, zoom,
// rotation and the position within the page must come back.
- (void)persistence:(NSString*)path {
    struct SumatraTestState s = [self state];
    int pageNo = MIN(2, s.pageCount);
    if (s.currentPage != pageNo) {
        [self goToPageUsingField:pageNo name:@"persistence: go to page"];
    }
    [self command:@selector(zoomActualSize:) name:@"persistence: actual size" check:nil];
    [self command:@selector(rotateRight:) name:@"persistence: rotate" check:nil];
    NSView* view = [_host selfTestDocumentView];
    NSRect visible = [view visibleRect];
    [view scrollPoint:NSMakePoint(visible.origin.x, visible.origin.y + kScrollProbe)];
    [self settle];
    struct SumatraTestState before = [self state];

    [self beginStep:@"close tab"];
    double t0 = Now();
    NSString* err = [self invoke:@selector(closeTab:)];
    BOOL closed = !err && [self waitFor:^BOOL {
        return [self state].tabCount == before.tabCount - 1;
    }
                                seconds:_waitSeconds];
    [self step:@"close tab" ok:closed since:t0 detail:err];
    if (!closed) {
        return;
    }
    [_host selfTestReloadPrefs];

    [self beginStep:@"reopen closed tab"];
    t0 = Now();
    err = [self invoke:@selector(reopenClosedTab:)];
    BOOL reopened = !err && [self waitFor:^BOOL {
        struct SumatraTestState st = [self state];
        return st.hasTab && !st.loading && st.rendered && SamePath([_host selfTestActivePath], path);
    }
                                  seconds:_waitSeconds];
    struct SumatraTestState after = [self state];
    NSMutableArray* problems = [NSMutableArray array];
    if (err) {
        [problems addObject:err];
    } else if (!reopened) {
        [problems addObject:@"not reopened and rendered in time"];
    } else {
        if (after.currentPage != before.currentPage) {
            [problems addObject:[NSString stringWithFormat:@"page %d, was %d", after.currentPage, before.currentPage]];
        }
        if (after.zoom != before.zoom) {
            [problems addObject:[NSString stringWithFormat:@"zoom %g, was %g", after.zoom, before.zoom]];
        }
        if (after.rotation != before.rotation) {
            [problems addObject:[NSString stringWithFormat:@"rotation %d, was %d", after.rotation, before.rotation]];
        }
        if (fabs(after.pageOffsetY - before.pageOffsetY) > kScrollTolerance ||
            fabs(after.pageOffsetX - before.pageOffsetX) > kScrollTolerance) {
            [problems addObject:[NSString stringWithFormat:@"offset in page (%.0f, %.0f), was (%.0f, %.0f)",
                                                           after.pageOffsetX, after.pageOffsetY, before.pageOffsetX,
                                                           before.pageOffsetY]];
        }
    }
    NSString* detail = [problems count] > 0
                           ? [problems componentsJoinedByString:@"; "]
                           : [NSString stringWithFormat:@"page %d, zoom %g, rotation %d, offset (%.0f, %.0f)",
                                                        after.currentPage, after.zoom, after.rotation,
                                                        after.pageOffsetX, after.pageOffsetY];
    [self step:@"view state restored" ok:[problems count] == 0 since:t0 detail:detail];
    if (!reopened) {
        return;
    }
    // leave the default view saved for cases that open this file again
    [self command:@selector(rotateLeft:) name:@"persistence: rotate back" check:nil];
    [self invoke:@selector(zoomFitPage:)];
    [self invoke:@selector(goToFirstPage:)];
    [self waitRendered];
}

- (void)closeAllTabs {
    [self beginStep:@"close"];
    double t0 = Now();
    int guard = 0;
    while ([self state].tabCount > 0 && guard++ < 16) {
        int count = [self state].tabCount;
        if (![self state].hasTab || [self invoke:@selector(closeTab:)]) {
            break;
        }
        [self waitFor:^BOOL {
          return [self state].tabCount < count;
        }
              seconds:_waitSeconds];
    }
    int left = [self state].tabCount;
    if (left > 0) {
        [self step:@"close" ok:NO since:t0 detail:[NSString stringWithFormat:@"%d tab(s) left open", left]];
    }
}

@end
